/**
 * config.cpp — NVS-backed settings. See config.h.
 */
#include "config.h"
#include <Preferences.h>

namespace {
Preferences g_prefs;
constexpr const char* NS = "ghalib";
}  // namespace

bool config_get_wifi(String& ssid, String& pass) {
  g_prefs.begin(NS, true /*read-only*/);
  ssid = g_prefs.getString("ssid", "");
  pass = g_prefs.getString("pass", "");
  g_prefs.end();
  return ssid.length() > 0;
}

void config_set_wifi(const char* ssid, const char* pass) {
  g_prefs.begin(NS, false);
  g_prefs.putString("ssid", ssid);
  g_prefs.putString("pass", pass);
  g_prefs.end();
}

int config_get_tz_offset() {
  g_prefs.begin(NS, true /*read-only*/);
  const int m = g_prefs.getInt("tz", 0);
  g_prefs.end();
  return m;
}

void config_set_tz_offset(int minutes) {
  g_prefs.begin(NS, false);
  g_prefs.putInt("tz", minutes);
  g_prefs.end();
}
