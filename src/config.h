/**
 * config.h — small persistent settings (Wi-Fi creds) in NVS (Preferences).
 */
#pragma once
#include <Arduino.h>

// Load saved Wi-Fi credentials. Returns true if an SSID is stored.
bool config_get_wifi(String& ssid, String& pass);

// Persist Wi-Fi credentials.
void config_set_wifi(const char* ssid, const char* pass);
