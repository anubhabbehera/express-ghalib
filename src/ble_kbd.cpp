/**
 * ble_kbd.cpp — NimBLE central: scan -> connect -> bond -> subscribe (M1 it.2).
 *
 * The scan runs on the NimBLE host task. When the first HID keyboard is found
 * we record its address and let ble_loop() (main task) do the blocking connect,
 * since connecting from inside a scan callback is discouraged. Every stage logs
 * to Serial so bring-up can be verified over USB.
 */
#include "ble_kbd.h"
#include <NimBLEDevice.h>
#include <lvgl.h>

namespace {
constexpr uint16_t HID_SERVICE   = 0x1812;   // Human Interface Device
constexpr uint16_t HID_REPORT    = 0x2A4D;   // Report characteristic
constexpr uint16_t APPEARANCE_KEYBOARD = 0x03C1;

// --- decoded-key ring buffer (NimBLE task -> LVGL task) --------------------
constexpr int QCAP = 32;
uint32_t g_q[QCAP];
int g_qhead = 0, g_qtail = 0;
portMUX_TYPE g_qmux = portMUX_INITIALIZER_UNLOCKED;

void enqueue_key(uint32_t k) {
  portENTER_CRITICAL(&g_qmux);
  const int nx = (g_qtail + 1) % QCAP;
  if (nx != g_qhead) { g_q[g_qtail] = k; g_qtail = nx; }
  portEXIT_CRITICAL(&g_qmux);
}

// HID keyboard usage code -> LVGL key (LV_KEY_*) or ASCII char. 0 = ignore.
uint32_t usage_to_key(uint8_t u, bool shift) {
  if (u >= 0x04 && u <= 0x1D) { char c = 'a' + (u - 0x04); return shift ? c - 32 : c; }
  if (u >= 0x1E && u <= 0x26) {
    static const char* b = "123456789";
    static const char* s = "!@#$%^&*(";
    return shift ? s[u - 0x1E] : b[u - 0x1E];
  }
  if (u == 0x27) return shift ? ')' : '0';
  switch (u) {
    case 0x28: return LV_KEY_ENTER;
    case 0x29: return LV_KEY_ESC;
    case 0x2A: return LV_KEY_BACKSPACE;
    case 0x2B: return LV_KEY_NEXT;       // Tab -> focus next
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    case 0x4C: return LV_KEY_DEL;         // Delete Forward
    case 0x4F: return LV_KEY_RIGHT;
    case 0x50: return LV_KEY_LEFT;
    case 0x51: return LV_KEY_DOWN;
    case 0x52: return LV_KEY_UP;
    case 0x4A: return LV_KEY_HOME;
    case 0x4D: return LV_KEY_END;
  }
  return 0;
}

enum State { SCANNING, HAVE_TARGET, CONNECTING, CONNECTED, FAILED };
volatile State g_state = SCANNING;

NimBLEAddress g_target;
bool          g_have_target = false;
NimBLEClient* g_client = nullptr;

char g_status[48] = "scanning...";
char g_last_report[48] = "";

void set_status(const char* s) { strncpy(g_status, s, sizeof(g_status) - 1); }

// --- NKRO bitmap keyboard report (8BitDo report-mode, 16 bytes) -----------
// Format (reverse-engineered + verified): byte[0] = HID modifier bitmap;
// byte[b>=1] is a key bitmap where set bit p => HID usage (b-1)*8 + p.
// New key-downs are detected by diffing against the previous bitmap.
void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len < 15) return;  // not the keyboard NKRO report (consumer/mouse/etc.)
  static uint8_t prev[16] = {0};
  const bool shift = (data[0] & 0x22) != 0;  // L/R shift
  const int n = len < 16 ? (int)len : 16;

  for (int b = 1; b < n; b++) {
    const uint8_t newly = data[b] & ~prev[b];  // bits that turned on
    if (!newly) continue;
    for (int p = 0; p < 8; p++) {
      if (!(newly & (1 << p))) continue;
      const uint8_t usage = (uint8_t)((b - 1) * 8 + p);
      const uint32_t k = usage_to_key(usage, shift);
      Serial.printf("[KEY] usage=0x%02X shift=%d -> key=%lu\n", usage, shift,
                    (unsigned long)k);
      if (k) {
        enqueue_key(k);
        if (k >= 0x20 && k < 0x7F)
          snprintf(g_last_report, sizeof(g_last_report), "key: '%c'", (char)k);
        else
          snprintf(g_last_report, sizeof(g_last_report), "key: 0x%02lX",
                   (unsigned long)k);
      }
    }
  }
  memcpy(prev, data, n);
}

// --- client (connection) callbacks ----------------------------------------
class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override {
    Serial.println("[BLE] connected");
  }
  void onDisconnect(NimBLEClient* c, int reason) override {
    Serial.printf("[BLE] disconnected, reason=%d -> rescanning\n", reason);
    g_state = SCANNING;
    g_have_target = false;
    set_status("disconnected; rescanning");
    NimBLEDevice::getScan()->start(0);
  }
  // Just Works pairing: accept the peer's numeric comparison.
  void onConfirmPasskey(NimBLEConnInfo& info, uint32_t pin) override {
    Serial.printf("[BLE] confirm passkey %lu (auto-accept)\n",
                  (unsigned long)pin);
    NimBLEDevice::injectConfirmPasskey(info, true);
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("[BLE] auth complete: encrypted=%d bonded=%d\n",
                  info.isEncrypted(), info.isBonded());
  }
};
ClientCB g_client_cb;

// --- scan callbacks --------------------------------------------------------
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    const bool hid =
        d->isAdvertisingService(NimBLEUUID(HID_SERVICE)) ||
        (d->haveAppearance() && d->getAppearance() == APPEARANCE_KEYBOARD);
    if (!hid) return;  // discovery is confirmed; only care about HID now

    std::string name = d->getName();
    Serial.printf("[BLE] HID device: %s  %s  rssi=%d\n",
                  name.empty() ? "(no name)" : name.c_str(),
                  d->getAddress().toString().c_str(), d->getRSSI());

    if (!g_have_target) {
      g_target = d->getAddress();
      g_have_target = true;
      g_state = HAVE_TARGET;
      NimBLEDevice::getScan()->stop();
      set_status("found HID kbd; connecting");
      Serial.printf("[BLE] target locked: %s\n", g_target.toString().c_str());
    }
  }
};
ScanCB g_scan_cb;

// Blocking connect + bond + subscribe. Runs on the main task via ble_loop().
void do_connect() {
  g_state = CONNECTING;
  Serial.printf("[BLE] connecting to %s ...\n", g_target.toString().c_str());

  g_client = NimBLEDevice::createClient();
  g_client->setClientCallbacks(&g_client_cb, false);

  if (!g_client->connect(g_target)) {
    Serial.println("[BLE] connect FAILED");
    NimBLEDevice::deleteClient(g_client);
    g_client = nullptr;
    g_state = SCANNING;
    g_have_target = false;
    set_status("connect failed; rescanning");
    NimBLEDevice::getScan()->start(0);
    return;
  }

  Serial.println("[BLE] securing (bond) ...");
  g_client->secureConnection();  // triggers pairing/bonding if required

  NimBLERemoteService* svc = g_client->getService(NimBLEUUID(HID_SERVICE));
  if (!svc) {
    Serial.println("[BLE] no HID service found after connect");
    set_status("no HID service");
    g_state = FAILED;
    return;
  }

  // The 8BitDo ignores Boot Protocol and always sends its 16-byte NKRO report
  // in report mode, so we subscribe to the report-mode chars and decode NKRO.
  int subs = 0;
  for (auto* chr : svc->getCharacteristics(true)) {
    if (chr->getUUID() == NimBLEUUID(HID_REPORT) && chr->canNotify()) {
      if (chr->subscribe(true, onNotify)) subs++;
    }
  }
  Serial.printf("[BLE] subscribed to %d report characteristic(s)\n", subs);
  snprintf(g_status, sizeof(g_status), "CONNECTED (typing ready)");
  g_state = CONNECTED;
}
}  // namespace

void ble_init() {
  NimBLEDevice::init("ghalib");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  // Bond, no MITM, LE Secure Connections; Just Works pairing (no I/O).
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&g_scan_cb, false);
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(15);
  scan->start(0);
  Serial.println("[BLE] central scan started (HID filter, bonding on)");
}

void ble_loop() {
  if (g_state == HAVE_TARGET) do_connect();
}

String ble_status_text() {
  String s = g_status;
  if (g_last_report[0]) { s += "\n"; s += g_last_report; }
  return s;
}

bool ble_kbd_pop(uint32_t* out) {
  bool ok = false;
  portENTER_CRITICAL(&g_qmux);
  if (g_qhead != g_qtail) {
    *out = g_q[g_qhead];
    g_qhead = (g_qhead + 1) % QCAP;
    ok = true;
  }
  portEXIT_CRITICAL(&g_qmux);
  return ok;
}
