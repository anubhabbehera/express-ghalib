/**
 * calendar.cpp — Calendar app: month grid -> per-day agenda -> event editor.
 * See calendar.h.
 *
 * Views (each an lv_scr):
 *   month : 7x6 grid of date cells; today is boxed, the selection is inverted;
 *           a cell with events shows a dot. Arrows move the selection (crossing
 *           a month edge rolls to the adjacent month); Enter opens the day.
 *   day   : agenda list of the selected date's events + a "New event" row.
 *   editor: two-line textarea (date/time, then title) — same as before.
 *
 * Navigation: month <-Esc- launcher; day <-Esc- month; editor <-Esc- day.
 * The physical Home button returns straight to the launcher via
 * launcher_go_home(), which runs calendar_teardown() (the leave hook).
 *
 * An event is stored as /events/<id>.txt: line 1 = "YYYY-MM-DD HH:MM" (sorts
 * chronologically as plain text), line 2+ = title.
 */
#include "calendar.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <lvgl.h>
#include <time.h>
#include <algorithm>
#include <set>
#include <vector>
#include "config.h"
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

// Stored as: line 1 = "YYYY-MM-DD HH:MM", line 2 = title, line 3+ = body.
// list_events() only reads lines 1-2, so the body is transparent to the grid.
void write_event(int id, const String& dt, const String& title,
                 const String& body) {
  File f = LittleFS.open(event_path(id), "w");
  if (!f) { Serial.printf("[cal] write failed id=%d\n", id); return; }
  f.print(dt);
  f.print('\n');
  f.print(title);
  if (body.length()) { f.print('\n'); f.print(body); }
  f.close();
}

void delete_event(int id) { LittleFS.remove(event_path(id)); }

// "HH:MM" from a "YYYY-MM-DD HH:MM" start string (best-effort).
String hhmm(const String& dt) {
  return dt.length() >= 16 ? dt.substring(11, 16) : dt;
}

// ---------------------------------------------------------------------------
// Date helpers (TZ=UTC0 is set globally, so mktime normalizes as plain UTC).
// ---------------------------------------------------------------------------
const char* kMonth[12] = {"January", "February", "March",     "April",
                          "May",     "June",     "July",      "August",
                          "September", "October", "November", "December"};
const char* kWday[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
const char* kWday3[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

const char* kMon3[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// "Mon, Jul 20" for the given date.
String pretty_date(int y, int m, int d) {
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = 12;
  mktime(&t);
  char b[20];
  snprintf(b, sizeof b, "%s, %s %d", kWday3[t.tm_wday], kMon3[m - 1], d);
  return String(b);
}

// Weekday (0=Sun) of the 1st of month m (1..12) in year y.
int first_weekday(int y, int m) {
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = 1; t.tm_hour = 12;
  mktime(&t);
  return t.tm_wday;
}

// Number of days in month m (1..12) of year y.
int days_in_month(int y, int m) {
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = m; t.tm_mday = 0; t.tm_hour = 12;  // day 0 of next month
  mktime(&t);
  return t.tm_mday;
}

// "YYYY-MM-DD" for the given date.
String iso_date(int y, int m, int d) {
  char b[11];
  snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
  return String(b);
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
lv_obj_t* g_month_scr = nullptr;
lv_obj_t* g_day_scr   = nullptr;
lv_obj_t* g_edit_scr  = nullptr;
lv_obj_t* g_title_ta  = nullptr;   // dark header: event title
lv_obj_t* g_time_ta   = nullptr;   // one-line HH:MM
lv_obj_t* g_body_ta   = nullptr;   // bounded body: details (sentinel = editing)
int       g_edit_id   = -1;
String    g_edit_date;             // "YYYY-MM-DD" fixed for the event in the editor

// Body text-size options (shared design with the Notes editor).
const lv_font_t* kSizes[3] = {&lv_font_montserrat_14, &lv_font_montserrat_16,
                              &lv_font_montserrat_20};
const char*      kSizeName[3] = {"S", "M", "L"};
lv_obj_t*        g_size_lbl[3] = {};
int              g_size = 1;

int g_view_y = 0, g_view_m = 0;   // month currently shown in the grid
int g_sel_d  = 1;                 // selected day-of-month in the grid
int g_today_y = 0, g_today_m = 0, g_today_d = 0;  // "today" per the RTC
int g_day_y = 0, g_day_m = 0, g_day_d = 0;        // date whose agenda is shown

int       g_first_wday = 0;       // weekday of the 1st (grid offset)
int       g_dim = 30;             // days in the shown month
lv_obj_t* g_cells[42] = {nullptr};  // grid cell per slot (null = blank)

void build_month();
void build_day(int y, int m, int d);
void open_editor(int id, bool is_new);

// Grid slot index for a day-of-month.
int slot_of(int day) { return g_first_wday + day - 1; }

// ---------------------------------------------------------------------------
// Editor — mirrors the Notes editor: dark title header, a date/time strip, a
// bounded body box, and a bottom S/M/L size bar. The date is fixed (from the
// day you opened); only the time is editable here.
// ---------------------------------------------------------------------------
// Compose "date time\ntitle\nbody" and persist, or drop if title/time invalid.
void save_current() {
  if (!g_body_ta) return;
  String title = g_title_ta ? String(lv_textarea_get_text(g_title_ta)) : "";
  String tm    = g_time_ta  ? String(lv_textarea_get_text(g_time_ta))  : "";
  String body  = g_body_ta  ? String(lv_textarea_get_text(g_body_ta))  : "";
  title.trim(); title.replace("\n", " ");   // title is single-line
  tm.trim();
  const String dt = g_edit_date + " " + tm;  // "YYYY-MM-DD HH:MM"
  if (title.isEmpty() || !valid_dt(dt)) delete_event(g_edit_id);  // drop drafts
  else write_event(g_edit_id, dt, title, body);
}

void editor_close_to_day() {
  save_current();
  g_title_ta = g_time_ta = g_body_ta = nullptr;
  lv_obj_t* es = g_edit_scr;
  g_edit_scr = nullptr;
  g_edit_id = -1;
  build_day(g_day_y, g_day_m, g_day_d);   // reflect edits
  if (es) lv_obj_del_async(es);
}

// Esc from any field returns to the day agenda (saved).
void editor_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) editor_close_to_day();
}

// Enter walks the form top-down: title -> time -> body.
void title_ready_cb(lv_event_t*) { if (g_time_ta) lv_group_focus_obj(g_time_ta); }
void time_ready_cb(lv_event_t*)  { if (g_body_ta) lv_group_focus_obj(g_body_ta); }

// Fill the active size chip; apply its font to the body; persist (shared design).
void size_set(int idx) {
  if (idx < 0) idx = 0;
  if (idx > 2) idx = 2;
  g_size = idx;
  config_set_text_size(idx);
  if (g_body_ta) lv_obj_set_style_text_font(g_body_ta, kSizes[idx], 0);
  for (int i = 0; i < 3; i++) {
    if (!g_size_lbl[i]) continue;
    const bool on = i == idx;
    lv_obj_set_style_bg_color(g_size_lbl[i], on ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_size_lbl[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(g_size_lbl[i], on ? lv_color_white() : lv_color_black(), 0);
  }
}

void size_bar_focus_cb(lv_event_t* e) {
  lv_obj_t* bar = lv_event_get_target(e);
  const bool f = lv_event_get_code(e) == LV_EVENT_FOCUSED;
  lv_obj_set_style_outline_width(bar, f ? 2 : 0, 0);   // outline: no layout shift
  lv_obj_set_style_outline_color(bar, lv_color_black(), 0);
  lv_obj_set_style_outline_pad(bar, 0, 0);
}
void size_bar_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  if (k == LV_KEY_LEFT || k == LV_KEY_UP) size_set(g_size - 1);
  else if (k == LV_KEY_RIGHT || k == LV_KEY_DOWN) size_set(g_size + 1);
  else if (k == LV_KEY_ENTER) size_set((g_size + 1) % 3);   // cycle
  else if (k == LV_KEY_ESC) editor_close_to_day();
}

// Split a stored event ("date time\ntitle\nbody") into its parts.
void parse_event(const String& full, String& date, String& tm, String& title,
                 String& body) {
  const int nl1 = full.indexOf('\n');
  const String l1 = nl1 < 0 ? full : full.substring(0, nl1);
  const String rest = nl1 < 0 ? String() : full.substring(nl1 + 1);
  const int nl2 = rest.indexOf('\n');
  title = nl2 < 0 ? rest : rest.substring(0, nl2);
  body  = nl2 < 0 ? String() : rest.substring(nl2 + 1);
  date  = l1.length() >= 10 ? l1.substring(0, 10) : l1;
  tm    = l1.length() >= 16 ? l1.substring(11, 16) : "09:00";
}

void open_editor(int id, bool is_new) {
  g_edit_id = id;

  String date, tm, title, body;
  if (is_new) {
    date = iso_date(g_day_y, g_day_m, g_day_d);
    tm = "09:00"; title = ""; body = "";
  } else {
    parse_event(read_event(id), date, tm, title, body);
  }
  g_edit_date = date;

  g_edit_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_edit_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(g_edit_scr, 0, 0);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  // --- fixed dark title header ---
  lv_obj_t* hdr = lv_obj_create(g_edit_scr);
  lv_obj_set_size(hdr, ST7305_W, 32);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_radius(hdr, 0, 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_pad_hor(hdr, 8, 0);
  lv_obj_set_style_pad_ver(hdr, 0, 0);
  lv_obj_set_style_bg_color(hdr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* tt = lv_textarea_create(hdr);
  lv_obj_set_size(tt, ST7305_W - 16, 30);
  lv_obj_align(tt, LV_ALIGN_LEFT_MID, 0, 0);
  lv_textarea_set_one_line(tt, true);
  lv_obj_set_style_bg_opa(tt, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(tt, 0, 0);
  lv_obj_set_style_pad_all(tt, 2, 0);
  lv_obj_set_style_text_color(tt, lv_color_white(), 0);
  lv_obj_set_style_anim_time(tt, 0, LV_PART_CURSOR);
  lv_textarea_set_placeholder_text(tt, "Event title");
  lv_textarea_set_text(tt, title.c_str());
  lv_obj_add_event_cb(tt, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(tt, title_ready_cb, LV_EVENT_READY, nullptr);  // Enter -> time
  lv_group_add_obj(g, tt);
  g_title_ta = tt;

  // --- date + time strip (date fixed; time editable) ---
  lv_obj_t* strip = lv_obj_create(g_edit_scr);
  lv_obj_set_size(strip, ST7305_W, 26);
  lv_obj_set_pos(strip, 0, 34);
  lv_obj_set_style_radius(strip, 0, 0);
  lv_obj_set_style_border_side(strip, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(strip, 1, 0);
  lv_obj_set_style_border_color(strip, lv_color_black(), 0);
  lv_obj_set_style_pad_hor(strip, 8, 0);
  lv_obj_set_style_pad_ver(strip, 0, 0);
  lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* dlbl = lv_label_create(strip);
  lv_label_set_text(dlbl, pretty_date(g_day_y, g_day_m, g_day_d).c_str());
  lv_obj_align(dlbl, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* tta = lv_textarea_create(strip);
  lv_obj_set_size(tta, 62, 24);
  lv_obj_align(tta, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_textarea_set_one_line(tta, true);
  lv_obj_set_style_radius(tta, 2, 0);
  lv_obj_set_style_border_width(tta, 1, 0);
  lv_obj_set_style_border_color(tta, lv_color_black(), 0);
  lv_obj_set_style_pad_all(tta, 1, 0);
  lv_obj_set_style_anim_time(tta, 0, LV_PART_CURSOR);
  lv_textarea_set_placeholder_text(tta, "HH:MM");
  lv_textarea_set_text(tta, tm.c_str());
  lv_obj_add_event_cb(tta, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(tta, time_ready_cb, LV_EVENT_READY, nullptr);  // Enter -> body
  lv_group_add_obj(g, tta);
  g_time_ta = tta;

  // --- bounded body box ---
  lv_obj_t* ta = lv_textarea_create(g_edit_scr);
  lv_obj_set_size(ta, ST7305_W - 12, ST7305_H - 62 - 30 - 6);
  lv_obj_set_pos(ta, 6, 64);
  lv_obj_set_style_radius(ta, 2, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, lv_color_black(), 0);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);  // steady (no blink) cursor
  lv_textarea_set_placeholder_text(ta, "Details... (Tab = size, Esc = back)");
  lv_textarea_set_text(ta, body.c_str());
  lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
  lv_obj_add_event_cb(ta, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(g, ta);
  g_body_ta = ta;

  // --- bottom size bar: ONE focusable control, Left/Right change size ---
  lv_obj_t* bar = lv_obj_create(g_edit_scr);
  lv_obj_set_size(bar, ST7305_W, 28);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_color(bar, lv_color_black(), 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_hor(bar, 8, 0);
  lv_obj_set_style_pad_ver(bar, 1, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(bar, 6, 0);

  lv_obj_t* cap = lv_label_create(bar);
  lv_label_set_text(cap, "Text size");
  for (int i = 0; i < 3; i++) {
    lv_obj_t* l = lv_label_create(bar);
    lv_label_set_text(l, kSizeName[i]);
    lv_obj_set_style_pad_hor(l, 8, 0);
    lv_obj_set_style_pad_ver(l, 2, 0);
    lv_obj_set_style_radius(l, 2, 0);
    g_size_lbl[i] = l;
  }
  lv_obj_add_event_cb(bar, size_bar_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(bar, size_bar_focus_cb, LV_EVENT_FOCUSED, nullptr);
  lv_obj_add_event_cb(bar, size_bar_focus_cb, LV_EVENT_DEFOCUSED, nullptr);
  lv_group_add_obj(g, bar);

  lv_scr_load(g_edit_scr);
  g_size = config_get_text_size();
  size_set(g_size);                          // body font + active chip fill
  lv_group_focus_obj(is_new ? g_title_ta : g_body_ta);
}

// ---------------------------------------------------------------------------
// Day agenda (events on one date)
// ---------------------------------------------------------------------------
void new_event_cb(lv_event_t*) { open_editor(next_id(), true); }

void open_event_cb(lv_event_t* e) {
  open_editor((int)(intptr_t)lv_event_get_user_data(e), false);
}

void day_close_to_month() {
  lv_obj_t* ds = g_day_scr;
  g_day_scr = nullptr;
  build_month();
  if (ds) lv_obj_del_async(ds);
}

void day_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC)
    day_close_to_month();
  else if (k == LV_KEY_DEL) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id >= 0) { delete_event(id); build_day(g_day_y, g_day_m, g_day_d); }
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

void build_day(int y, int m, int d) {
  g_day_y = y; g_day_m = m; g_day_d = d;
  lv_obj_t* old = g_day_scr;
  g_day_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_day_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_day_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  char hdr[24];
  snprintf(hdr, sizeof hdr, "%s %d", kMonth[m - 1], d);
  lv_label_set_text(title, hdr);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* cont = lv_obj_create(g_day_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 36);
  lv_obj_set_pos(cont, 0, 36);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* nb = make_row(cont, LV_SYMBOL_PLUS "  New event",
                          (void*)(intptr_t)-1, new_event_cb, day_key_cb);

  const String prefix = iso_date(y, m, d);   // "YYYY-MM-DD"
  int shown = 0;
  for (const auto& ev : list_events()) {
    if (!ev.dt.startsWith(prefix)) continue;
    String row = hhmm(ev.dt) + "  " + ev.title;
    make_row(cont, row.c_str(), (void*)(intptr_t)ev.id, open_event_cb, day_key_cb);
    shown++;
  }
  Serial.printf("[cal] day %s: %d events\n", prefix.c_str(), shown);

  lv_scr_load(g_day_scr);
  lv_group_focus_obj(nb);
  if (old) lv_obj_del_async(old);
}

// ---------------------------------------------------------------------------
// Month grid
// ---------------------------------------------------------------------------
// Repaint one grid slot: selected -> inverted; today -> boxed; else plain.
void paint_slot(int slot) {
  if (slot < 0 || slot >= 42 || !g_cells[slot]) return;
  lv_obj_t* c = g_cells[slot];
  const int day = slot - g_first_wday + 1;
  const bool sel   = (day == g_sel_d);
  const bool today = (g_view_y == g_today_y && g_view_m == g_today_m &&
                      day == g_today_d);
  lv_obj_set_style_bg_color(c, sel ? lv_color_black() : lv_color_white(), 0);
  lv_obj_set_style_bg_opa(c, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(c, today ? 2 : 1, 0);
  lv_obj_t* lbl = lv_obj_get_child(c, 0);   // day-number label
  if (lbl) lv_obj_set_style_text_color(lbl,
             sel ? lv_color_white() : lv_color_black(), 0);
  lv_obj_t* dot = lv_obj_get_child(c, 1);   // event marker (may be absent)
  if (dot && lv_obj_check_type(dot, &lv_obj_class))
    lv_obj_set_style_bg_color(dot, sel ? lv_color_white() : lv_color_black(), 0);
}

// Move the selection by delta days; rebuild if it crosses into another month.
void move_selection(int delta) {
  struct tm t = {};
  t.tm_year = g_view_y - 1900; t.tm_mon = g_view_m - 1;
  t.tm_mday = g_sel_d + delta; t.tm_hour = 12;
  mktime(&t);   // normalize (rolls month/year, honours leap years)
  const int ny = t.tm_year + 1900, nm = t.tm_mon + 1, nd = t.tm_mday;
  if (ny != g_view_y || nm != g_view_m) {
    g_view_y = ny; g_view_m = nm; g_sel_d = nd;
    build_month();
  } else {
    const int old = g_sel_d;
    g_sel_d = nd;
    paint_slot(slot_of(old));
    paint_slot(slot_of(nd));
  }
}

// Open the selected day one tick later: keeping the grid focused through the
// Enter *release* stops that release from leaking as a click onto the day
// view's "New event" row (same deferral used in Settings for tz-save).
void open_day_deferred(lv_timer_t* t) {
  lv_timer_del(t);
  build_day(g_view_y, g_view_m, g_sel_d);
}

void month_key_cb(lv_event_t* e) {
  switch (lv_event_get_key(e)) {
    case LV_KEY_LEFT:  move_selection(-1); break;
    case LV_KEY_RIGHT: move_selection(+1); break;
    case LV_KEY_UP:    move_selection(-7); break;
    case LV_KEY_DOWN:  move_selection(+7); break;
    case LV_KEY_PREV:  move_selection(-days_in_month(g_view_y, g_view_m)); break;
    case LV_KEY_NEXT:  move_selection(+days_in_month(g_view_y, g_view_m)); break;
    case LV_KEY_ENTER: lv_timer_create(open_day_deferred, 40, nullptr); break;
    case LV_KEY_ESC:   launcher_go_home(); break;  // -> calendar_teardown()
  }
}

void build_month() {
  lv_obj_t* old = g_month_scr;
  g_month_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_month_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(g_month_scr, 0, 0);

  g_first_wday = first_weekday(g_view_y, g_view_m);
  g_dim = days_in_month(g_view_y, g_view_m);
  if (g_sel_d > g_dim) g_sel_d = g_dim;
  if (g_sel_d < 1) g_sel_d = 1;

  // Title: "July 2026", centred.
  lv_obj_t* title = lv_label_create(g_month_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  char hdr[24];
  snprintf(hdr, sizeof hdr, "%s %d", kMonth[g_view_m - 1], g_view_y);
  lv_label_set_text(title, hdr);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 3);

  // Weekday header row.
  const int colw = ST7305_W / 7;            // 400/7 = 57
  for (int i = 0; i < 7; i++) {
    lv_obj_t* w = lv_label_create(g_month_scr);
    lv_obj_set_style_text_font(w, &lv_font_montserrat_14, 0);
    lv_label_set_text(w, kWday[i]);
    lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(w, colw);
    lv_obj_set_pos(w, i * colw, 30);
  }

  // Which days have events this month? (dot markers)
  std::set<int> ev_days;
  const String mp = iso_date(g_view_y, g_view_m, 1).substring(0, 8);  // "YYYY-MM-"
  for (const auto& ev : list_events())
    if (ev.dt.startsWith(mp)) {
      int dd = ev.dt.substring(8, 10).toInt();
      if (dd >= 1 && dd <= 31) ev_days.insert(dd);
    }

  // Grid of 6 rows x 7 cols beneath the header.
  const int gy = 48;
  const int rowh = (ST7305_H - gy) / 6;      // (300-48)/6 = 42
  for (int i = 0; i < 42; i++) g_cells[i] = nullptr;
  for (int day = 1; day <= g_dim; day++) {
    const int slot = slot_of(day);
    const int col = slot % 7, r = slot / 7;
    lv_obj_t* c = lv_obj_create(g_month_scr);
    lv_obj_set_size(c, colw, rowh);
    lv_obj_set_pos(c, col * colw, gy + r * rowh);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_border_color(c, lv_color_black(), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* num = lv_label_create(c);
    lv_obj_set_style_text_font(num, &lv_font_montserrat_14, 0);
    char db[4];
    snprintf(db, sizeof db, "%d", day);
    lv_label_set_text(num, db);
    lv_obj_align(num, LV_ALIGN_TOP_LEFT, 3, 2);

    if (ev_days.count(day)) {                // event marker: small filled dot
      lv_obj_t* dot = lv_obj_create(c);
      lv_obj_set_size(dot, 6, 6);
      lv_obj_set_style_radius(dot, 3, 0);
      lv_obj_set_style_border_width(dot, 0, 0);
      lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
      lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -3);
      lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }
    g_cells[slot] = c;
    paint_slot(slot);
  }

  // One focusable proxy carries all keys (no per-cell group churn).
  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);
  lv_group_add_obj(g, g_month_scr);
  lv_obj_add_event_cb(g_month_scr, month_key_cb, LV_EVENT_KEY, nullptr);

  lv_scr_load(g_month_scr);
  lv_group_focus_obj(g_month_scr);
  if (old) lv_obj_del_async(old);
}

// ---------------------------------------------------------------------------
// Teardown / entry
// ---------------------------------------------------------------------------
void calendar_teardown() {
  if (g_body_ta)   { save_current(); g_title_ta = g_time_ta = g_body_ta = nullptr; }
  if (g_edit_scr)  { lv_obj_del_async(g_edit_scr);  g_edit_scr  = nullptr; }
  if (g_day_scr)   { lv_obj_del_async(g_day_scr);   g_day_scr   = nullptr; }
  if (g_month_scr) { lv_obj_del_async(g_month_scr); g_month_scr = nullptr; }
  g_edit_id = -1;
}

}  // namespace

void calendar_open() {
  launcher_set_leave_hook(calendar_teardown);
  g_month_scr = g_day_scr = g_edit_scr = nullptr;  // fresh entry

  // Seed the grid at today's date (per the RTC's local wall clock).
  char now[17];
  rtc_local_datetime(now);                         // "YYYY-MM-DD HH:MM"
  int y = 0, mo = 0, d = 0;
  if (sscanf(now, "%d-%d-%d", &y, &mo, &d) == 3 && y > 1970) {
    g_today_y = y; g_today_m = mo; g_today_d = d;
  } else {                                          // RTC unreadable -> a default
    g_today_y = 2026; g_today_m = 1; g_today_d = 1;
  }
  g_view_y = g_today_y; g_view_m = g_today_m; g_sel_d = g_today_d;
  build_month();
}
