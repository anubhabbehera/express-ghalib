/**
 * launcher.cpp — home-screen app launcher. See launcher.h.
 */
#include "launcher.h"
#include <lvgl.h>
#include "ble_kbd.h"
#include "calendar.h"
#include "config.h"
#include "music.h"
#include "notes.h"
#include "power.h"
#include "reminders.h"
#include "rtc.h"
#include "settings.h"
#include "st7305.h"
#include "tasks.h"

namespace {

struct App {
  const char* icon;   // LV_SYMBOL_*
  const char* name;
};

// Icons are LVGL's built-in vector glyphs — crisp on a 1-bit panel, no assets.
const App kApps[] = {
    {LV_SYMBOL_EDIT,     "Notes"},
    {LV_SYMBOL_LIST,     "Calendar"},
    {LV_SYMBOL_OK,       "Tasks"},
    {LV_SYMBOL_BELL,     "Reminders"},
    {LV_SYMBOL_AUDIO,    "Music"},
    {LV_SYMBOL_SETTINGS, "Settings"},
};

lv_obj_t* g_home = nullptr;      // the launcher screen
lv_obj_t* g_tiles[8] = {};
int       g_tile_n = 0;
lv_obj_t* g_ble_icon  = nullptr;
lv_obj_t* g_wifi_icon = nullptr;
lv_obj_t* g_clock     = nullptr;   // status-bar HH:MM (from RTC)
lv_obj_t* g_batt      = nullptr;   // status-bar battery %
bool      g_wifi_ok   = false;

lv_obj_t* g_app_scr = nullptr;   // a stub app screen, if open
void (*g_leave)() = nullptr;     // teardown hook for whatever app is open

// --- focus feedback: invert the focused tile (black fill, white glyph) ------
void tile_focus_cb(lv_event_t* e) {
  lv_obj_t* tile = lv_event_get_target(e);
  const bool focused = lv_event_get_code(e) == LV_EVENT_FOCUSED;
  const lv_color_t bg = focused ? lv_color_black() : lv_color_white();
  const lv_color_t fg = focused ? lv_color_white() : lv_color_black();
  lv_obj_set_style_bg_color(tile, bg, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(tile); i++)
    lv_obj_set_style_text_color(lv_obj_get_child(tile, i), fg, 0);
}

// --- arrow keys move focus across the grid ---------------------------------
void tile_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_RIGHT || k == LV_KEY_DOWN || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_LEFT || k == LV_KEY_UP || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
}

// --- stub app screen (Calendar/Reminders/Music/Settings for now) -----------
void stub_leave() {
  if (g_app_scr) { lv_obj_del_async(g_app_scr); g_app_scr = nullptr; }
}

void app_back_cb(lv_event_t* e) {
  const bool esc = lv_event_get_code(e) == LV_EVENT_KEY &&
                   lv_event_get_key(e) == LV_KEY_ESC;
  if (esc || lv_event_get_code(e) == LV_EVENT_CLICKED) launcher_go_home();
}

void open_app(const char* name) {
  lv_obj_t* app = lv_obj_create(nullptr);
  g_app_scr = app;
  launcher_set_leave_hook(stub_leave);

  lv_obj_t* title = lv_label_create(app);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_label_set_text(title, name);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -24);

  lv_obj_t* hint = lv_label_create(app);
  lv_label_set_text(hint, "coming soon");
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 8);

  lv_obj_t* back = lv_btn_create(app);
  lv_obj_t* bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back");
  lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_group_remove_all_objs(lv_group_get_default());
  lv_group_add_obj(lv_group_get_default(), back);
  lv_obj_add_event_cb(back, app_back_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(back, app_back_cb, LV_EVENT_KEY, nullptr);

  lv_scr_load(app);
  lv_group_focus_obj(back);
}

void tile_click_cb(lv_event_t* e) {
  const char* name = static_cast<const char*>(lv_event_get_user_data(e));
  if (strcmp(name, "Notes") == 0) notes_open();
  else if (strcmp(name, "Calendar") == 0) calendar_open();
  else if (strcmp(name, "Tasks") == 0) tasks_open();
  else if (strcmp(name, "Reminders") == 0) reminders_open();
  else if (strcmp(name, "Music") == 0) music_open();
  else if (strcmp(name, "Settings") == 0) settings_open();
  else open_app(name);
}

// Periodically reflect clock + battery + BLE/Wi-Fi state in the status bar.
// (Battery reading lives in power.cpp — shared with the standby dashboard.)
// Thanks to direct_mode each of these is a cheap partial redraw, not a full one.
void status_timer_cb(lv_timer_t*) {
  if (g_clock) {
    char dt[17];
    rtc_local_datetime(dt);               // local "YYYY-MM-DD HH:MM"
    lv_label_set_text(g_clock, dt + 11);  // -> "HH:MM"
  }
  if (g_batt) {
    const int pct = power_battery_pct();
    if (pct < 0) {
      lv_label_set_text(g_batt, LV_SYMBOL_CHARGE);  // external power / no battery
    } else {
      char b[8];
      snprintf(b, sizeof(b), "%d%%", pct);
      lv_label_set_text(g_batt, b);
    }
  }
  if (g_ble_icon)
    lv_label_set_text(g_ble_icon, ble_connected() ? LV_SYMBOL_BLUETOOTH
                                                   : LV_SYMBOL_EYE_CLOSE);
  if (g_wifi_icon)
    lv_label_set_text(g_wifi_icon, g_wifi_ok ? LV_SYMBOL_WIFI : "");
}

lv_obj_t* make_tile(lv_obj_t* parent, const App& app) {
  lv_obj_t* tile = lv_obj_create(parent);
  lv_obj_set_size(tile, 112, 100);
  lv_obj_set_style_radius(tile, 8, 0);
  lv_obj_set_style_border_width(tile, 1, 0);
  lv_obj_set_style_border_color(tile, lv_color_black(), 0);
  lv_obj_set_style_pad_all(tile, 6, 0);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t* icon = lv_label_create(tile);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
  lv_label_set_text(icon, app.icon);

  lv_obj_t* label = lv_label_create(tile);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  lv_label_set_text(label, app.name);

  lv_obj_add_event_cb(tile, tile_focus_cb, LV_EVENT_FOCUSED, nullptr);
  lv_obj_add_event_cb(tile, tile_focus_cb, LV_EVENT_DEFOCUSED, nullptr);
  lv_obj_add_event_cb(tile, tile_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED,
                      const_cast<char*>(app.name));
  return tile;
}

// (Re)populate the input group with the launcher tiles and show the home grid.
void launcher_show() {
  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);
  for (int i = 0; i < g_tile_n; i++) lv_group_add_obj(g, g_tiles[i]);
  lv_scr_load(g_home);
  if (g_tile_n) lv_group_focus_obj(g_tiles[0]);
}

}  // namespace

void launcher_set_leave_hook(void (*fn)()) { g_leave = fn; }

void launcher_set_wifi_ok(bool ok) { g_wifi_ok = ok; }

void launcher_go_home() {
  void (*f)() = g_leave;
  g_leave = nullptr;
  launcher_show();     // home is active again
  if (f) f();          // now safe to tear down the app's screens
}

void launcher_build() {
  g_home = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_home, LV_OBJ_FLAG_SCROLLABLE);

  // --- status bar ---
  lv_obj_t* bar = lv_obj_create(g_home);
  lv_obj_set_size(bar, ST7305_W, 34);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_hor(bar, 12, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* wordmark = lv_label_create(bar);
  lv_label_set_text(wordmark, "express-ghalib");
  lv_obj_align(wordmark, LV_ALIGN_LEFT_MID, 0, 0);

  g_ble_icon = lv_label_create(bar);
  lv_label_set_text(g_ble_icon, LV_SYMBOL_EYE_CLOSE);
  lv_obj_align(g_ble_icon, LV_ALIGN_RIGHT_MID, 0, 0);

  g_wifi_icon = lv_label_create(bar);
  String s, p;
  g_wifi_ok = config_get_wifi(s, p);  // "set up" if creds are stored
  lv_label_set_text(g_wifi_icon, g_wifi_ok ? LV_SYMBOL_WIFI : "");
  lv_obj_align(g_wifi_icon, LV_ALIGN_RIGHT_MID, -28, 0);

  g_batt = lv_label_create(bar);      // battery % from GPIO4 (see power.cpp)
  lv_label_set_text(g_batt, "");
  lv_obj_align(g_batt, LV_ALIGN_RIGHT_MID, -56, 0);

  g_clock = lv_label_create(bar);     // live HH:MM from the RTC
  lv_obj_set_style_text_font(g_clock, &lv_font_montserrat_20, 0);
  lv_label_set_text(g_clock, "--:--");
  lv_obj_align(g_clock, LV_ALIGN_CENTER, 0, 0);

  // --- app grid ---
  lv_obj_t* grid = lv_obj_create(g_home);
  lv_obj_set_size(grid, ST7305_W, ST7305_H - 34);
  lv_obj_set_pos(grid, 0, 34);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 12, 0);
  lv_obj_set_style_pad_row(grid, 10, 0);
  lv_obj_set_style_pad_column(grid, 10, 0);
  lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  for (const App& a : kApps) g_tiles[g_tile_n++] = make_tile(grid, a);

  launcher_show();
  lv_timer_create(status_timer_cb, 1000, nullptr);
}
