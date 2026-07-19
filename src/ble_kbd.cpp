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

namespace {
constexpr uint16_t HID_SERVICE   = 0x1812;   // Human Interface Device
constexpr uint16_t HID_REPORT    = 0x2A4D;   // Report characteristic
constexpr uint16_t APPEARANCE_KEYBOARD = 0x03C1;

enum State { SCANNING, HAVE_TARGET, CONNECTING, CONNECTED, FAILED };
volatile State g_state = SCANNING;

NimBLEAddress g_target;
bool          g_have_target = false;
NimBLEClient* g_client = nullptr;

char g_status[48] = "scanning...";
char g_last_report[48] = "";

void set_status(const char* s) { strncpy(g_status, s, sizeof(g_status) - 1); }

// --- HID report notifications ---------------------------------------------
void onNotify(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool) {
  // Log raw report bytes; decoding to LVGL keys is the next iteration.
  char hex[48] = {0};
  for (size_t i = 0; i < len && i < 12; i++)
    snprintf(hex + strlen(hex), sizeof(hex) - strlen(hex), "%02X ", data[i]);
  Serial.printf("[HID] handle=%u len=%u  %s\n", chr->getHandle(),
                (unsigned)len, hex);
  snprintf(g_last_report, sizeof(g_last_report), "rpt: %s", hex);
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

  int subs = 0;
  for (auto* chr : svc->getCharacteristics(true)) {
    if (chr->getUUID() == NimBLEUUID(HID_REPORT) && chr->canNotify()) {
      if (chr->subscribe(true, onNotify)) subs++;
    }
  }
  Serial.printf("[BLE] subscribed to %d report characteristic(s)\n", subs);
  snprintf(g_status, sizeof(g_status), "CONNECTED (%d report subs)", subs);
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
