/**
 * ble_kbd.h — BLE HID (HOGP) keyboard host. M1 iteration 2:
 * scan -> connect -> bond -> subscribe to HID reports (logged).
 *
 * Discovery is confirmed working (8BitDo Retro advertises as HID, appearance
 * 0x03C1). This stage connects to the first HID keyboard found and subscribes
 * to its report notifications. HID-report -> LVGL key mapping comes next.
 */
#pragma once
#include <Arduino.h>

// Configure NimBLE as a central (with bonding) and start scanning.
void ble_init();

// Drive the connection state machine. Call from the main loop().
void ble_loop();

// One-line status for the panel (state + last report).
String ble_status_text();

// True once a keyboard is connected + subscribed.
bool ble_connected();

// Pop the next decoded key as an LVGL key code (LV_KEY_* or an ASCII char).
// Returns false if the queue is empty. Safe to call from the LVGL task.
bool ble_kbd_pop(uint32_t* key_out);

// Inject a key into the input queue from another source (e.g. physical buttons).
void ble_kbd_inject(uint32_t lvgl_key);

// One accessory seen by the pairing scan (UI snapshot).
struct BleFoundKbd {
  char name[24];
  int  rssi;
  bool hid;       // advertised as a HID device (tried first)
  bool current;   // the one we are bonding to / bonded to
};

// Open pairing mode: drop the active link and rescan, listing every HID
// accessory in range. There is NO timeout — the mode stays open until
// ble_kbd_stop_pairing(), so the list keeps filling while the user watches.
// Once the scan settles the strongest accessory is bonded automatically (with
// no working keyboard, proximity is the only selector we have). Safe to call
// from the main task (e.g. a long-press handler); the work runs in ble_loop.
void ble_kbd_start_pairing();

// Leave pairing mode (KEY pressed again). Any link made while pairing is kept;
// otherwise the normal first-seen auto-connect scan resumes.
void ble_kbd_stop_pairing();

// True while pairing mode is open (for the overlay + re-entrancy guards).
bool ble_kbd_pairing();

// Snapshot the accessories found so far, strongest first. Returns how many
// entries were written (at most `max`).
int ble_kbd_pair_results(BleFoundKbd* out, int max);
