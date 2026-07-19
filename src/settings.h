/**
 * settings.h — Settings app. For now: Wi-Fi setup + NTP time sync to the RTC.
 * Entered from the launcher's Settings tile.
 */
#pragma once

void settings_open();

// Try saved Wi-Fi creds (if any) and NTP-sync the RTC. Blocking, UI-pumped.
// Safe to call at boot; returns quickly if no creds are stored.
void settings_boot_sync();
