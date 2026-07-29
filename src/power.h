/**
 * power.h — M7 power management: idle -> standby dashboard -> deep sleep.
 *
 * Sleep model (PocketMage-inspired): after an idle timeout the screen becomes a
 * static glanceable dashboard (big clock, today's agenda, battery) and the
 * ESP32 deep-sleeps. A timer wake once a minute refreshes the clock through a
 * minimal boot path (display + FS + RTC only) and sleeps again; KEY/BOOT wake
 * into a normal full boot. Reminders arm the wake timer so alerts still fire
 * from deep sleep (visible + audible) on a full boot.
 */
#pragma once

// Call EARLY in setup(), after storage_init() + rtc_init() but BEFORE
// display_init(). On a silent poll timer wake (battery, nothing due, no USB)
// this re-arms and goes straight back to deep sleep without touching the
// panel — i.e. it does not return, and the screen never flashes. On a user
// (KEY/BOOT) wake, a due-reminder wake, a wake that finds USB power (-> desk
// clock via power_init), or a cold boot it returns and the full boot runs.
void power_early_boot();

// Call LAST in setup(). Replays reminders missed while asleep and starts the
// idle watchdog (standby after config_get_sleep_secs of no input).
void power_init();

// Battery percentage from the GPIO4 sense (/3 divider). Returns -1 to mean
// "external power / charging / full" (the board has no VBUS-detect pin, so a
// pinned-high reading is indistinguishable from a full cell).
int power_battery_pct();
