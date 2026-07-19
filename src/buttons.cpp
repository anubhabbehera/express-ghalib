/**
 * buttons.cpp — physical KEY/BOOT buttons. See buttons.h.
 */
#include "buttons.h"
#include <Arduino.h>
#include <lvgl.h>
#include "ble_kbd.h"
#include "launcher.h"

namespace {
constexpr int PIN_BOOT = 0;    // BOOT button (active-low)
constexpr int PIN_KEY  = 18;   // KEY button  (active-low)
constexpr uint32_t DEBOUNCE_MS = 40;

struct Btn {
  int pin;
  bool prev_up;       // last stable level (true = released/HIGH)
  uint32_t last_ms;
};
Btn g_boot{PIN_BOOT, true, 0};
Btn g_key{PIN_KEY, true, 0};

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
}  // namespace

void buttons_init() {
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_KEY, INPUT_PULLUP);
}

void buttons_poll() {
  if (pressed(g_key)) {
    Serial.println("[BTN] KEY -> Back (Esc)");
    ble_kbd_inject(LV_KEY_ESC);
  }
  if (pressed(g_boot)) {
    Serial.println("[BTN] BOOT -> Home");
    launcher_go_home();
  }
}
