/**
 * launcher.cpp — home-screen app launcher. See launcher.h.
 */
#include "launcher.h"
#include <climits>
#include <lvgl.h>
#include "ble_kbd.h"
#include "calendar.h"
#include "config.h"
#include "files.h"
#include "journal.h"
#include "lexicon.h"
#include "music.h"
#include "notes.h"
#include "power.h"
#include "reader.h"
#include "reminders.h"
#include "rtc.h"
#include "settings.h"
#include "st7305.h"
#include "tasks.h"

// Custom pixel-art tile icons (src/img_icons.c, tools/make_icons.py). Each
// has a palette-inverted twin for the focused (solid-black) tile state —
// see tile_focus_cb.
extern "C" {
extern const lv_img_dsc_t img_icon_notes, img_icon_notes_inv;
extern const lv_img_dsc_t img_icon_journal, img_icon_journal_inv;
extern const lv_img_dsc_t img_icon_reader, img_icon_reader_inv;
extern const lv_img_dsc_t img_icon_calendar, img_icon_calendar_inv;
extern const lv_img_dsc_t img_icon_tasks, img_icon_tasks_inv;
extern const lv_img_dsc_t img_icon_reminders, img_icon_reminders_inv;
extern const lv_img_dsc_t img_icon_music, img_icon_music_inv;
extern const lv_img_dsc_t img_icon_lexicon, img_icon_lexicon_inv;
extern const lv_img_dsc_t img_icon_files, img_icon_files_inv;
extern const lv_img_dsc_t img_icon_settings, img_icon_settings_inv;
}

namespace {

struct App {
  const lv_img_dsc_t* icon;
  const lv_img_dsc_t* icon_inv;
  const char* name;
};

const App kApps[] = {
    {&img_icon_notes,     &img_icon_notes_inv,     "Notes"},
    {&img_icon_journal,   &img_icon_journal_inv,   "Journal"},
    {&img_icon_reader,    &img_icon_reader_inv,    "Reader"},
    {&img_icon_calendar,  &img_icon_calendar_inv,  "Calendar"},
    {&img_icon_tasks,     &img_icon_tasks_inv,     "Tasks"},
    {&img_icon_reminders, &img_icon_reminders_inv, "Reminders"},
    {&img_icon_music,     &img_icon_music_inv,     "Music"},
    {&img_icon_lexicon,   &img_icon_lexicon_inv,   "Lexicon"},
    {&img_icon_files,     &img_icon_files_inv,     "Files"},
    {&img_icon_settings,  &img_icon_settings_inv,  "Settings"},
};

lv_obj_t* g_home = nullptr;      // the launcher screen
lv_obj_t* g_tiles[12] = {};
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

  int idx = 0;
  for (int i = 0; i < g_tile_n; i++)
    if (g_tiles[i] == tile) { idx = i; break; }
  lv_img_set_src(lv_obj_get_child(tile, 0),
                 focused ? kApps[idx].icon_inv : kApps[idx].icon);
  lv_obj_set_style_text_color(lv_obj_get_child(tile, 1), fg, 0);
}

// --- arrow keys: true 2D navigation over the row-major grid ----------------
// The UI is landscape 400x300: 88px tiles + 6px gaps in a 380px grid = 4 per
// row (10 apps = rows of 4-4-2). Left/Right walk the tiles linearly (wrapping
// via the group); Up/Down jump a whole row (+-4). Down from a row with no
// tile straight below lands on the last tile; Up/Down stop at the edges.
constexpr int kGridCols = 4;

void tile_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  lv_obj_t* t = lv_event_get_target(e);
  int idx = 0;
  for (int i = 0; i < g_tile_n; i++)
    if (g_tiles[i] == t) { idx = i; break; }

  if (k == LV_KEY_RIGHT || k == LV_KEY_NEXT) {
    lv_group_focus_next(g);
  } else if (k == LV_KEY_LEFT || k == LV_KEY_PREV) {
    lv_group_focus_prev(g);
  } else if (k == LV_KEY_DOWN) {
    if (idx + kGridCols < g_tile_n)
      lv_group_focus_obj(g_tiles[idx + kGridCols]);
    else if (idx / kGridCols < (g_tile_n - 1) / kGridCols)
      lv_group_focus_obj(g_tiles[g_tile_n - 1]);  // short last row
  } else if (k == LV_KEY_UP) {
    if (idx - kGridCols >= 0) lv_group_focus_obj(g_tiles[idx - kGridCols]);
  }
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
  else if (strcmp(name, "Journal") == 0) journal_open();
  else if (strcmp(name, "Reader") == 0) reader_open();
  else if (strcmp(name, "Lexicon") == 0) lexicon_open();
  else if (strcmp(name, "Calendar") == 0) calendar_open();
  else if (strcmp(name, "Tasks") == 0) tasks_open();
  else if (strcmp(name, "Reminders") == 0) reminders_open();
  else if (strcmp(name, "Music") == 0) music_open();
  else if (strcmp(name, "Files") == 0) files_open();
  else if (strcmp(name, "Settings") == 0) settings_open();
  else open_app(name);
}

// Periodically reflect clock + battery + BLE/Wi-Fi state in the status bar.
// (Battery reading lives in power.cpp — shared with the standby dashboard.)
//
// Change-detection is essential here, not cosmetic: lv_label_set_text always
// invalidates (even for identical text), and mono_flush_cb always pushes the
// whole framebuffer (~12 ms blocking SPI) on any invalidation. Without the
// guards below, this 1 s timer flushes the entire panel every second while the
// launcher is front, even though the clock changes once a minute and the rest
// almost never. We diff each field and only touch a label when it truly
// changed. Caches are seeded to force-first-paint sentinels; the labels persist
// for the process lifetime (built once) so the statics can never go stale.
void status_timer_cb(lv_timer_t*) {
  static char last_clock[6] = "";      // "HH:MM"
  static int  last_pct      = INT_MIN;
  static int  last_ble      = -1;      // tri-state so the first tick paints
  static int  last_wifi     = -1;

  if (g_clock) {
    char dt[17];
    rtc_local_datetime(dt);            // local "YYYY-MM-DD HH:MM"
    if (strcmp(last_clock, dt + 11) != 0) {
      strncpy(last_clock, dt + 11, sizeof(last_clock) - 1);
      lv_label_set_text(g_clock, dt + 11);  // -> "HH:MM"
    }
  }
  if (g_batt) {
    const int pct = power_battery_pct();  // negative = external power / no batt
    if (pct != last_pct) {
      last_pct = pct;
      if (pct < 0) {
        lv_label_set_text(g_batt, LV_SYMBOL_CHARGE);
      } else {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", pct);
        lv_label_set_text(g_batt, b);
      }
    }
  }
  if (g_ble_icon) {
    const int ble = ble_connected() ? 1 : 0;
    if (ble != last_ble) {
      last_ble = ble;
      lv_label_set_text(g_ble_icon, ble ? LV_SYMBOL_BLUETOOTH
                                        : LV_SYMBOL_EYE_CLOSE);
    }
  }
  if (g_wifi_icon) {
    const int wifi = g_wifi_ok ? 1 : 0;
    if (wifi != last_wifi) {
      last_wifi = wifi;
      lv_label_set_text(g_wifi_icon, wifi ? LV_SYMBOL_WIFI : "");
    }
  }
}

lv_obj_t* make_tile(lv_obj_t* parent, const App& app) {
  lv_obj_t* tile = lv_obj_create(parent);
  lv_obj_set_size(tile, 88, 76);    // 3 cols x 4 rows fit under the bar
  lv_obj_set_style_radius(tile, 8, 0);
  lv_obj_set_style_border_width(tile, 1, 0);
  lv_obj_set_style_border_color(tile, lv_color_black(), 0);
  lv_obj_set_style_pad_all(tile, 6, 0);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t* icon = lv_img_create(tile);
  lv_img_set_src(icon, app.icon);

  lv_obj_t* label = lv_label_create(tile);
  lv_obj_set_style_text_font(label, &pixel_operator_16, 0);
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
  lv_obj_set_style_text_font(g_clock, &pixel_operator_bold_16, 0);
  lv_label_set_text(g_clock, "--:--");
  lv_obj_align(g_clock, LV_ALIGN_CENTER, 0, 0);

  // --- app grid ---
  lv_obj_t* grid = lv_obj_create(g_home);
  lv_obj_set_size(grid, ST7305_W, ST7305_H - 34);
  lv_obj_set_pos(grid, 0, 34);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 10, 0);
  lv_obj_set_style_pad_row(grid, 8, 0);
  lv_obj_set_style_pad_column(grid, 6, 0);
  lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  for (const App& a : kApps) g_tiles[g_tile_n++] = make_tile(grid, a);

  launcher_show();
  lv_timer_create(status_timer_cb, 1000, nullptr);
}
