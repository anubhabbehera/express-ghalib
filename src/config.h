/**
 * config.h — small persistent settings (Wi-Fi creds) in NVS (Preferences).
 */
#pragma once
#include <Arduino.h>

// Load saved Wi-Fi credentials. Returns true if an SSID is stored.
bool config_get_wifi(String& ssid, String& pass);

// Persist Wi-Fi credentials.
void config_set_wifi(const char* ssid, const char* pass);

// Local timezone offset from UTC, in minutes (e.g. +330 = IST). Default 0.
int  config_get_tz_offset();
void config_set_tz_offset(int minutes);

// Notes editor text size: 0 = small, 1 = medium, 2 = large. Default 1.
int  config_get_text_size();
void config_set_text_size(int size);
