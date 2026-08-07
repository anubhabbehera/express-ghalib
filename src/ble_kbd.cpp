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
// Appearance is <10-bit category><6-bit subcategory>; category 15 = HID, which
// covers keyboard (0x03C1), keypad, and the generic 0x03C0 some kbds report.
constexpr uint16_t APPEARANCE_CAT_HID = 15;

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

void set_status(const char* s) { strncpy(g_status, s, sizeof(g_status) - 1); }

// --- pairing mode ----------------------------------------------------------
// A long-press opens pairing (main task sets the flag); ble_loop() drops the
// active link and rescans, collecting EVERY HID accessory in range into
// g_pair_list for the overlay. The mode has no deadline — it stays open until
// ble_kbd_stop_pairing() — so the list keeps filling while the user watches.
// Once no new accessory has appeared for PAIR_SETTLE_MS we bond the strongest
// one: without a working keyboard, proximity is the only selector available.
constexpr uint32_t PAIR_SETTLE_MS = 2500;
constexpr uint32_t PAIR_CONNECT_MS = 4000;  // connect leash while pairing
constexpr int      PAIR_NEAR_RSSI  = -55;   // "held against the board"
constexpr int      PAIR_MAX       = 8;

struct PairDev {
  NimBLEAddress addr;
  char          name[24];
  int           rssi;
  bool          hid;      // advertised as HID (preferred when picking a target)
  bool          failed;   // connect already failed; skip when picking a target
};
PairDev      g_pair_list[PAIR_MAX];
int          g_pair_count = 0;
portMUX_TYPE g_pairmux = portMUX_INITIALIZER_UNLOCKED;

volatile bool g_pair_request = false;  // set by ble_kbd_start_pairing()
volatile bool g_pair_stop    = false;  // set by ble_kbd_stop_pairing()
volatile bool g_pairing      = false;  // true while pairing mode is open
volatile uint32_t g_pair_settle = 0;   // millis() when the list is "settled"
NimBLEAddress g_pair_target;           // accessory being bonded (UI marker)
bool          g_pair_have_target = false;

// Record/refresh an accessory in the pairing list; true if newly added.
// `name` is null/empty when the device hasn't given one yet. Runs on the NimBLE
// host task.
bool pair_note(const NimBLEAddress& addr, const char* name, int rssi, bool hid) {
  bool added = false;
  portENTER_CRITICAL(&g_pairmux);
  int i = 0;
  for (; i < g_pair_count; i++)
    if (g_pair_list[i].addr == addr) break;
  if (i == g_pair_count) {
    if (g_pair_count >= PAIR_MAX) { portEXIT_CRITICAL(&g_pairmux); return false; }
    g_pair_count++;
    g_pair_list[i].failed = false;
    g_pair_list[i].hid    = false;
    g_pair_list[i].name[0] = 0;
    added = true;
  }
  g_pair_list[i].addr = addr;
  g_pair_list[i].rssi = rssi;
  // The name and the HID hint often arrive in different packets (advert vs
  // scan response), so accumulate rather than overwrite — a later nameless
  // advert must not erase a name we already learned.
  if (hid) g_pair_list[i].hid = true;
  if (name && *name) {
    strncpy(g_pair_list[i].name, name, sizeof(g_pair_list[i].name) - 1);
    g_pair_list[i].name[sizeof(g_pair_list[i].name) - 1] = 0;
  }
  portEXIT_CRITICAL(&g_pairmux);
  // A newly seen accessory restarts the settle clock so a device that shows up
  // late still gets to compete on RSSI. Repeat adverts (the duplicate filter is
  // off while pairing) only refresh the signal reading.
  if (added) g_pair_settle = millis() + PAIR_SETTLE_MS;
  return added;
}

// Serial-only "have I logged this address yet" set, so the pairing log can
// cover every advertiser (the panel list is filtered and only holds PAIR_MAX).
// Runs on the NimBLE host task; reset whenever pairing opens.
constexpr int DBG_MAX = 48;
NimBLEAddress g_dbg_seen[DBG_MAX];
int           g_dbg_count = 0;

bool dbg_first_sighting(const NimBLEAddress& addr) {
  for (int i = 0; i < g_dbg_count; i++)
    if (g_dbg_seen[i] == addr) return false;
  if (g_dbg_count >= DBG_MAX) return false;
  g_dbg_seen[g_dbg_count++] = addr;
  return true;
}

void pair_mark_failed(const NimBLEAddress& addr) {
  portENTER_CRITICAL(&g_pairmux);
  for (int i = 0; i < g_pair_count; i++)
    if (g_pair_list[i].addr == addr) { g_pair_list[i].failed = true; break; }
  portEXIT_CRITICAL(&g_pairmux);
}

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
    // "Looks like HID" from the advert alone. Note this is a HINT, not a test:
    // plenty of keyboards put neither the HID UUID nor an appearance in the
    // advertisement (the HID service only shows up in the GATT table after you
    // connect), which is exactly why they never appeared in the old scan.
    const bool hid =
        d->isAdvertisingService(NimBLEUUID(HID_SERVICE)) ||
        (d->haveAppearance() && (d->getAppearance() >> 6) == APPEARANCE_CAT_HID);

    std::string name = d->getName();
    const char* nm = name.empty() ? "(unnamed)" : name.c_str();

    // Pairing: list every NAMED device plus anything advertising HID, so a
    // keyboard that hides its HID UUID still shows up and can be picked. The
    // real HID check happens after connecting (do_connect drops non-keyboards).
    if (g_pairing) {
      // Serial gets EVERY advertiser, named or not — when a keyboard fails to
      // show up on the panel this log is what says whether it is advertising at
      // all (nothing here = not a BLE device, or not in pairing mode). First
      // sightings only: the duplicate filter is off while pairing, so every
      // device re-reports several times a second.
      if (dbg_first_sighting(d->getAddress()))
        Serial.printf("[BLE] adv: %-20s %s rssi=%d hid=%d appear=0x%04X\n", nm,
                      d->getAddress().toString().c_str(), d->getRSSI(), (int)hid,
                      d->haveAppearance() ? d->getAppearance() : 0);

      if (!hid && name.empty()) return;   // unnamed non-HID: noise on the panel
      pair_note(d->getAddress(), name.c_str(), d->getRSSI(), hid);
      return;  // the target is chosen in ble_loop() once the list settles
    }

    if (!hid) return;  // outside pairing, auto-connect only to advertised HID

    Serial.printf("[BLE] HID device: %s  %s  rssi=%d\n", nm,
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
//
// NOTE: connect() blocks this task, and with it lv_timer_handler() and
// buttons_poll(). NimBLE's 30 s default is far too long for the pairing screen
// — a candidate that never answers would freeze the UI (and the KEY that closes
// the screen) for half a minute — so pairing attempts get a short leash.
void do_connect() {
  g_state = CONNECTING;
  Serial.printf("[BLE] connecting to %s ...\n", g_target.toString().c_str());

  g_client = NimBLEDevice::createClient();
  g_client->setClientCallbacks(&g_client_cb, false);
  if (g_pairing) g_client->setConnectTimeout(PAIR_CONNECT_MS);

  if (!g_client->connect(g_target)) {
    Serial.println("[BLE] connect FAILED");
    NimBLEDevice::deleteClient(g_client);
    g_client = nullptr;
    g_state = SCANNING;
    g_have_target = false;
    if (g_pairing) {
      // Stay on the pairing screen: blacklist this one, keep listing, and let
      // the settle timer pick the next-strongest candidate.
      pair_mark_failed(g_target);
      g_pair_have_target = false;
      g_pair_settle = millis() + PAIR_SETTLE_MS;
      set_status("pair failed; still scanning");
    } else {
      set_status("connect failed; rescanning");
    }
    NimBLEDevice::getScan()->start(0);
    return;
  }

  Serial.println("[BLE] securing (bond) ...");
  g_client->secureConnection();  // triggers pairing/bonding if required

  NimBLERemoteService* svc = g_client->getService(NimBLEUUID(HID_SERVICE));
  if (!svc) {
    Serial.println("[BLE] no HID service found after connect");
    if (g_pairing) {  // not a keyboard after all — drop it and keep listing
      pair_mark_failed(g_target);
      g_pair_have_target = false;
      g_client->disconnect();
      NimBLEDevice::deleteClient(g_client);
      g_client = nullptr;
      g_have_target = false;
      g_state = SCANNING;
      g_pair_settle = millis() + PAIR_SETTLE_MS;
      set_status("not a keyboard; still scanning");
      NimBLEDevice::getScan()->start(0);
      return;
    }
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
  if (g_pairing) {
    // Stay on the pairing screen so the user sees the result and dismisses it
    // themselves; no point scanning further now that we have a keyboard.
    NimBLEDevice::getScan()->stop();
    snprintf(g_status, sizeof(g_status), "paired! KEY to close");
  } else {
    snprintf(g_status, sizeof(g_status), "CONNECTED (typing ready)");
  }
  g_state = CONNECTED;
}

// Open pairing mode: tear down any active link and restart the scan in
// list-everything mode. Runs on the main task.
//
// Bonds are NOT wiped here (the old behaviour) — escaping the screen would then
// leave you with no keyboard at all. The stale bond for the accessory we
// actually pick is deleted in pair_connect_best(), just before reconnecting.
void enter_pairing() {
  Serial.println("[BLE] pairing screen opened: scanning for accessories");
  NimBLEDevice::getScan()->stop();

  if (g_client) {
    if (g_client->isConnected()) g_client->disconnect();
    NimBLEDevice::deleteClient(g_client);
    g_client = nullptr;
  }

  portENTER_CRITICAL(&g_pairmux);
  g_pair_count = 0;
  portEXIT_CRITICAL(&g_pairmux);
  g_dbg_count = 0;  // log every advertiser afresh for this pairing session

  g_have_target      = false;
  g_pair_have_target = false;
  g_state       = SCANNING;
  g_pairing     = true;
  g_pair_settle = millis() + PAIR_SETTLE_MS;
  set_status("searching for accessories...");

  // Duplicate filtering off: repeat adverts are what keep the listed RSSI live.
  // Stop again first — disconnecting above fires onDisconnect, which restarts
  // the scan on the host task, and start() is a no-op while one is running (so
  // the filter change would silently not apply).
  NimBLEDevice::getScan()->stop();
  NimBLEDevice::getScan()->setDuplicateFilter(false);
  NimBLEDevice::getScan()->start(0);
}

// Close pairing mode (KEY pressed again). A link made while pairing is kept;
// otherwise the normal first-seen auto-connect scan resumes.
void exit_pairing() {
  g_pairing = false;
  g_pair_have_target = false;
  Serial.println("[BLE] pairing screen closed");

  NimBLEDevice::getScan()->stop();
  NimBLEDevice::getScan()->setDuplicateFilter(true);
  if (g_state == CONNECTED) return;  // keep the keyboard we just bonded

  g_have_target = false;
  g_state = SCANNING;
  set_status("scanning...");
  NimBLEDevice::getScan()->start(0);
}

// The list has settled: bond the best candidate that hasn't already failed.
// Proximity is the selector — there is no keyboard to pick with.
//
// Candidates are devices that advertised HID, plus anything held right against
// the board (>= PAIR_NEAR_RSSI): a keyboard that reveals its HID service only
// after connecting still needs a way in, and do_connect() blacklists whatever
// turns out not to be a keyboard. Everything else is listed but never dialled —
// silently connecting to a stranger's phone is not ours to do, and each attempt
// blocks the UI for up to PAIR_CONNECT_MS.
void pair_connect_best() {
  int  best = -1;
  char name[24] = "";
  NimBLEAddress addr;
  portENTER_CRITICAL(&g_pairmux);
  for (int i = 0; i < g_pair_count; i++) {
    const PairDev& c = g_pair_list[i];
    if (c.failed || (!c.hid && c.rssi < PAIR_NEAR_RSSI)) continue;
    if (best < 0) { best = i; continue; }
    const PairDev& b = g_pair_list[best];
    if (c.hid != b.hid ? c.hid : c.rssi > b.rssi) best = i;
  }
  if (best >= 0) {
    addr = g_pair_list[best].addr;
    strncpy(name, g_pair_list[best].name, sizeof(name) - 1);
    if (!name[0]) strncpy(name, "(unnamed)", sizeof(name) - 1);
  }
  portEXIT_CRITICAL(&g_pairmux);
  if (best < 0) {  // every candidate failed; keep listing, re-check later
    g_pair_settle = millis() + PAIR_SETTLE_MS;
    return;
  }

  NimBLEDevice::getScan()->stop();
  NimBLEDevice::deleteBond(addr);  // the "forget" — re-bond from scratch

  g_pair_target      = addr;
  g_pair_have_target = true;
  g_target       = addr;
  g_have_target  = true;
  g_state        = HAVE_TARGET;
  snprintf(g_status, sizeof(g_status), "pairing %s...", name);
  Serial.printf("[BLE] pairing target: %s (%s)\n", name,
                addr.toString().c_str());
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

  if (g_pair_request) { g_pair_request = false; g_pair_stop = false; enter_pairing(); }
  if (g_pair_stop)    { g_pair_stop = false; if (g_pairing) exit_pairing(); }
  // Pairing mode never times out; once the list stops growing, bond the
  // strongest candidate. Skipped while a connect is already in flight/up.
  if (g_pairing && g_state == SCANNING && g_pair_count > 0 &&
      (int32_t)(millis() - g_pair_settle) >= 0)
    pair_connect_best();
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

void ble_kbd_stop_pairing() { g_pair_request = false; g_pair_stop = true; }

// Reports the *requested* state, not just the applied one, so the overlay opens
// and closes on the button press instead of a loop iteration later.
bool ble_kbd_pairing() {
  if (g_pair_stop) return false;
  return g_pairing || g_pair_request;
}

int ble_kbd_pair_results(BleFoundKbd* out, int max) {
  if (!out || max <= 0) return 0;
  PairDev tmp[PAIR_MAX];
  portENTER_CRITICAL(&g_pairmux);
  const int n = g_pair_count;
  for (int i = 0; i < n; i++) tmp[i] = g_pair_list[i];
  portEXIT_CRITICAL(&g_pairmux);

  // Strongest first (insertion sort — n <= PAIR_MAX).
  for (int i = 1; i < n; i++) {
    const PairDev k = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j].rssi < k.rssi) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = k;
  }

  const int cnt = n < max ? n : max;
  for (int i = 0; i < cnt; i++) {
    strncpy(out[i].name, tmp[i].name[0] ? tmp[i].name : "(unnamed)",
            sizeof(out[i].name) - 1);
    out[i].name[sizeof(out[i].name) - 1] = 0;
    out[i].rssi    = tmp[i].rssi;
    out[i].hid     = tmp[i].hid;
    out[i].current = g_pair_have_target && tmp[i].addr == g_pair_target;
  }
  return cnt;
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
