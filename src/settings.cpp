/**
 * settings.cpp — Settings app: Wi-Fi setup + NTP -> RTC. See settings.h.
 *
 * Flow: launcher -> Wi-Fi scan list -> password entry -> connect + NTP sync.
 * Time is stored as UTC for now (local-timezone offset is a later refinement).
 */
#include "settings.h"
#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include <time.h>
#include "config.h"
#include "launcher.h"
#include "rtc.h"
#include "st7305.h"

namespace {

lv_obj_t* g_menu_scr = nullptr;   // Settings home: Wi-Fi | Time zone
lv_obj_t* g_scan_scr = nullptr;
lv_obj_t* g_pass_scr = nullptr;
lv_obj_t* g_tz_scr   = nullptr;   // timezone entry screen
lv_obj_t* g_tz_ta    = nullptr;
lv_obj_t* g_status   = nullptr;   // status label on the password screen
lv_obj_t* g_pass_ta  = nullptr;
char      g_ssid[40] = {0};

void build_scan();  // fwd
void build_menu();  // fwd
void build_tz();    // fwd

void set_status(const String& s) {
  if (g_status) lv_label_set_text(g_status, s.c_str());
}

// --- row helper (explicit mono styling + inverted focus) -------------------
void row_focus_cb(lv_event_t* e) {
  lv_obj_t* row = lv_event_get_target(e);
  const bool f = lv_event_get_code(e) == LV_EVENT_FOCUSED;
  lv_obj_set_style_bg_color(row, f ? lv_color_black() : lv_color_white(), 0);
  lv_obj_set_style_bg_opa(row, f ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(row); i++)
    lv_obj_set_style_text_color(lv_obj_get_child(row, i),
                                f ? lv_color_white() : lv_color_black(), 0);
}

lv_obj_t* make_row(lv_obj_t* parent, const char* text, void* ud,
                   lv_event_cb_t click_cb, lv_event_cb_t key_cb) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, 40);
  lv_obj_set_style_radius(row, 0, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(row, lv_color_black(), 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_pad_left(row, 10, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* lbl = lv_label_create(row);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_add_event_cb(row, row_focus_cb, LV_EVENT_FOCUSED, nullptr);
  lv_obj_add_event_cb(row, row_focus_cb, LV_EVENT_DEFOCUSED, nullptr);
  lv_obj_add_event_cb(row, key_cb, LV_EVENT_KEY, ud);
  lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, ud);
  lv_group_add_obj(lv_group_get_default(), row);
  return row;
}

// --- connect + NTP ---------------------------------------------------------
bool wifi_connect_and_sync(const char* ssid, const char* pass) {
  set_status("Connecting...");
  lv_refr_now(nullptr);  // paint the status before we block

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(30);
    lv_timer_handler();  // keep the UI alive during the wait
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] connect failed");
    set_status("Wi-Fi failed");
    return false;
  }

  set_status("Getting time (NTP)...");
  lv_refr_now(nullptr);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");  // UTC
  struct tm tm;
  if (!getLocalTime(&tm, 10000)) {
    set_status("Connected, NTP failed");
    return false;
  }
  rtc_set(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
          tm.tm_sec);
  config_set_wifi(ssid, pass);
  char dt[20];
  rtc_datetime(dt);
  set_status(String("Synced (UTC): ") + dt);
  Serial.printf("[wifi] connected, RTC synced: %s\n", dt);
  return true;
}

// --- teardown (returning to launcher) --------------------------------------
void settings_teardown() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  if (g_menu_scr) { lv_obj_del_async(g_menu_scr); g_menu_scr = nullptr; }
  if (g_pass_scr) { lv_obj_del_async(g_pass_scr); g_pass_scr = nullptr; }
  if (g_scan_scr) { lv_obj_del_async(g_scan_scr); g_scan_scr = nullptr; }
  if (g_tz_scr)   { lv_obj_del_async(g_tz_scr);   g_tz_scr = nullptr; }
  g_status = g_pass_ta = g_tz_ta = nullptr;
}

// --- password screen -------------------------------------------------------
void back_to_scan() {
  lv_obj_t* old = g_pass_scr;
  g_pass_scr = nullptr;
  g_status = g_pass_ta = nullptr;
  build_scan();
  if (old) lv_obj_del_async(old);
}

// Deferred so the Enter key-release settles on the password field first, rather
// than leaking a click onto the home screen's focused tile (lv_timer_handler is
// non-reentrant, so we can't drain it synchronously inside the Enter event).
void go_home_cb(lv_timer_t*) { launcher_go_home(); }

void connect_cb(lv_event_t*) {
  if (wifi_connect_and_sync(g_ssid, lv_textarea_get_text(g_pass_ta))) {
    launcher_set_wifi_ok(true);
    lv_timer_t* t = lv_timer_create(go_home_cb, 1000, nullptr);
    lv_timer_set_repeat_count(t, 1);  // one-shot; auto-deletes after firing
  }
}

void pass_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) back_to_scan();
}

void build_password() {
  g_pass_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_pass_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_pass_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_label_set_text_fmt(title, "%s", g_ssid);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* ta = lv_textarea_create(g_pass_scr);
  lv_obj_set_width(ta, ST7305_W - 24);
  lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 48);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_password_mode(ta, true);
  lv_textarea_set_placeholder_text(ta, "password");
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);
  // Single focusable field: Enter submits (LV_EVENT_READY on a one-line
  // textarea), Esc cancels. No Tab-to-button (the textarea swallows Tab).
  lv_obj_add_event_cb(ta, pass_key_cb, LV_EVENT_KEY, nullptr);    // Esc = back
  lv_obj_add_event_cb(ta, connect_cb, LV_EVENT_READY, nullptr);   // Enter = connect
  lv_group_add_obj(g, ta);
  g_pass_ta = ta;

  lv_obj_t* hint = lv_label_create(g_pass_scr);
  lv_label_set_text(hint, LV_SYMBOL_WIFI "  Enter = connect & sync   (Esc = back)");
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 92);

  g_status = lv_label_create(g_pass_scr);
  lv_label_set_text(g_status, "");
  lv_obj_set_width(g_status, ST7305_W - 24);
  lv_obj_set_width(g_status, ST7305_W - 24);
  lv_obj_align(g_status, LV_ALIGN_TOP_MID, 0, 150);

  lv_scr_load(g_pass_scr);
  lv_group_focus_obj(ta);
}

// --- scan / SSID list ------------------------------------------------------
void ssid_click_cb(lv_event_t* e) {
  const char* ssid = static_cast<const char*>(lv_event_get_user_data(e));
  strncpy(g_ssid, ssid, sizeof(g_ssid) - 1);
  g_ssid[sizeof(g_ssid) - 1] = 0;
  lv_obj_t* old = g_scan_scr;
  g_scan_scr = nullptr;
  build_password();
  if (old) lv_obj_del_async(old);
}

void ssid_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC) {           // back to the Settings menu
    lv_obj_t* old = g_scan_scr;
    g_scan_scr = nullptr;
    build_menu();
    if (old) lv_obj_del_async(old);
  }
}

// SSID strings must outlive the row; keep them here.
String g_scan_ssids[16];

void build_scan() {
  g_scan_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_scan_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_scan_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  char dt[20];
  rtc_datetime(dt);
  lv_label_set_text_fmt(title, "Wi-Fi   %s", dt);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* cont = lv_obj_create(g_scan_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 36);
  lv_obj_set_pos(cont, 0, 36);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);
  lv_scr_load(g_scan_scr);

  // Synchronous scan (a few seconds). Show a placeholder first.
  lv_obj_t* scanning = lv_label_create(cont);
  lv_label_set_text(scanning, "scanning...");
  lv_refr_now(nullptr);

  WiFi.mode(WIFI_STA);
  const int n = WiFi.scanNetworks();
  lv_obj_del(scanning);
  Serial.printf("[wifi] scan found %d networks\n", n);

  const int show = n < 16 ? n : 16;
  lv_obj_t* first = nullptr;
  for (int i = 0; i < show; i++) {
    g_scan_ssids[i] = WiFi.SSID(i);
    const bool lock = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    String label = String(lock ? LV_SYMBOL_CLOSE " " : "  ") + g_scan_ssids[i] +
                   "  " + WiFi.RSSI(i) + "dBm";
    lv_obj_t* row = make_row(cont, label.c_str(), (void*)g_scan_ssids[i].c_str(),
                             ssid_click_cb, ssid_key_cb);
    if (!first) first = row;
  }
  if (!show) {
    lv_obj_t* none = lv_label_create(cont);
    lv_label_set_text(none, "no networks (Esc = back)");
  }
  if (first) lv_group_focus_obj(first);
}

// --- timezone entry --------------------------------------------------------
int parse_tz(const char* s) {
  while (*s == ' ') s++;
  int sign = 1;
  if (*s == '+') s++;
  else if (*s == '-') { sign = -1; s++; }
  int hh = 0, mm = 0;
  sscanf(s, "%d:%d", &hh, &mm);          // "5:30" or bare "5"
  return sign * (hh * 60 + mm);
}

void tz_back_to_menu() {
  lv_obj_t* old = g_tz_scr;
  g_tz_scr = nullptr;
  g_tz_ta = nullptr;
  build_menu();
  if (old) lv_obj_del_async(old);
}

// Deferred so the Enter key-release settles on the tz field first, rather than
// leaking a click onto the menu's focused row (same fix as the Wi-Fi connect).
void tz_deferred_back(lv_timer_t*) { tz_back_to_menu(); }

void tz_save_cb(lv_event_t*) {           // Enter on the one-line field
  if (g_tz_ta) config_set_tz_offset(parse_tz(lv_textarea_get_text(g_tz_ta)));
  lv_timer_t* t = lv_timer_create(tz_deferred_back, 60, nullptr);
  lv_timer_set_repeat_count(t, 1);       // one-shot; auto-deletes after firing
}
void tz_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) tz_back_to_menu();
}

void build_tz() {
  g_tz_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_tz_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_tz_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_label_set_text(title, "Time zone");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* ta = lv_textarea_create(g_tz_scr);
  lv_obj_set_width(ta, ST7305_W - 24);
  lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 48);
  lv_textarea_set_one_line(ta, true);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);
  char cur[16];
  const int m = config_get_tz_offset(), am = m < 0 ? -m : m;
  snprintf(cur, sizeof(cur), "%c%d:%02d", m < 0 ? '-' : '+', am / 60, am % 60);
  lv_textarea_set_text(ta, cur);
  lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
  lv_obj_add_event_cb(ta, tz_key_cb, LV_EVENT_KEY, nullptr);      // Esc = back
  lv_obj_add_event_cb(ta, tz_save_cb, LV_EVENT_READY, nullptr);   // Enter = save
  lv_group_add_obj(g, ta);
  g_tz_ta = ta;

  lv_obj_t* hint = lv_label_create(g_tz_scr);
  lv_label_set_text(hint, "Offset from UTC, e.g. +5:30 or -8:00\nEnter = save   Esc = back");
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 92);

  lv_scr_load(g_tz_scr);
  lv_group_focus_obj(ta);
}

// --- Settings menu (Wi-Fi | Time zone) -------------------------------------
void menu_wifi_cb(lv_event_t*) {
  lv_obj_t* old = g_menu_scr; g_menu_scr = nullptr;
  build_scan();
  if (old) lv_obj_del_async(old);
}
void menu_tz_cb(lv_event_t*) {
  lv_obj_t* old = g_menu_scr; g_menu_scr = nullptr;
  build_tz();
  if (old) lv_obj_del_async(old);
}
// Sleep-timeout row: Enter cycles Off -> 1m -> 2m -> 5m -> 10m in place.
constexpr int kSleepPresets[] = {0, 60, 120, 300, 600};
void sleep_row_text(char* out, size_t n) {
  const int s = config_get_sleep_secs();
  if (s <= 0)
    snprintf(out, n, LV_SYMBOL_POWER "  Sleep after   Off");
  else
    snprintf(out, n, LV_SYMBOL_POWER "  Sleep after   %dm", s / 60);
}
void menu_sleep_cb(lv_event_t* e) {
  const int cur = config_get_sleep_secs();
  int idx = 0;
  for (int i = 0; i < 5; i++)
    if (kSleepPresets[i] == cur) idx = i;
  config_set_sleep_secs(kSleepPresets[(idx + 1) % 5]);
  char txt[40];
  sleep_row_text(txt, sizeof txt);
  lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), txt);
}

void menu_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC)
    launcher_go_home();
}

void build_menu() {
  g_menu_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_menu_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_menu_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_label_set_text(title, "Settings");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* cont = lv_obj_create(g_menu_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 36);
  lv_obj_set_pos(cont, 0, 36);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* r1 = make_row(cont, LV_SYMBOL_WIFI "  Wi-Fi setup", nullptr,
                          menu_wifi_cb, menu_key_cb);
  char tz[28];
  const int m = config_get_tz_offset(), am = m < 0 ? -m : m;
  snprintf(tz, sizeof(tz), LV_SYMBOL_SETTINGS "  Time zone   %c%d:%02d",
           m < 0 ? '-' : '+', am / 60, am % 60);
  make_row(cont, tz, nullptr, menu_tz_cb, menu_key_cb);
  char sl[40];
  sleep_row_text(sl, sizeof sl);
  make_row(cont, sl, nullptr, menu_sleep_cb, menu_key_cb);

  lv_scr_load(g_menu_scr);
  lv_group_focus_obj(r1);
}

}  // namespace

void settings_open() {
  launcher_set_leave_hook(settings_teardown);
  g_menu_scr = g_scan_scr = g_pass_scr = g_tz_scr = nullptr;
  build_menu();
}

void settings_boot_sync() {
  String ssid, pass;
  if (!config_get_wifi(ssid, pass)) return;  // nothing saved
  Serial.printf("[wifi] boot: trying saved SSID '%s'\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    delay(50);
    lv_timer_handler();
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    struct tm tm;
    if (getLocalTime(&tm, 8000))
      rtc_set(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
              tm.tm_min, tm.tm_sec);
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}
