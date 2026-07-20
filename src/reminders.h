/**
 * reminders.h — reminder scheduler. Polls the RTC against calendar events
 * (/events/<id>.txt, line 1 = "YYYY-MM-DD HH:MM") and raises an on-screen alert
 * when one comes due. Runs in the background regardless of the active screen.
 *
 * M3.5 scope: software polling (the device never sleeps), visual alert only.
 * The ES8311 audible beep is a follow-up commit (needs the I2S pinout).
 */
#pragma once

// Start the background scheduler (registers the poll timer). Call once at boot,
// after rtc_init() + storage_init() + LVGL are up.
void reminders_init();

// Open the Reminders app: a read-only list of upcoming reminders.
void reminders_open();

// True while a reminder alert overlay is showing (for the KEY-button dismiss).
bool reminders_alert_active();

// Dismiss the current alert overlay (from the physical KEY button or on Home).
void reminders_dismiss();
