/**
 * launcher.h — home-screen app launcher.
 *
 * A grid of monochrome icon tiles (Notes/Calendar/Reminders/Music/Settings),
 * navigated with the BLE keyboard's arrow keys + Enter, in the Orion/e-ink PDA
 * spirit. The focused tile inverts (black fill, white glyph) for clear feedback
 * on the 1-bit reflective panel. Esc returns from a stub app to the home grid.
 */
#pragma once

// Build the launcher on a fresh screen and load it. Tiles are added to the
// default LVGL group so the keypad indev drives navigation.
void launcher_build();

// Jump back to the home grid from anywhere (e.g. a physical Home button); frees
// the current app screen if it isn't the home screen.
void launcher_go_home();

// Register a teardown hook the launcher runs (once) when returning home. Apps
// set this on entry to free their screens/timers safely after home is shown.
void launcher_set_leave_hook(void (*fn)());

// Update the status-bar Wi-Fi indicator (true once Wi-Fi is set up / synced).
void launcher_set_wifi_ok(bool ok);
