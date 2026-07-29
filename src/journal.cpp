/**
 * journal.cpp — date-keyed journal. See journal.h.
 *
 * List rows follow the Tasks/Reminders idiom (hand-built rows, inverted focus).
 * The editor is a slimmed Notes editor: fixed dark date header (not editable)
 * + body textarea with 3 s autosave. Empty entries are deleted on close.
 */
#include "journal.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <lvgl.h>
#include <algorithm>
#include <vector>

#include "launcher.h"
#include "rtc.h"
#include "st7305.h"
#include "storage.h"

namespace {

// Seed body for a fresh day — mirrors the Notes "Journal" template (minus the
// title line; the date is the journal's title).
constexpr const char* kSeed = "Thoughts:\n\n\nGrateful for:\n- ";

// ---------------------------------------------------------------------------
// Storage — /journal/YYYYMMDD.txt
// ---------------------------------------------------------------------------
String entry_path(const String& key) { return "/journal/" + key + ".txt"; }

bool key_valid(const String& key) {
  if (key.length() != 8) return false;
  for (char c : key)
    if (c < '0' || c > '9') return false;
  const int mo = key.substring(4, 6).toInt();
  const int d = key.substring(6, 8).toInt();
  return mo >= 1 && mo <= 12 && d >= 1 && d <= 31;
}

// All entry keys, newest first.
std::vector<String> list_entries() {
  std::vector<String> v;
  File dir = LittleFS.open("/journal");
  if (!dir || !dir.isDirectory()) return v;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String nm = f.name();
    const int slash = nm.lastIndexOf('/');
    if (slash >= 0) nm = nm.substring(slash + 1);
    if (!nm.endsWith(".txt")) continue;
    const String key = nm.substring(0, nm.length() - 4);
    if (key_valid(key)) v.push_back(key);
  }
  std::sort(v.begin(), v.end(), [](const String& a, const String& b) {
    return a > b;
  });
  return v;
}

String today_key() {
  char dt[17];
  rtc_local_datetime(dt);        // "YYYY-MM-DD HH:MM"
  String k = String(dt).substring(0, 10);
  k.replace("-", "");
  return k;
}

// "YYYYMMDD" -> "Wed, Jul 29 2026" (weekday via mktime; TZ is UTC0).
String pretty_date(const String& key) {
  struct tm tm = {};
  tm.tm_year = key.substring(0, 4).toInt() - 1900;
  tm.tm_mon = key.substring(4, 6).toInt() - 1;
  tm.tm_mday = key.substring(6, 8).toInt();
  tm.tm_hour = 12;
  mktime(&tm);                   // fills tm_wday
  char out[24];
  strftime(out, sizeof out, "%a, %b %d %Y", &tm);
  return String(out);
}

// Key for the day `delta` days from `key` (delta may be negative).
String key_shift(const String& key, int delta) {
  struct tm tm = {};
  tm.tm_year = key.substring(0, 4).toInt() - 1900;
  tm.tm_mon = key.substring(4, 6).toInt() - 1;
  tm.tm_mday = key.substring(6, 8).toInt() + delta;
  tm.tm_hour = 12;
  mktime(&tm);                   // normalizes month/year rollover
  char out[9];
  snprintf(out, sizeof out, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
           tm.tm_mday);
  return String(out);
}

// Consecutive written days ending today (or yesterday if today is unwritten).
int calc_streak(const std::vector<String>& keys, const String& today) {
  auto has = [&](const String& k) {
    return std::find(keys.begin(), keys.end(), k) != keys.end();
  };
  String day = has(today) ? today : key_shift(today, -1);
  int n = 0;
  while (has(day)) { n++; day = key_shift(day, -1); }
  return n;
}

// First non-empty line of an entry, truncated for a list row.
String entry_preview(const String& key) {
  File f = LittleFS.open(entry_path(key), "r");
  if (!f) return "";
  String line;
  while (f.available()) {
    line = f.readStringUntil('\n');
    line.trim();
    if (!line.isEmpty()) break;
  }
  f.close();
  if (line.length() > 24) line = line.substring(0, 24) + "...";
  return line;
}

// Month-name date parsing for the jump box: "jan 1" / "jan1" -> current year.
// Also accepts "t"/"today"/"" (today) and bare "YYYYMMDD". Returns "" if bad.
String parse_jump(String q, const String& today) {
  q.trim();
  q.toLowerCase();
  if (q.isEmpty() || q == "t" || q == "today") return today;
  bool digits = true;
  for (char c : q)
    if (c < '0' || c > '9') digits = false;
  if (digits && q.length() == 8) return key_valid(q) ? q : "";
  static const char* mons = "janfebmaraprmayjunjulaugsepoctnovdec";
  if (q.length() < 4) return "";
  const char* p = strstr(mons, q.substring(0, 3).c_str());
  if (!p) return "";
  const int mo = (int)((p - mons) / 3 + 1);
  const int d = q.substring(3).toInt();     // toInt skips leading spaces
  if (d < 1 || d > 31) return "";
  char key[9];
  snprintf(key, sizeof key, "%s%02d%02d", today.substring(0, 4).c_str(), mo, d);
  return key_valid(key) ? String(key) : "";
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
lv_obj_t*   g_scr = nullptr;       // list screen
lv_obj_t*   g_edit_scr = nullptr;  // editor screen
lv_obj_t*   g_jump_ta = nullptr;   // jump box on the list screen
lv_obj_t*   g_body_ta = nullptr;   // editor body
lv_timer_t* g_autosave = nullptr;
String      g_edit_key;            // day being edited
String      g_status;              // one-shot message under the header

void build_list();

// --- editor ----------------------------------------------------------------
void save_entry() {
  if (!g_body_ta) return;
  String body = lv_textarea_get_text(g_body_ta);
  String probe = body;
  probe.trim();
  if (probe.isEmpty() || probe == String(kSeed).substring(0, probe.length())) {
    // Untouched seed / blank -> not a written day; drop the file.
    LittleFS.remove(entry_path(g_edit_key));
    return;
  }
  File f = LittleFS.open(entry_path(g_edit_key), "w");
  if (!f) { Serial.printf("[jrnl] write failed %s\n", g_edit_key.c_str()); return; }
  f.print(body);
  f.close();
}

void autosave_cb(lv_timer_t*) { save_entry(); }

void editor_close_to_list() {
  if (g_autosave) { lv_timer_del(g_autosave); g_autosave = nullptr; }
  save_entry();
  g_body_ta = nullptr;
  lv_obj_t* es = g_edit_scr;
  g_edit_scr = nullptr;
  g_edit_key = "";
  build_list();
  if (es) lv_obj_del_async(es);
}

void editor_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) editor_close_to_list();
}

void open_editor(const String& key) {
  g_edit_key = key;
  String body;
  File f = LittleFS.open(entry_path(key), "r");
  if (f) { body = f.readString(); f.close(); }
  const bool fresh = body.isEmpty();
  if (fresh) body = kSeed;

  g_edit_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_edit_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(g_edit_scr, 0, 0);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  // Fixed dark date header (the entry's "title" — not editable).
  lv_obj_t* hdr = lv_obj_create(g_edit_scr);
  lv_obj_set_size(hdr, ST7305_W, 32);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_radius(hdr, 0, 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_pad_hor(hdr, 8, 0);
  lv_obj_set_style_bg_color(hdr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* dt = lv_label_create(hdr);
  lv_label_set_text(dt, pretty_date(key).c_str());
  lv_obj_set_style_text_color(dt, lv_color_white(), 0);
  lv_obj_set_style_text_font(dt, &lv_font_montserrat_16, 0);
  lv_obj_align(dt, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* ta = lv_textarea_create(g_edit_scr);
  lv_obj_set_size(ta, ST7305_W - 12, ST7305_H - 32 - 8);
  lv_obj_set_pos(ta, 6, 36);
  lv_obj_set_style_radius(ta, 2, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, lv_color_black(), 0);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);  // steady cursor
  lv_textarea_set_text(ta, body.c_str());
  lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
  lv_obj_add_event_cb(ta, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(g, ta);
  g_body_ta = ta;

  lv_scr_load(g_edit_scr);
  lv_group_focus_obj(ta);
  g_autosave = lv_timer_create(autosave_cb, 3000, nullptr);
  (void)fresh;
}

// --- archive to SD ---------------------------------------------------------
void archive_to_sd() {
  if (!storage_sd_mount()) { g_status = "SD not available"; build_list(); return; }
  SD_MMC.mkdir("/export");
  SD_MMC.mkdir("/export/journal");
  int n = 0;
  for (const String& key : list_entries()) {
    File in = LittleFS.open(entry_path(key), "r");
    if (!in) continue;
    File out = SD_MMC.open("/export/journal/" + key + ".txt", "w");
    if (!out) { in.close(); continue; }
    uint8_t buf[256];
    while (in.available()) out.write(buf, in.read(buf, sizeof buf));
    in.close();
    out.close();
    n++;
  }
  g_status = String("archived ") + n + " to SD";
  build_list();
}

// --- list ------------------------------------------------------------------
void row_focus_cb(lv_event_t* e) {
  lv_obj_t* row = lv_event_get_target(e);
  const bool f = lv_event_get_code(e) == LV_EVENT_FOCUSED;
  lv_obj_set_style_bg_color(row, f ? lv_color_black() : lv_color_white(), 0);
  lv_obj_set_style_bg_opa(row, f ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(row); i++)
    lv_obj_set_style_text_color(lv_obj_get_child(row, i),
                                f ? lv_color_white() : lv_color_black(), 0);
}

// Row user_data: heap String* holding the entry key ("" = the Today row).
void row_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC)
    launcher_go_home();
  else if (k == 't' || k == 'T')
    open_editor(today_key());
  else if (k == 'a' || k == 'A')
    archive_to_sd();
  else if (k == LV_KEY_DEL || k == LV_KEY_BACKSPACE) {
    const String* key = static_cast<String*>(lv_event_get_user_data(e));
    if (key && !key->isEmpty()) {
      LittleFS.remove(entry_path(*key));
      build_list();
    }
  }
}

void row_click_cb(lv_event_t* e) {
  const String* key = static_cast<String*>(lv_event_get_user_data(e));
  open_editor(key && !key->isEmpty() ? *key : today_key());
}

// Row keys live on the row objects; free them with the screen.
void row_del_cb(lv_event_t* e) {
  delete static_cast<String*>(lv_event_get_user_data(e));
}

lv_obj_t* make_row(lv_obj_t* parent, const String& left, const String& right,
                   const String& key) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, 34);
  lv_obj_set_style_radius(row, 0, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(row, lv_color_black(), 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_pad_left(row, 10, 0);
  lv_obj_set_style_pad_right(row, 10, 0);
  lv_obj_set_style_pad_top(row, 6, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* l = lv_label_create(row);
  lv_label_set_text(l, left.c_str());
  lv_obj_set_style_text_color(l, lv_color_black(), 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, ST7305_W - (right.isEmpty() ? 30 : 130));
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

  if (!right.isEmpty()) {
    lv_obj_t* r = lv_label_create(row);
    lv_label_set_text(r, right.c_str());
    lv_obj_set_style_text_color(r, lv_color_black(), 0);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
  }

  String* ud = new String(key);
  lv_obj_add_event_cb(row, row_focus_cb, LV_EVENT_FOCUSED, nullptr);
  lv_obj_add_event_cb(row, row_focus_cb, LV_EVENT_DEFOCUSED, nullptr);
  lv_obj_add_event_cb(row, row_key_cb, LV_EVENT_KEY, ud);
  lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, ud);
  lv_obj_add_event_cb(row, row_del_cb, LV_EVENT_DELETE, ud);
  lv_group_add_obj(lv_group_get_default(), row);
  return row;
}

// --- jump box ---------------------------------------------------------------
void jump_ready_cb(lv_event_t*) {
  if (!g_jump_ta) return;
  const String key = parse_jump(lv_textarea_get_text(g_jump_ta), today_key());
  if (key.isEmpty()) {
    g_status = "date? try: jan 1 / 20260101 / t";
    build_list();
    return;
  }
  open_editor(key);
}

// One-line box: Up/Down leave it (a one-line textarea otherwise eats arrows).
void jump_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN) lv_group_focus_next(g);
  else if (k == LV_KEY_UP) lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC) launcher_go_home();
}

void build_list() {
  const String today = today_key();
  auto keys = list_entries();
  const int streak = calc_streak(keys, today);

  lv_obj_t* old = g_scr;
  g_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_label_set_text(title, "Journal");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* stats = lv_label_create(g_scr);
  char st[48];
  snprintf(st, sizeof st, "%u written  ·  streak %d", (unsigned)keys.size(),
           streak);
  lv_label_set_text(stats, st);
  lv_obj_align(stats, LV_ALIGN_TOP_RIGHT, -8, 10);

  // One-shot status / hint line.
  lv_obj_t* hint = lv_label_create(g_scr);
  lv_label_set_text(hint, g_status.isEmpty()
                              ? "T=today  A=archive->SD  Del=delete"
                              : g_status.c_str());
  g_status = "";
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 12, 30);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  // Jump box.
  lv_obj_t* jt = lv_textarea_create(g_scr);
  g_jump_ta = jt;
  lv_obj_set_size(jt, ST7305_W - 16, 28);
  lv_obj_set_pos(jt, 8, 48);
  lv_textarea_set_one_line(jt, true);
  lv_obj_set_style_radius(jt, 2, 0);
  lv_obj_set_style_border_width(jt, 1, 0);
  lv_obj_set_style_border_color(jt, lv_color_black(), 0);
  lv_obj_set_style_pad_all(jt, 3, 0);
  lv_obj_set_style_anim_time(jt, 0, LV_PART_CURSOR);
  lv_textarea_set_placeholder_text(jt, "go to: jan 1 / 20260101 / t");
  lv_obj_add_event_cb(jt, jump_ready_cb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(jt, jump_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(g, jt);

  lv_obj_t* cont = lv_obj_create(g_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 82);
  lv_obj_set_pos(cont, 0, 82);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(cont, 0, 0);

  // Today first (opens/creates), then every written day, newest first.
  const bool today_written =
      std::find(keys.begin(), keys.end(), today) != keys.end();
  lv_obj_t* first =
      make_row(cont, String(today_written ? LV_SYMBOL_EDIT : LV_SYMBOL_PLUS) +
                         "  Today - " + pretty_date(today),
               "", "");

  int shown = 0;
  for (const String& key : keys) {
    if (key == today || shown >= 22) continue;
    make_row(cont, pretty_date(key), entry_preview(key), key);
    shown++;
  }

  lv_scr_load(g_scr);
  lv_group_focus_obj(first);
  if (old) lv_obj_del_async(old);
}

void app_teardown() {
  if (g_autosave) { lv_timer_del(g_autosave); g_autosave = nullptr; }
  if (g_body_ta) { save_entry(); g_body_ta = nullptr; }
  if (g_edit_scr) { lv_obj_del_async(g_edit_scr); g_edit_scr = nullptr; }
  if (g_scr) { lv_obj_del_async(g_scr); g_scr = nullptr; }
  g_jump_ta = nullptr;
  g_edit_key = "";
}

}  // namespace

void journal_open() {
  launcher_set_leave_hook(app_teardown);
  g_scr = g_edit_scr = nullptr;
  build_list();
}
