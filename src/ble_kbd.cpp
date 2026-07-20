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

// Drop all queued keys. Used on repeat-key release so buffered repeats (the
// display flush drains slower than they're generated) don't over-run after
// you lift off.
void flush_queue() {
  portENTER_CRITICAL(&g_qmux);
  g_qhead = g_qtail = 0;
  portEXIT_CRITICAL(&g_qmux);
}

// --- key auto-repeat (held key) --------------------------------------------
// The keyboard only sends a report on change, so a held key produces no further
// notifications — we synthesise repeats in ble_loop() from the last held key,
// with backspace/del accelerating so a long hold deletes fast.
constexpr uint32_t REPEAT_DELAY_MS = 400;   // hold this long before repeating
constexpr uint32_t REPEAT_BASE_MS  = 90;    // steady repeat interval
constexpr uint32_t REPEAT_MIN_MS   = 25;    // fastest (accelerated) interval
volatile uint32_t g_held_key   = 0;         // decoded key currently held (0 = none)
uint8_t           g_held_usage = 0;         // its HID usage (to detect release)
volatile uint32_t g_next_repeat = 0;
int               g_repeat_n = 0;

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

// --- re-pairing mode -------------------------------------------------------
// A long-press requests pairing (main task sets the flag); ble_loop() performs
// the disconnect + forget-bonds, then scans a fixed window and picks the HID
// with the strongest RSSI so proximity selects the intended keyboard.
constexpr uint32_t PAIR_WINDOW_MS = 3000;
volatile bool g_pair_request = false;  // set by ble_kbd_start_pairing()
volatile bool g_pairing      = false;  // true during the RSSI window
uint32_t      g_pair_deadline = 0;     // millis() when the window closes
NimBLEAddress g_pair_best;             // strongest-RSSI HID so far
bool          g_pair_have_best = false;
int           g_pair_best_rssi = -128;
char          g_pair_best_name[32] = "";

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
        // Arm auto-repeat for this key (Esc excluded — holding it must not
        // repeatedly exit screens). Last new key-down wins.
        if (k != LV_KEY_ESC) {
          g_held_usage  = usage;
          g_held_key    = k;
          g_next_repeat = millis() + REPEAT_DELAY_MS;
          g_repeat_n    = 0;
        }
        if (k >= 0x20 && k < 0x7F)
          snprintf(g_last_report, sizeof(g_last_report), "key: '%c'", (char)k);
        else
          snprintf(g_last_report, sizeof(g_last_report), "key: 0x%02lX",
                   (unsigned long)k);
      }
    }
  }
  // Stop repeating once the held key's bit clears (released or replaced). If it
  // had been repeating, flush the queued repeats so it stops the instant you
  // lift off instead of draining a backlog (e.g. extra deletes after backspace).
  if (g_held_key) {
    const int hb = (g_held_usage / 8) + 1;
    const bool still = hb < n && (data[hb] & (1 << (g_held_usage % 8)));
    if (!still) {
      if (g_repeat_n > 0) flush_queue();
      g_held_key = 0;
      g_repeat_n = 0;
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

    // Re-pairing: don't lock the first HID — collect the whole window and keep
    // the strongest signal, so bringing the new keyboard close selects it.
    if (g_pairing) {
      const int rssi = d->getRSSI();
      if (!g_pair_have_best || rssi > g_pair_best_rssi) {
        g_pair_best      = d->getAddress();
        g_pair_best_rssi = rssi;
        g_pair_have_best = true;
        strncpy(g_pair_best_name, name.empty() ? "keyboard" : name.c_str(),
                sizeof(g_pair_best_name) - 1);
        g_pair_best_name[sizeof(g_pair_best_name) - 1] = 0;
        snprintf(g_status, sizeof(g_status), "pairing: found %s",
                 g_pair_best_name);
      }
      return;  // decision is made in ble_loop() when the window closes
    }

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

// Begin a re-pairing window: tear down any active link, forget ALL bonds, and
// restart the scan in RSSI-collection mode. Runs on the main task.
void enter_pairing() {
  Serial.println("[BLE] re-pairing requested: forgetting bonds, rescanning");
  NimBLEDevice::getScan()->stop();

  if (g_client) {
    if (g_client->isConnected()) g_client->disconnect();
    NimBLEDevice::deleteClient(g_client);
    g_client = nullptr;
  }
  NimBLEDevice::deleteAllBonds();  // the missing "forget" — start clean

  g_have_target    = false;
  g_pair_have_best = false;
  g_pair_best_rssi = -128;
  g_pair_best_name[0] = 0;
  g_state       = SCANNING;
  g_pairing     = true;
  g_pair_deadline = millis() + PAIR_WINDOW_MS;
  set_status("pairing: bring kbd close...");

  NimBLEDevice::getScan()->start(0);
}

// Close the RSSI window: connect the strongest candidate, or fall back to the
// normal first-seen auto-scan if nothing showed up.
void finish_pairing() {
  g_pairing = false;
  if (g_pair_have_best) {
    g_target       = g_pair_best;
    g_have_target  = true;
    g_state        = HAVE_TARGET;
    NimBLEDevice::getScan()->stop();
    snprintf(g_status, sizeof(g_status), "pairing %s...", g_pair_best_name);
    Serial.printf("[BLE] pairing target: %s (rssi=%d)\n",
                  g_pair_best.toString().c_str(), g_pair_best_rssi);
  } else {
    set_status("no keyboard found; scanning");
    Serial.println("[BLE] pairing window empty; back to normal scan");
    // Scan is still running; onResult will resume first-seen auto-connect.
  }
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
  // Auto-repeat the held key; backspace/del accelerate the longer you hold.
  if (g_held_key && (int32_t)(millis() - g_next_repeat) >= 0) {
    enqueue_key(g_held_key);
    g_repeat_n++;
    uint32_t iv = REPEAT_BASE_MS;
    if (g_held_key == LV_KEY_BACKSPACE || g_held_key == LV_KEY_DEL) {
      const int fast = (int)REPEAT_BASE_MS - g_repeat_n * 10;
      iv = fast < (int)REPEAT_MIN_MS ? REPEAT_MIN_MS : fast;
    }
    g_next_repeat = millis() + iv;
  }

  if (g_pair_request) { g_pair_request = false; enter_pairing(); }
  if (g_pairing && (int32_t)(millis() - g_pair_deadline) >= 0) finish_pairing();
  if (g_state == HAVE_TARGET) do_connect();
}

String ble_status_text() {
  String s = g_status;
  if (g_last_report[0]) { s += "\n"; s += g_last_report; }
  return s;
}

bool ble_connected() { return g_state == CONNECTED; }

void ble_kbd_inject(uint32_t k) { enqueue_key(k); }

void ble_kbd_start_pairing() { g_pair_request = true; }

bool ble_kbd_pairing() { return g_pairing || g_pair_request; }

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
