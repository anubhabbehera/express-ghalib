/**
 * reminders.cpp — reminder scheduler + alert overlay. See reminders.h.
 *
 * The scheduler is a background lv_timer (10 s). Each tick it reads the current
 * time from the RTC ("YYYY-MM-DD HH:MM") and edge-triggers any calendar event
 * whose start crosses from future to due: an event fires exactly once, when
 *   g_last_check < event_dt <= now.
 * Because the timestamps are fixed-width ISO-ish strings, lexical order equals
 * chronological order, so a plain string compare is the whole test — no parsing,
 * no persistent fired-set, and events already past at boot never fire.
 *
 * The alert is a modal on lv_layer_top() so it floats over whatever app is open.
 * It is dismissed by the physical KEY button (works with no keyboard) or auto-
 * dismisses after ALERT_TIMEOUT_MS.
 */
#include "reminders.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <lvgl.h>
#include <algorithm>
#include <vector>
#include "launcher.h"
#include "rtc.h"
#include "st7305.h"

namespace {

constexpr uint32_t POLL_MS          = 10000;  // scheduler tick
constexpr uint32_t ALERT_TIMEOUT_MS = 60000;  // auto-dismiss an ignored alert

struct Event { String dt; String title; };

// Read /events/*.txt into (dt, title). Mirrors calendar.cpp's storage format:
// line 1 = "YYYY-MM-DD HH:MM", line 2 = title. Kept local to stay decoupled.
std::vector<Event> load_events() {
  std::vector<Event> v;
  File dir = LittleFS.open("/events");
  if (!dir || !dir.isDirectory()) return v;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String nm = f.name();
    if (!nm.endsWith(".txt")) continue;
    String dt = f.readStringUntil('\n');
    dt.trim();
    String title = f.readStringUntil('\n');
    title.trim();
    if (title.isEmpty()) title = "(untitled)";
    if (dt.length() >= 16) v.push_back({dt.substring(0, 16), title});
  }
  std::sort(v.begin(), v.end(),
            [](const Event& a, const Event& b) { return a.dt < b.dt; });
  return v;
}

String now_str() {
  char dt[17];
  rtc_datetime(dt);  // "YYYY-MM-DD HH:MM"
  return String(dt);
}

// Short display form "MM-DD HH:MM" from "YYYY-MM-DD HH:MM".
String short_when(const String& dt) {
  return dt.length() >= 16 ? dt.substring(5, 16) : dt;
}

// --- alert overlay (lv_layer_top) ------------------------------------------
lv_obj_t* g_alert_box   = nullptr;
lv_obj_t* g_alert_label = nullptr;
uint32_t  g_alert_deadline = 0;

void raise_alert(const String& when, const String& title) {
  if (!g_alert_box) {
    g_alert_box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_alert_box, ST7305_W - 32, 150);
    lv_obj_center(g_alert_box);
    lv_obj_set_style_bg_color(g_alert_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_alert_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_alert_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(g_alert_box, 3, 0);
    lv_obj_set_style_radius(g_alert_box, 6, 0);
    lv_obj_clear_flag(g_alert_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hdr = lv_label_create(g_alert_box);
    lv_label_set_text(hdr, LV_SYMBOL_BELL "  Reminder");
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);

    g_alert_label = lv_label_create(g_alert_box);
    lv_obj_set_width(g_alert_label, ST7305_W - 64);
    lv_label_set_long_mode(g_alert_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g_alert_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_alert_label, LV_ALIGN_CENTER, 0, 4);

    lv_obj_t* hint = lv_label_create(g_alert_box);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "KEY = dismiss");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
  }
  lv_label_set_text(g_alert_label, (short_when(when) + "\n" + title).c_str());
  g_alert_deadline = millis() + ALERT_TIMEOUT_MS;
  Serial.printf("[REM] alert: %s  %s\n", when.c_str(), title.c_str());
}

// --- scheduler --------------------------------------------------------------
String g_last_check;  // "now" at the previous tick; edge-trigger boundary

void poll_cb(lv_timer_t*) {
  // Auto-dismiss an alert the user ignored.
  if (g_alert_box && (int32_t)(millis() - g_alert_deadline) >= 0)
    reminders_dismiss();

  const String now = now_str();
  if (g_last_check.isEmpty()) { g_last_check = now; return; }  // baseline only

  for (const Event& ev : load_events())
    if (g_last_check < ev.dt && ev.dt <= now) raise_alert(ev.dt, ev.title);

  g_last_check = now;
}

// --- Reminders app: read-only list of upcoming events ----------------------
lv_obj_t* g_app_scr = nullptr;

void app_teardown() {
  if (g_app_scr) { lv_obj_del_async(g_app_scr); g_app_scr = nullptr; }
}

void row_focus_cb(lv_event_t* e) {
  lv_obj_t* row = lv_event_get_target(e);
  const bool f = lv_event_get_code(e) == LV_EVENT_FOCUSED;
  lv_obj_set_style_bg_color(row, f ? lv_color_black() : lv_color_white(), 0);
  lv_obj_set_style_bg_opa(row, f ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(row); i++)
    lv_obj_set_style_text_color(lv_obj_get_child(row, i),
                                f ? lv_color_white() : lv_color_black(), 0);
}

void app_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC)
    launcher_go_home();  // -> app_teardown()
}

lv_obj_t* make_row(lv_obj_t* parent, const char* text) {
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
  lv_obj_add_event_cb(row, app_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(lv_group_get_default(), row);
  return row;
}

}  // namespace

void reminders_init() {
  g_last_check = "";  // first tick captures the baseline "now"
  lv_timer_create(poll_cb, POLL_MS, nullptr);
  Serial.println("[REM] scheduler started (poll every 10s)");
}

bool reminders_alert_active() { return g_alert_box != nullptr; }

void reminders_dismiss() {
  if (!g_alert_box) return;
  lv_obj_del(g_alert_box);
  g_alert_box = g_alert_label = nullptr;
}

void reminders_open() {
  launcher_set_leave_hook(app_teardown);
  g_app_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_app_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_app_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_label_set_text(title, "Reminders");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* cont = lv_obj_create(g_app_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 36);
  lv_obj_set_pos(cont, 0, 36);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  // Only events still in the future will alert — show those as "upcoming".
  const String now = now_str();
  lv_obj_t* first = nullptr;
  int shown = 0;
  for (const Event& ev : load_events()) {
    if (ev.dt < now) continue;  // already passed -> won't fire
    lv_obj_t* row = make_row(cont, (short_when(ev.dt) + "  " + ev.title).c_str());
    if (!first) first = row;
    if (++shown >= 16) break;
  }
  if (!shown) {
    lv_obj_t* none = lv_label_create(cont);
    lv_label_set_text(none, "No upcoming reminders  (Esc = back)");
    lv_obj_set_style_text_color(none, lv_color_black(), 0);
  }

  lv_scr_load(g_app_scr);
  if (first) lv_group_focus_obj(first);
}
