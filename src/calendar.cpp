/**
 * calendar.cpp — Calendar app (agenda list + event editor) backed by LittleFS.
 * See calendar.h.
 *
 * Navigation: launcher -> agenda -> editor. The physical Home button and the
 * agenda's Esc both return to the launcher via launcher_go_home(), which runs
 * calendar_teardown() (registered as the leave hook) to save + free screens.
 *
 * An event is stored as /events/<id>.txt: line 1 = "YYYY-MM-DD HH:MM" (sorts
 * chronologically as plain text), line 2+ = title. The editor is a two-line
 * textarea (date/time, then title), reusing the Notes-editor approach — this
 * avoids the multi-field Tab-navigation trouble a form would bring on a keypad.
 */
#include "calendar.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <lvgl.h>
#include <algorithm>
#include <vector>
#include "launcher.h"
#include "rtc.h"
#include "st7305.h"

namespace {

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
struct EventMeta { int id; String dt; String title; };

String event_path(int id) { return String("/events/") + id + ".txt"; }

// True if s looks like "YYYY-MM-DD HH:MM".
bool valid_dt(const String& s) {
  int y, mo, d, h, mi;
  return sscanf(s.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5;
}

std::vector<EventMeta> list_events() {
  std::vector<EventMeta> v;
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
    if (title.length() > 28) title = title.substring(0, 28) + "...";
    if (title.isEmpty()) title = "(untitled)";
    v.push_back({id, dt, title});
  }
  // Chronological: ISO-ish date strings sort correctly as text; id breaks ties.
  std::sort(v.begin(), v.end(), [](const EventMeta& a, const EventMeta& b) {
    return a.dt != b.dt ? a.dt < b.dt : a.id < b.id;
  });
  return v;
}

int next_id() {
  int mx = 0;
  File dir = LittleFS.open("/events");
  if (dir && dir.isDirectory())
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String nm = f.name();
      const int slash = nm.lastIndexOf('/');
      if (slash >= 0) nm = nm.substring(slash + 1);
      if (nm.endsWith(".txt")) mx = std::max(mx, (int)nm.toInt());
    }
  return mx + 1;
}

String read_event(int id) {
  File f = LittleFS.open(event_path(id), "r");
  if (!f) return String();
  String s = f.readString();
  f.close();
  return s;
}

void write_event(int id, const String& dt, const String& title) {
  File f = LittleFS.open(event_path(id), "w");
  if (!f) { Serial.printf("[cal] write failed id=%d\n", id); return; }
  f.print(dt);
  f.print('\n');
  f.print(title);
  f.close();
}

void delete_event(int id) { LittleFS.remove(event_path(id)); }

// Short display form "MM-DD HH:MM" from a "YYYY-MM-DD HH:MM" start string.
String short_when(const String& dt) {
  if (dt.length() >= 16) return dt.substring(5, 16);  // drop "YYYY-"
  return dt;
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
lv_obj_t* g_list_scr = nullptr;
lv_obj_t* g_edit_scr = nullptr;
lv_obj_t* g_edit_ta  = nullptr;
int       g_edit_id  = -1;

void build_list();
void open_editor(int id, const char* seed = nullptr);  // seed=null -> read file

// Parse the editor's text into (dt, title); persist, or drop if incomplete.
void save_current() {
  if (!g_edit_ta) return;
  String txt = lv_textarea_get_text(g_edit_ta);
  const int nl = txt.indexOf('\n');
  String dt    = nl < 0 ? txt : txt.substring(0, nl);
  String title = nl < 0 ? String() : txt.substring(nl + 1);
  dt.trim();
  title.trim();
  title.replace("\n", " ");  // collapse any extra lines into the title
  if (title.isEmpty() || !valid_dt(dt)) delete_event(g_edit_id);  // drop drafts
  else write_event(g_edit_id, dt, title);
}

// Runs when returning to the launcher (Home button / agenda Esc).
void calendar_teardown() {
  if (g_edit_ta)  { save_current(); g_edit_ta = nullptr; }
  if (g_edit_scr) { lv_obj_del_async(g_edit_scr); g_edit_scr = nullptr; }
  if (g_list_scr) { lv_obj_del_async(g_list_scr); g_list_scr = nullptr; }
  g_edit_id = -1;
}

// --- editor ----------------------------------------------------------------
void editor_close_to_list() {
  save_current();
  g_edit_ta = nullptr;
  lv_obj_t* es = g_edit_scr;
  g_edit_scr = nullptr;
  g_edit_id = -1;
  build_list();                       // rebuild agenda (reflects edits), shows it
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
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);  // steady (no blink) cursor
  lv_textarea_set_placeholder_text(ta, "2026-01-01 09:00\nWhat's happening?");
  // seed != null -> new event (date/time prefilled); else load the file.
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

// --- agenda list -----------------------------------------------------------
void new_event_cb(lv_event_t*) {
  // Prefill line 1 with the current local date/time so editing is minimal.
  char now[17];
  rtc_local_datetime(now);            // local "YYYY-MM-DD HH:MM"
  String seed = String(now) + "\n";   // empty title line to fill in
  open_editor(next_id(), seed.c_str());
}

void open_event_cb(lv_event_t* e) {
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
    launcher_go_home();               // -> calendar_teardown()
  else if (k == LV_KEY_DEL) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id >= 0) { delete_event(id); build_list(); }
  }
}

// Invert the focused row (black fill, white text) — clear on the 1-bit panel.
void row_focus_cb(lv_event_t* e) {
  lv_obj_t* row = lv_event_get_target(e);
  const bool f = lv_event_get_code(e) == LV_EVENT_FOCUSED;
  lv_obj_set_style_bg_color(row, f ? lv_color_black() : lv_color_white(), 0);
  lv_obj_set_style_bg_opa(row, f ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(row); i++)
    lv_obj_set_style_text_color(lv_obj_get_child(row, i),
                                f ? lv_color_white() : lv_color_black(), 0);
}

// A full-width row with explicit mono styling (lv_list is invisible on 1-bit).
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

void build_list() {
  lv_obj_t* old = g_list_scr;
  g_list_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_list_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_list_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_label_set_text(title, "Calendar");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* cont = lv_obj_create(g_list_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 36);
  lv_obj_set_pos(cont, 0, 36);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* nb = make_row(cont, LV_SYMBOL_PLUS "  New event",
                          (void*)(intptr_t)-1, new_event_cb, list_key_cb);

  auto events = list_events();
  Serial.printf("[cal] build_list: %u events\n", (unsigned)events.size());
  for (const auto& ev : events) {
    String row = short_when(ev.dt) + "  " + ev.title;
    make_row(cont, row.c_str(), (void*)(intptr_t)ev.id, open_event_cb,
             list_key_cb);
  }

  lv_scr_load(g_list_scr);
  lv_group_focus_obj(nb);
  if (old) lv_obj_del_async(old);
}

}  // namespace

void calendar_open() {
  launcher_set_leave_hook(calendar_teardown);
  g_list_scr = nullptr;  // fresh entry
  build_list();
}
