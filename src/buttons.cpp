/**
 * buttons.cpp — physical KEY/BOOT buttons. See buttons.h.
 */
#include "buttons.h"
#include <Arduino.h>
#include <lvgl.h>
#include "ble_kbd.h"
#include "launcher.h"
#include "reminders.h"

namespace {
constexpr int PIN_BOOT = 0;    // BOOT button (active-low)
constexpr int PIN_KEY  = 18;   // KEY button  (active-low)
constexpr uint32_t DEBOUNCE_MS  = 40;
constexpr uint32_t LONGPRESS_MS = 1500;  // KEY held this long -> BLE pairing

struct Btn {
  int pin;
  bool prev_up;       // last stable level (true = released/HIGH)
  uint32_t last_ms;
};
Btn g_boot{PIN_BOOT, true, 0};

// Returns true once on each debounced press (falling edge, active-low).
bool pressed(Btn& b) {
  const bool up = digitalRead(b.pin);  // HIGH = released
  const uint32_t now = millis();
  bool fired = false;
  if (b.prev_up && !up && (now - b.last_ms) > DEBOUNCE_MS) {
    fired = true;
    b.last_ms = now;
  }
  b.prev_up = up;
  return fired;
}

// KEY is press-duration aware: a short tap = Back (fired on release), a long
// hold = enter BLE pairing mode (fired once at the threshold, while still held).
// Back moves from the press-edge to the release-edge so a long hold never also
// leaks an Esc. This is the only way to re-pair when the keyboard itself is dead.
struct HoldBtn {
  int pin;
  bool prev_up;
  uint32_t down_ms;   // when the current press began
  bool long_fired;    // long-press action already fired for this press
};
HoldBtn g_key{PIN_KEY, true, 0, false};

// Runs KEY's state machine. Returns true once when a short tap completes.
bool key_poll(bool& out_long) {
  const bool up = digitalRead(g_key.pin);  // HIGH = released
  const uint32_t now = millis();
  out_long = false;
  bool short_tap = false;

  if (g_key.prev_up && !up) {              // press begins
    g_key.down_ms = now;
    g_key.long_fired = false;
  } else if (!g_key.prev_up && !up) {      // still held
    if (!g_key.long_fired && (now - g_key.down_ms) >= LONGPRESS_MS) {
      g_key.long_fired = true;
      out_long = true;                     // fire pairing once
    }
  } else if (!g_key.prev_up && up) {       // released
    if (!g_key.long_fired && (now - g_key.down_ms) > DEBOUNCE_MS)
      short_tap = true;                    // debounced tap -> Back
  }
  g_key.prev_up = up;
  return short_tap;
}
}  // namespace

void buttons_init() {
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_KEY, INPUT_PULLUP);
}

void buttons_poll() {
  bool key_long = false;
  const bool key_tap = key_poll(key_long);
  // Physical buttons don't pass through an LVGL indev on every path (BOOT goes
  // straight to launcher_go_home), so count them as activity for the idle
  // watchdog explicitly.
  if (key_tap || key_long || !digitalRead(PIN_BOOT))
    lv_disp_trig_activity(nullptr);
  if (key_long && !ble_kbd_pairing()) {
    Serial.println("[BTN] KEY (hold) -> BLE pairing mode");
    ble_kbd_start_pairing();
  } else if (key_tap) {
    if (reminders_alert_active()) {  // a reminder is showing -> KEY dismisses it
      Serial.println("[BTN] KEY -> dismiss reminder");
      reminders_dismiss();
    } else {
      Serial.println("[BTN] KEY -> Back (Esc)");
      ble_kbd_inject(LV_KEY_ESC);
    }
  }
  if (pressed(g_boot)) {
    Serial.println("[BTN] BOOT -> Home");
    reminders_dismiss();  // clear any alert on the way home (no-op if none)
    launcher_go_home();
  }
}
