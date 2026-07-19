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
