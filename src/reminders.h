/**
 * reminders.h — reminder scheduler. Polls the RTC against calendar events
 * (/events/<id>.txt, line 1 = "YYYY-MM-DD HH:MM") and raises an on-screen alert
 * when one comes due. Runs in the background regardless of the active screen.
 *
 * M3.5 scope: software polling (the device never sleeps), visual alert only.
 * The ES8311 audible beep is a follow-up commit (needs the I2S pinout).
 */
#pragma once
#include <Arduino.h>

// Start the background scheduler (registers the poll timer). Call once at boot,
// after rtc_init() + storage_init() + LVGL are up.
void reminders_init();

// --- power-management hooks (see power.cpp) --------------------------------
// Pretend the last scheduler tick happened at `local_dt` ("YYYY-MM-DD HH:MM"),
// so events that came due while the device was deep-sleeping fire on the next
// tick. Call after reminders_init().
void reminders_seed_baseline(const char* local_dt);

// Run one scheduler tick immediately (instead of waiting for the poll timer).
void reminders_check_now();

// Earliest upcoming event/snooze datetime strictly after now ("" if none).
String reminders_next_dt();

// True if any event came due in (since, now] — decides whether a timer wake
// needs the full boot (alert + beep) or was just a dashboard clock tick.
bool reminders_due_since(const char* since);

// True while an in-memory snooze is outstanding (blocks deep sleep: snoozes
// don't survive a reboot, and they're at most a few minutes out).
bool reminders_snooze_pending();

// Fill dts/titles with up to `max` upcoming events (earliest first); returns
// the count. Used by the standby dashboard's agenda.
int reminders_upcoming(String* dts, String* titles, int max);

// Open the Reminders app: a read-only list of upcoming reminders.
void reminders_open();

// True while a reminder alert overlay is showing (for the KEY-button dismiss).
bool reminders_alert_active();

// Dismiss the current alert overlay (from the physical KEY button or on Home).
void reminders_dismiss();
