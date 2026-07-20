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
#include "audio.h"
#include "launcher.h"
#include "rtc.h"
#include "st7305.h"

namespace {

constexpr uint32_t POLL_MS          = 10000;  // scheduler tick
constexpr uint32_t ALERT_TIMEOUT_MS = 60000;  // auto-dismiss an ignored alert

struct Event { int id; String dt; String title; };

// Read /events/*.txt into (id, dt, title). Mirrors calendar.cpp's storage
// format: line 1 = "YYYY-MM-DD HH:MM", line 2 = title. Kept local to stay
// decoupled from calendar.cpp (matching the deliberate split of the event store).
std::vector<Event> load_events() {
  std::vector<Event> v;
  File dir = LittleFS.open("/events");
  if (!dir || !dir.isDirectory()) return v;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String nm = f.name();
    const int slash = nm.lastIndexOf('/');
    if (slash >= 0) nm = nm.substring(slash + 1);
    if (!nm.endsWith(".txt")) continue;
    const int id = nm.substring(0, nm.length() - 4).toInt();
    String dt = f.readStringUntil('\n');
    dt.trim();
    String title = f.readStringUntil('\n');
    title.trim();
    if (title.isEmpty()) title = "(untitled)";
    if (dt.length() >= 16) v.push_back({id, dt.substring(0, 16), title});
  }
  std::sort(v.begin(), v.end(),
            [](const Event& a, const Event& b) { return a.dt < b.dt; });
  return v;
}

String now_str() {
  char dt[17];
  rtc_local_datetime(dt);  // local "YYYY-MM-DD HH:MM" (matches how events are entered)
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
  lv_refr_now(nullptr);  // paint the overlay before the (blocking) beep
  audio_beep();          // audible half of the reminder (visible + audible)
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

// --- Reminders app: upcoming list + a two-line editor ----------------------
// A reminder IS a calendar event, so the editor mirrors the Calendar editor
// (line 1 = "YYYY-MM-DD HH:MM", line 2 = title). "New reminder" is always the
// first row, so the list is never empty — which also gives Esc a focusable
// target to return Home from.
lv_obj_t* g_app_scr  = nullptr;
lv_obj_t* g_edit_scr = nullptr;
lv_obj_t* g_edit_ta  = nullptr;
int       g_edit_id  = -1;

void build_list();
void open_editor(int id, const char* seed = nullptr);  // seed=null -> load file

String event_path(int id) { return String("/events/") + id + ".txt"; }

bool valid_dt(const String& s) {
  int y, mo, d, h, mi;
  return sscanf(s.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5;
}

int next_id() {
  int mx = 0;
  for (const Event& ev : load_events()) mx = std::max(mx, ev.id);
  return mx + 1;
}

String read_event(int id) {
  File f = LittleFS.open(event_path(id), "r");
  if (!f) return String();
  String s = f.readString();
  f.close();
  return s;
}

void delete_event(int id) { LittleFS.remove(event_path(id)); }

// Parse the editor text into (dt, title); persist, or drop it if incomplete.
void save_current() {
  if (!g_edit_ta) return;
  String txt = lv_textarea_get_text(g_edit_ta);
  const int nl = txt.indexOf('\n');
  String dt    = nl < 0 ? txt : txt.substring(0, nl);
  String title = nl < 0 ? String() : txt.substring(nl + 1);
  dt.trim();
  title.trim();
  title.replace("\n", " ");
  if (title.isEmpty() || !valid_dt(dt)) { delete_event(g_edit_id); return; }
  File f = LittleFS.open(event_path(g_edit_id), "w");
  if (!f) { Serial.printf("[REM] write failed id=%d\n", g_edit_id); return; }
  f.print(dt);
  f.print('\n');
  f.print(title);
  f.close();
}

void app_teardown() {
  if (g_edit_ta)  { save_current(); g_edit_ta = nullptr; }
  if (g_edit_scr) { lv_obj_del_async(g_edit_scr); g_edit_scr = nullptr; }
  if (g_app_scr)  { lv_obj_del_async(g_app_scr);  g_app_scr = nullptr; }
  g_edit_id = -1;
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

// --- editor -----------------------------------------------------------------
void editor_close_to_list() {
  save_current();
  g_edit_ta = nullptr;
  lv_obj_t* es = g_edit_scr;
  g_edit_scr = nullptr;
  g_edit_id = -1;
  build_list();                       // reflect the edit, show the list again
  if (es) lv_obj_del_async(es);
}

void editor_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) editor_close_to_list();
}

void open_editor(int id, const char* seed) {
  g_edit_id = id;
  g_edit_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_edit_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* hint = lv_label_create(g_edit_scr);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_label_set_text(hint, "Line 1: YYYY-MM-DD HH:MM   Line 2: title   (Esc=back)");
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 8, 6);

  lv_obj_t* ta = lv_textarea_create(g_edit_scr);
  lv_obj_set_size(ta, ST7305_W, ST7305_H - 30);
  lv_obj_set_pos(ta, 0, 30);
  lv_obj_set_style_radius(ta, 0, 0);
  lv_obj_set_style_border_width(ta, 0, 0);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);  // steady cursor (reflective)
  lv_textarea_set_placeholder_text(ta, "2026-01-01 09:00\nWhat's the reminder?");
  // seed != null -> new reminder (date/time prefilled); else load the file.
  lv_textarea_set_text(ta, seed ? seed : read_event(id).c_str());
  lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
  lv_obj_add_event_cb(ta, editor_key_cb, LV_EVENT_KEY, nullptr);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);
  lv_group_add_obj(g, ta);

  g_edit_ta = ta;
  lv_scr_load(g_edit_scr);
  lv_group_focus_obj(ta);
}

// --- list rows --------------------------------------------------------------
void new_reminder_cb(lv_event_t*) {
  char now[17];
  rtc_datetime(now);                  // prefill line 1 with the current time
  String seed = String(now) + "\n";   // empty title line to fill in
  open_editor(next_id(), seed.c_str());
}

void open_reminder_cb(lv_event_t* e) {
  open_editor((int)(intptr_t)lv_event_get_user_data(e));  // seed=null -> file
}

void list_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC)
    launcher_go_home();               // -> app_teardown()
  else if (k == LV_KEY_DEL) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id >= 0) { delete_event(id); build_list(); }
  }
}

lv_obj_t* make_row(lv_obj_t* parent, const char* text, void* ud,
                   lv_event_cb_t click_cb) {
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
  lv_obj_add_event_cb(row, list_key_cb, LV_EVENT_KEY, ud);
  lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, ud);
  lv_group_add_obj(lv_group_get_default(), row);
  return row;
}

void build_list() {
  lv_obj_t* old = g_app_scr;
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

  lv_obj_t* nb = make_row(cont, LV_SYMBOL_PLUS "  New reminder",
                          (void*)(intptr_t)-1, new_reminder_cb);

  // Only future events will alert — show those as "upcoming".
  const String now = now_str();
  int shown = 0;
  for (const Event& ev : load_events()) {
    if (ev.dt < now) continue;        // already passed -> won't fire
    make_row(cont, (short_when(ev.dt) + "  " + ev.title).c_str(),
             (void*)(intptr_t)ev.id, open_reminder_cb);
    if (++shown >= 15) break;
  }
  if (!shown) {
    lv_obj_t* none = lv_label_create(cont);
    lv_label_set_text(none, "(no upcoming reminders)");
    lv_obj_set_style_text_color(none, lv_color_black(), 0);
  }

  lv_scr_load(g_app_scr);
  lv_group_focus_obj(nb);
  if (old) lv_obj_del_async(old);
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
  g_app_scr = nullptr;  // fresh entry
  build_list();
}
