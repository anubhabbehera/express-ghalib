/**
 * reminders.cpp — reminder scheduler + alert overlay + Reminders app.
 * See reminders.h.
 *
 * The scheduler is a background lv_timer (10 s). Each tick it reads the current
 * local time from the RTC ("YYYY-MM-DD HH:MM") and edge-triggers any calendar
 * event whose start crosses from future to due: an event fires exactly once,
 * when  g_last_check < event_dt <= now.  Because the timestamps are fixed-width
 * ISO-ish strings, lexical order equals chronological order, so a plain string
 * compare is the whole test — no parsing, no persistent fired-set, and events
 * already past at boot never fire. Snoozed reminders live in a separate
 * in-memory list (they must not rewrite the underlying event) and are checked
 * the same edge-triggered way.
 *
 * The alert is a modal on lv_layer_top() so it floats over whatever app is open.
 * While it is up the keypad is retargeted to an alert-local group so its
 * Dismiss / Snooze chips capture the keyboard; the physical KEY button also
 * dismisses (works with no keyboard). It auto-dismisses after ALERT_TIMEOUT_MS.
 *
 * A reminder IS a calendar event (shared /events store, same file format:
 * line 1 = "YYYY-MM-DD HH:MM", line 2 = title, line 3+ = body). The editor here
 * mirrors the Notes/Calendar editor (dark title header, editable date + time,
 * bounded body, S/M/L size bar) — the difference from Calendar is that a
 * reminder's date is freely editable (it is not picked from a month grid).
 */
#include "reminders.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <lvgl.h>
#include <time.h>
#include <algorithm>
#include <vector>
#include "audio.h"
#include "config.h"
#include "launcher.h"
#include "recur.h"
#include "rtc.h"
#include "st7305.h"

namespace {

String now_str();            // fwd (time helpers live below load_events)
time_t parse_dt(const String& s);
String fmt_dt(time_t tt);

constexpr uint32_t POLL_MS          = 10000;  // scheduler tick
constexpr uint32_t ALERT_TIMEOUT_MS = 60000;  // auto-dismiss an ignored alert
constexpr int      SNOOZE_MIN       = 10;     // snooze re-fires after this long

struct Event { int id; String dt; String title; };

// Read /events/*.txt into (id, dt, title). Reads only lines 1-2 (the body on
// line 3+ is transparent here). Kept local to stay decoupled from calendar.cpp.
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
    String l1 = f.readStringUntil('\n');
    l1.trim();
    const String dt = l1.length() > 16 ? l1.substring(0, 16) : l1;
    const String rep =
        l1.length() > 17 && l1[16] == '|' ? l1.substring(17) : String();
    String title = f.readStringUntil('\n');
    title.trim();
    if (title.isEmpty()) title = "(untitled)";
    if (dt.length() < 16) continue;
    if (rep.isEmpty()) {
      v.push_back({id, dt, title});
    } else {
      // Recurring: materialize the occurrence in the recent fire window (so
      // the edge-triggered scheduler sees it) and, if that one is already
      // past, the next upcoming one (for lists / dashboards / sleep arming).
      const String now = now_str();
      const String occ = recur_next(rep, dt, fmt_dt(parse_dt(now) - 86400));
      if (!occ.isEmpty()) {
        v.push_back({id, occ, title});
        if (occ <= now) {
          const String nxt = recur_next(rep, dt, now);
          if (!nxt.isEmpty()) v.push_back({id, nxt, title});
        }
      }
    }
  }
  std::sort(v.begin(), v.end(),
            [](const Event& a, const Event& b) { return a.dt < b.dt; });
  return v;
}

// ---------------------------------------------------------------------------
// Time helpers (TZ=UTC0 is set globally, so mktime/localtime are plain UTC —
// differences and formatting are consistent with how events are entered).
// ---------------------------------------------------------------------------
const char* kWday3[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* kMon3[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

String now_str() {
  char dt[17];
  rtc_local_datetime(dt);  // local "YYYY-MM-DD HH:MM"
  return String(dt);
}

time_t parse_dt(const String& s) {
  struct tm t = {};
  if (sscanf(s.c_str(), "%d-%d-%d %d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday,
             &t.tm_hour, &t.tm_min) < 5)
    return 0;
  t.tm_year -= 1900; t.tm_mon -= 1;
  return mktime(&t);
}

String fmt_dt(time_t tt) {
  struct tm t;
  localtime_r(&tt, &t);
  char b[17];
  strftime(b, sizeof b, "%Y-%m-%d %H:%M", &t);
  return String(b);
}

String add_minutes(const String& dt, int mins) {
  return fmt_dt(parse_dt(dt) + (time_t)mins * 60);
}

// Human "when": "in 45m", "in 3h", "Today 18:00", "Tomorrow 09:00",
// "Wed 14:00", or "Aug 3 09:00".
String relative_when(const String& now, const String& dt) {
  const time_t tn = parse_dt(now), te = parse_dt(dt);
  const long secs = (long)(te - tn);
  if (secs < 60) return "now";
  const long mins = secs / 60;
  if (mins < 60) return "in " + String(mins) + "m";
  if (mins < 360) {
    const long h = mins / 60, m = mins % 60;
    return m ? "in " + String(h) + "h " + String(m) + "m" : "in " + String(h) + "h";
  }
  struct tm et; localtime_r(&te, &et);
  char hm[6]; snprintf(hm, sizeof hm, "%02d:%02d", et.tm_hour, et.tm_min);
  const String nd = now.substring(0, 10), ed = dt.substring(0, 10);
  if (ed == nd) return String("Today ") + hm;
  if (fmt_dt(parse_dt(nd + " 00:00") + 86400).substring(0, 10) == ed)
    return String("Tomorrow ") + hm;
  const long days = (parse_dt(ed + " 12:00") - parse_dt(nd + " 12:00")) / 86400;
  if (days > 0 && days < 7) return String(kWday3[et.tm_wday]) + " " + hm;
  return String(kMon3[et.tm_mon]) + " " + String(et.tm_mday) + " " + hm;
}

// 0 = Today, 1 = Tomorrow, 2 = Later — for section grouping.
int day_bucket(const String& now, const String& dt) {
  const String nd = now.substring(0, 10), ed = dt.substring(0, 10);
  if (ed == nd) return 0;
  if (fmt_dt(parse_dt(nd + " 00:00") + 86400).substring(0, 10) == ed) return 1;
  return 2;
}

bool valid_dt(const String& s) {
  int y, mo, d, h, mi;
  return sscanf(s.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5;
}

// ---------------------------------------------------------------------------
// Event storage helpers
// ---------------------------------------------------------------------------
String event_path(int id) { return String("/events/") + id + ".txt"; }

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

void write_event(int id, const String& dt, const String& title,
                 const String& body, const String& rep = String()) {
  File f = LittleFS.open(event_path(id), "w");
  if (!f) { Serial.printf("[REM] write failed id=%d\n", id); return; }
  f.print(dt);
  if (rep.length()) { f.print('|'); f.print(rep); }
  f.print('\n');
  f.print(title);
  if (body.length()) { f.print('\n'); f.print(body); }
  f.close();
}

void delete_event(int id) { LittleFS.remove(event_path(id)); }

void parse_event(const String& full, String& date, String& tm, String& title,
                 String& body, String& rep) {
  const int nl1 = full.indexOf('\n');
  const String l1 = nl1 < 0 ? full : full.substring(0, nl1);
  const String rest = nl1 < 0 ? String() : full.substring(nl1 + 1);
  const int nl2 = rest.indexOf('\n');
  title = nl2 < 0 ? rest : rest.substring(0, nl2);
  body  = nl2 < 0 ? String() : rest.substring(nl2 + 1);
  date  = l1.length() >= 10 ? l1.substring(0, 10) : l1;
  tm    = l1.length() >= 16 ? l1.substring(11, 16) : "09:00";
  rep   = l1.length() > 17 && l1[16] == '|' ? l1.substring(17) : String();
}

// ---------------------------------------------------------------------------
// Alert overlay (lv_layer_top) with Dismiss / Snooze
// ---------------------------------------------------------------------------
struct Snooze { String dt; String title; };
std::vector<Snooze> g_snoozes;

lv_obj_t* g_alert_box   = nullptr;
lv_obj_t* g_alert_label = nullptr;
lv_group_t* g_alert_group = nullptr;   // holds the Dismiss/Snooze chips
uint32_t  g_alert_deadline = 0;
String    g_alert_when, g_alert_title;

lv_indev_t* kbd_indev() { return lv_indev_get_next(nullptr); }  // sole indev

void chip_focus_cb(lv_event_t* e) {
  lv_obj_t* c = lv_event_get_target(e);
  const bool f = lv_event_get_code(e) == LV_EVENT_FOCUSED;
  lv_obj_set_style_bg_color(c, f ? lv_color_black() : lv_color_white(), 0);
  lv_obj_set_style_bg_opa(c, f ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_obj_t* l = lv_obj_get_child(c, 0);
  if (l) lv_obj_set_style_text_color(l, f ? lv_color_white() : lv_color_black(), 0);
}

void chip_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  if (k == LV_KEY_LEFT || k == LV_KEY_UP)
    lv_group_focus_prev(g_alert_group);
  else if (k == LV_KEY_RIGHT || k == LV_KEY_DOWN)
    lv_group_focus_next(g_alert_group);
  else if (k == LV_KEY_ESC)
    reminders_dismiss();
}

void dismiss_cb(lv_event_t*) { reminders_dismiss(); }

void snooze_cb(lv_event_t*) {
  g_snoozes.push_back({add_minutes(now_str(), SNOOZE_MIN), g_alert_title});
  Serial.printf("[REM] snoozed '%s' +%dmin\n", g_alert_title.c_str(), SNOOZE_MIN);
  reminders_dismiss();
}

lv_obj_t* make_chip(lv_obj_t* parent, const char* text, lv_event_cb_t click_cb) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_set_size(c, 110, 34);
  lv_obj_set_style_radius(c, 4, 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_border_color(c, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(c, 0, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* l = lv_label_create(c);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  lv_obj_add_event_cb(c, click_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(c, chip_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(c, chip_focus_cb, LV_EVENT_FOCUSED, nullptr);
  lv_obj_add_event_cb(c, chip_focus_cb, LV_EVENT_DEFOCUSED, nullptr);
  lv_group_add_obj(g_alert_group, c);
  return c;
}

void raise_alert(const String& when, const String& title) {
  g_alert_when = when;
  g_alert_title = title;
  if (!g_alert_box) {
    g_alert_box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_alert_box, ST7305_W - 24, 190);
    lv_obj_center(g_alert_box);
    lv_obj_set_style_bg_color(g_alert_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_alert_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_alert_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(g_alert_box, 3, 0);
    lv_obj_set_style_radius(g_alert_box, 6, 0);
    lv_obj_clear_flag(g_alert_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hdr = lv_label_create(g_alert_box);
    lv_label_set_text(hdr, LV_SYMBOL_BELL "  Reminder");
    lv_obj_set_style_text_font(hdr, &pixel_operator_bold_16, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);

    g_alert_label = lv_label_create(g_alert_box);
    lv_obj_set_width(g_alert_label, ST7305_W - 64);
    lv_label_set_long_mode(g_alert_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g_alert_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_alert_label, LV_ALIGN_TOP_MID, 0, 34);

    // Dismiss / Snooze chips in their own group (keypad retargeted to it).
    g_alert_group = lv_group_create();
    lv_obj_t* chips = lv_obj_create(g_alert_box);
    lv_obj_set_size(chips, ST7305_W - 40, 42);
    lv_obj_align(chips, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_width(chips, 0, 0);
    lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(chips, 0, 0);
    lv_obj_clear_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* dis = make_chip(chips, LV_SYMBOL_OK " Dismiss", dismiss_cb);
    make_chip(chips, LV_SYMBOL_REFRESH " Snooze", snooze_cb);

    lv_indev_set_group(kbd_indev(), g_alert_group);  // capture the keyboard
    lv_group_focus_obj(dis);
  }
  lv_label_set_text(g_alert_label,
                    (relative_when(now_str(), when) + "\n" + title).c_str());
  g_alert_deadline = millis() + ALERT_TIMEOUT_MS;
  Serial.printf("[REM] alert: %s  %s\n", when.c_str(), title.c_str());
  lv_refr_now(nullptr);  // paint the overlay before the (blocking) beep
  audio_beep();          // audible half of the reminder (visible + audible)
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------
String g_last_check;  // "now" at the previous tick; edge-trigger boundary

void poll_cb(lv_timer_t*) {
  // Auto-dismiss an alert the user ignored.
  if (g_alert_box && (int32_t)(millis() - g_alert_deadline) >= 0)
    reminders_dismiss();

  const String now = now_str();
  if (g_last_check.isEmpty()) { g_last_check = now; return; }  // baseline only

  for (const Event& ev : load_events())
    if (g_last_check < ev.dt && ev.dt <= now) raise_alert(ev.dt, ev.title);

  // Snoozed reminders (in-memory; do not touch the stored event).
  for (size_t i = 0; i < g_snoozes.size();) {
    if (g_snoozes[i].dt <= now) {
      raise_alert(g_snoozes[i].dt, g_snoozes[i].title);
      g_snoozes.erase(g_snoozes.begin() + i);
    } else {
      ++i;
    }
  }

  g_last_check = now;
}

// ---------------------------------------------------------------------------
// Reminders app: upcoming list (relative labels + grouping + quick-add) and a
// Notes-style editor.
// ---------------------------------------------------------------------------
lv_obj_t* g_app_scr   = nullptr;
lv_obj_t* g_edit_scr  = nullptr;
lv_obj_t* g_title_ta  = nullptr;   // dark header: reminder title
lv_obj_t* g_date_ta   = nullptr;   // one-line YYYY-MM-DD
lv_obj_t* g_time_ta   = nullptr;   // one-line HH:MM
lv_obj_t* g_body_ta   = nullptr;   // bounded body (sentinel = editing)
int       g_edit_id   = -1;
String    g_edit_rep;              // repeat spec carried through the editor

const lv_font_t* kSizes[3] = {&pixel_operator_16, &pixel_operator_16,
                              &pixel_operator_bold_16};
const char*      kSizeName[3] = {"S", "M", "L"};
lv_obj_t*        g_size_lbl[3] = {};
int              g_size = 1;

void build_list();
void open_editor(int id, bool is_new, const String& seed_dt);

// --- editor ----------------------------------------------------------------
void save_current() {
  if (!g_body_ta) return;
  String title = g_title_ta ? String(lv_textarea_get_text(g_title_ta)) : "";
  String date  = g_date_ta  ? String(lv_textarea_get_text(g_date_ta))  : "";
  String tm    = g_time_ta  ? String(lv_textarea_get_text(g_time_ta))  : "";
  String body  = g_body_ta  ? String(lv_textarea_get_text(g_body_ta))  : "";
  title.trim(); title.replace("\n", " ");
  date.trim(); tm.trim();
  const String dt = date + " " + tm;
  if (title.isEmpty() || !valid_dt(dt)) delete_event(g_edit_id);  // drop drafts
  else write_event(g_edit_id, dt, title, body, g_edit_rep);  // keep recurrence
}

void editor_close_to_list() {
  save_current();
  g_title_ta = g_date_ta = g_time_ta = g_body_ta = nullptr;
  lv_obj_t* es = g_edit_scr;
  g_edit_scr = nullptr;
  g_edit_id = -1;
  build_list();
  if (es) lv_obj_del_async(es);
}

void editor_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) editor_close_to_list();
}

// Enter walks the form top-down: title -> date -> time -> body.
void title_ready_cb(lv_event_t*) { if (g_date_ta) lv_group_focus_obj(g_date_ta); }
void date_ready_cb(lv_event_t*)  { if (g_time_ta) lv_group_focus_obj(g_time_ta); }
void time_ready_cb(lv_event_t*)  { if (g_body_ta) lv_group_focus_obj(g_body_ta); }

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
  lv_obj_set_style_outline_width(bar, f ? 2 : 0, 0);
  lv_obj_set_style_outline_color(bar, lv_color_black(), 0);
  lv_obj_set_style_outline_pad(bar, 0, 0);
}
void size_bar_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  if (k == LV_KEY_LEFT || k == LV_KEY_UP) size_set(g_size - 1);
  else if (k == LV_KEY_RIGHT || k == LV_KEY_DOWN) size_set(g_size + 1);
  else if (k == LV_KEY_ENTER) size_set((g_size + 1) % 3);
  else if (k == LV_KEY_ESC) editor_close_to_list();
}

// A bordered one-line field for the date/time strip.
lv_obj_t* make_field(lv_obj_t* parent, int w, const char* ph, const char* val,
                     lv_event_cb_t ready_cb) {
  lv_obj_t* ta = lv_textarea_create(parent);
  lv_obj_set_size(ta, w, 24);
  lv_textarea_set_one_line(ta, true);
  lv_obj_set_style_radius(ta, 2, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, lv_color_black(), 0);
  lv_obj_set_style_pad_all(ta, 1, 0);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);
  lv_textarea_set_placeholder_text(ta, ph);
  lv_textarea_set_text(ta, val);
  lv_obj_add_event_cb(ta, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(ta, ready_cb, LV_EVENT_READY, nullptr);
  lv_group_add_obj(lv_group_get_default(), ta);
  return ta;
}

void open_editor(int id, bool is_new, const String& seed_dt) {
  g_edit_id = id;
  String date, tm, title, body;
  if (is_new) {
    date = seed_dt.length() >= 10 ? seed_dt.substring(0, 10) : seed_dt;
    tm   = seed_dt.length() >= 16 ? seed_dt.substring(11, 16) : "09:00";
    title = ""; body = "";
    g_edit_rep = "";
  } else {
    parse_event(read_event(id), date, tm, title, body, g_edit_rep);
  }

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
  lv_textarea_set_placeholder_text(tt, "Reminder title");
  lv_textarea_set_text(tt, title.c_str());
  lv_obj_add_event_cb(tt, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(tt, title_ready_cb, LV_EVENT_READY, nullptr);
  lv_group_add_obj(g, tt);
  g_title_ta = tt;

  // --- date + time strip (both editable) ---
  lv_obj_t* strip = lv_obj_create(g_edit_scr);
  lv_obj_set_size(strip, ST7305_W, 28);
  lv_obj_set_pos(strip, 0, 34);
  lv_obj_set_style_radius(strip, 0, 0);
  lv_obj_set_style_border_side(strip, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(strip, 1, 0);
  lv_obj_set_style_border_color(strip, lv_color_black(), 0);
  lv_obj_set_style_pad_hor(strip, 6, 0);
  lv_obj_set_style_pad_ver(strip, 1, 0);
  lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

  g_date_ta = make_field(strip, 120, "YYYY-MM-DD", date.c_str(), date_ready_cb);
  lv_obj_align(g_date_ta, LV_ALIGN_LEFT_MID, 0, 0);
  g_time_ta = make_field(strip, 62, "HH:MM", tm.c_str(), time_ready_cb);
  lv_obj_align(g_time_ta, LV_ALIGN_RIGHT_MID, 0, 0);

  // --- bounded body box ---
  lv_obj_t* ta = lv_textarea_create(g_edit_scr);
  lv_obj_set_size(ta, ST7305_W - 12, ST7305_H - 64 - 30 - 6);
  lv_obj_set_pos(ta, 6, 66);
  lv_obj_set_style_radius(ta, 2, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, lv_color_black(), 0);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);
  lv_textarea_set_placeholder_text(ta, "Notes... (Tab = size, Esc = back)");
  lv_textarea_set_text(ta, body.c_str());
  lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
  lv_obj_add_event_cb(ta, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(g, ta);
  g_body_ta = ta;

  // --- bottom size bar ---
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
  size_set(g_size);
  lv_group_focus_obj(is_new ? g_title_ta : g_body_ta);
}

// --- list -------------------------------------------------------------------
void app_teardown() {
  if (g_body_ta)  { save_current(); g_title_ta = g_date_ta = g_time_ta = g_body_ta = nullptr; }
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

// New / preset row: creates + opens an editor. user_data encodes the action.
void new_reminder_cb(lv_event_t*) {
  open_editor(next_id(), true, now_str());
}
void preset_1h_cb(lv_event_t*) {
  open_editor(next_id(), true, add_minutes(now_str(), 60));
}
void preset_evening_cb(lv_event_t*) {
  const String now = now_str();
  String dt = now.substring(0, 10) + " 18:00";
  if (dt <= now) dt = fmt_dt(parse_dt(dt) + 86400);   // already past -> tomorrow
  open_editor(next_id(), true, dt);
}
void preset_tomorrow_cb(lv_event_t*) {
  const String now = now_str();
  const String dt = fmt_dt(parse_dt(now.substring(0, 10) + " 09:00") + 86400);
  open_editor(next_id(), true, dt);
}
void open_reminder_cb(lv_event_t* e) {
  open_editor((int)(intptr_t)lv_event_get_user_data(e), false, String());
}

lv_obj_t* make_row(lv_obj_t* parent, const char* text, void* ud,
                   lv_event_cb_t click_cb) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, 38);
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

// Non-focusable section header ("Today" / "Tomorrow" / "Later").
void add_header(lv_obj_t* parent, const char* text) {
  lv_obj_t* h = lv_label_create(parent);
  lv_label_set_text(h, text);
  lv_obj_set_style_text_font(h, &pixel_operator_16, 0);
  lv_obj_set_style_pad_top(h, 6, 0);
  lv_obj_set_style_pad_left(h, 10, 0);
  lv_obj_set_style_pad_bottom(h, 2, 0);
}

void build_list() {
  lv_obj_t* old = g_app_scr;
  g_app_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_app_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_app_scr);
  lv_obj_set_style_text_font(title, &pixel_operator_bold_16, 0);
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

  // Quick-add fast paths, then the custom editor.
  lv_obj_t* nb = make_row(cont, LV_SYMBOL_BELL "  In 1 hour",
                          (void*)(intptr_t)-1, preset_1h_cb);
  make_row(cont, LV_SYMBOL_BELL "  This evening (18:00)",
           (void*)(intptr_t)-1, preset_evening_cb);
  make_row(cont, LV_SYMBOL_BELL "  Tomorrow (09:00)",
           (void*)(intptr_t)-1, preset_tomorrow_cb);
  make_row(cont, LV_SYMBOL_PLUS "  New reminder",
           (void*)(intptr_t)-1, new_reminder_cb);

  // Upcoming events with relative labels, grouped Today / Tomorrow / Later.
  const String now = now_str();
  int shown = 0, group = -1;
  const char* kGroupName[3] = {"Today", "Tomorrow", "Later"};
  for (const Event& ev : load_events()) {
    if (ev.dt < now) continue;        // already passed -> won't fire
    const int b = day_bucket(now, ev.dt);
    if (b != group) { add_header(cont, kGroupName[b]); group = b; }
    const String label = relative_when(now, ev.dt) + "   " + ev.title;
    make_row(cont, label.c_str(), (void*)(intptr_t)ev.id, open_reminder_cb);
    if (++shown >= 20) break;
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

// --- power-management hooks (see power.cpp / reminders.h) -------------------
void reminders_seed_baseline(const char* local_dt) { g_last_check = local_dt; }

void reminders_check_now() { poll_cb(nullptr); }

String reminders_next_dt() {
  const String now = now_str();
  String best;
  for (const Event& ev : load_events())
    if (ev.dt > now && (best.isEmpty() || ev.dt < best)) best = ev.dt;
  for (const auto& s : g_snoozes)
    if (s.dt > now && (best.isEmpty() || s.dt < best)) best = s.dt;
  return best;
}

bool reminders_due_since(const char* since) {
  const String now = now_str(), s(since);
  for (const Event& ev : load_events())
    if (s < ev.dt && ev.dt <= now) return true;
  return false;
}

bool reminders_snooze_pending() { return !g_snoozes.empty(); }

int reminders_upcoming(String* dts, String* titles, int max) {
  const String now = now_str();
  int n = 0;
  for (const Event& ev : load_events()) {  // already sorted by dt
    if (ev.dt <= now) continue;
    if (n >= max) break;
    dts[n] = ev.dt;
    titles[n] = ev.title;
    n++;
  }
  return n;
}

bool reminders_alert_active() { return g_alert_box != nullptr; }

void reminders_dismiss() {
  if (!g_alert_box) return;
  lv_indev_set_group(kbd_indev(), lv_group_get_default());  // release the keyboard
  lv_obj_del(g_alert_box);
  g_alert_box = g_alert_label = nullptr;
  if (g_alert_group) { lv_group_del(g_alert_group); g_alert_group = nullptr; }
}

void reminders_open() {
  launcher_set_leave_hook(app_teardown);
  g_app_scr = nullptr;  // fresh entry
  build_list();
}
