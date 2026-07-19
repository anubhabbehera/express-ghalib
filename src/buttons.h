/**
 * buttons.h — the two physical side buttons.
 *   KEY (GPIO18)  -> Back  (injects LV_KEY_ESC into the input queue)
 *   BOOT (GPIO0)  -> Home  (jumps to the launcher grid)
 * Both are active-low; polled with simple debounce.
 */
#pragma once

void buttons_init();
void buttons_poll();  // call from the main loop()
