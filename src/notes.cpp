/**
 * notes.cpp — Notes app (list + editor) backed by LittleFS. See notes.h.
 *
 * Navigation: launcher -> note list -> editor. The physical Home button and
 * the list's Esc both return to the launcher via launcher_go_home(), which runs
 * notes_teardown() (registered as the leave hook) to save + free screens.
 */
#include "notes.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <lvgl.h>
#include <algorithm>
#include <vector>
#include "config.h"
#include "launcher.h"
#include "rtc.h"
#include "st7305.h"
#include "storage.h"

namespace {

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
struct NoteMeta { int id; String title; time_t mtime; };

String note_path(int id) { return String("/notes/") + id + ".txt"; }

// Most-recently-modified first. Mtimes are real since rtc.cpp syncs the system
// clock from the RTC at boot; ties (old files) fall back to newest id.
std::vector<NoteMeta> list_notes() {
  std::vector<NoteMeta> v;
  File dir = LittleFS.open("/notes");
  if (!dir || !dir.isDirectory()) return v;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String nm = f.name();
    const int slash = nm.lastIndexOf('/');
    if (slash >= 0) nm = nm.substring(slash + 1);
    if (!nm.endsWith(".txt")) continue;
    const int id = nm.substring(0, nm.length() - 4).toInt();
    String title = f.readStringUntil('\n');
    title.trim();
    if (title.length() > 32) title = title.substring(0, 32) + "...";
    if (title.isEmpty()) title = "(untitled)";
    v.push_back({id, title, f.getLastWrite()});
  }
  std::sort(v.begin(), v.end(), [](const NoteMeta& a, const NoteMeta& b) {
    if (a.mtime != b.mtime) return a.mtime > b.mtime;
    return a.id > b.id;
  });
  return v;
}

int next_id() {
  int mx = 0;
  for (const auto& n : list_notes()) mx = std::max(mx, n.id);
  return mx + 1;
}

String read_note(int id) {
  File f = LittleFS.open(note_path(id), "r");
  if (!f) return String();
  String s = f.readString();
  f.close();
  return s;
}

void write_note(int id, const char* text) {
  File f = LittleFS.open(note_path(id), "w");
  if (!f) { Serial.printf("[notes] write failed id=%d\n", id); return; }
  f.print(text);
  f.close();
}

void delete_note(int id) { LittleFS.remove(note_path(id)); }

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
lv_obj_t*    g_list_scr = nullptr;
lv_obj_t*    g_pick_scr = nullptr;   // template picker
lv_obj_t*    g_edit_scr = nullptr;
lv_obj_t*    g_title_ta = nullptr;   // fixed dark title header
lv_obj_t*    g_edit_ta  = nullptr;   // bounded body
lv_timer_t*  g_autosave = nullptr;
int          g_edit_id  = -1;

// Body text-size options (bottom bar). One focusable control: Left/Right or
// Enter change the active size; persisted via config_get/set_text_size.
const lv_font_t* kSizes[3] = {&lv_font_montserrat_14, &lv_font_montserrat_16,
                              &lv_font_montserrat_20};
const char*      kSizeName[3] = {"S", "M", "L"};
lv_obj_t*        g_size_lbl[3] = {};
int              g_size = 1;

lv_obj_t* g_search_ta = nullptr;  // search box on the list screen
lv_obj_t* g_wc_lbl    = nullptr;  // word count in the editor's bottom bar
String    g_filter;               // active search query ("" = all notes)
String    g_status;               // one-shot message on the list screen
bool      g_focus_search = false; // next build_list: focus the search box

void split_note(const String& full, String& title, String& body) {
  const int nl = full.indexOf('\n');
  if (nl < 0) { title = full; body = ""; }
  else { title = full.substring(0, nl); body = full.substring(nl + 1); }
}

// New-note templates. First line becomes the note's title; the body seeds the
// editor. "Blank" is the failover (existing notes also open with no template).
struct Template { const char* name; const char* icon; const char* body; };
const Template kTemplates[] = {
    {"Blank",     LV_SYMBOL_FILE, ""},
    {"Journal",   LV_SYMBOL_EDIT, "Journal\n\nThoughts:\n\n\nGrateful for:\n- "},
    {"Daily Log", LV_SYMBOL_LIST, "{date}\n\n[ ] \n[ ] \n[ ] \n\nNotes:\n"},
};

void build_list();
void build_picker();
void close_picker();
void open_editor(int id, const char* seed = nullptr);  // seed=null -> read file

// Note file = "title\nbody" (list_notes reads line 1 as the title).
String compose_note() {
  String title = g_title_ta ? lv_textarea_get_text(g_title_ta) : "";
  String body  = g_edit_ta  ? lv_textarea_get_text(g_edit_ta)  : "";
  title.replace("\n", " ");                 // title is single-line
  return title + "\n" + body;
}

void save_current() {
  if (!g_edit_ta) return;
  const String full = compose_note();
  if (full == "\n") delete_note(g_edit_id);  // title + body both empty -> drop
  else write_note(g_edit_id, full.c_str());
}

void autosave_cb(lv_timer_t*) {
  if (!g_edit_ta) return;
  const String full = compose_note();
  if (full != "\n") write_note(g_edit_id, full.c_str());
}

// Runs when returning to the launcher (Home button / list Esc).
void notes_teardown() {
  if (g_autosave) { lv_timer_del(g_autosave); g_autosave = nullptr; }
  if (g_edit_ta)  { save_current(); g_edit_ta = nullptr; g_title_ta = nullptr; }
  if (g_edit_scr) { lv_obj_del_async(g_edit_scr); g_edit_scr = nullptr; }
  if (g_pick_scr) { lv_obj_del_async(g_pick_scr); g_pick_scr = nullptr; }
  if (g_list_scr) { lv_obj_del_async(g_list_scr); g_list_scr = nullptr; }
  g_edit_id = -1;
  g_search_ta = g_wc_lbl = nullptr;
  g_filter = "";
}

// --- editor ----------------------------------------------------------------
int count_words(const char* s) {
  int n = 0;
  bool in_word = false;
  for (; *s; s++) {
    const bool ws = *s == ' ' || *s == '\n' || *s == '\t' || *s == '\r';
    if (!ws && !in_word) n++;
    in_word = !ws;
  }
  return n;
}

void wc_update() {
  if (!g_wc_lbl || !g_edit_ta) return;
  char b[16];
  snprintf(b, sizeof b, "%dw", count_words(lv_textarea_get_text(g_edit_ta)));
  lv_label_set_text(g_wc_lbl, b);
}

void body_changed_cb(lv_event_t*) { wc_update(); }

void editor_close_to_list() {
  if (g_autosave) { lv_timer_del(g_autosave); g_autosave = nullptr; }
  save_current();
  g_edit_ta = nullptr;
  g_title_ta = nullptr;
  g_wc_lbl = nullptr;
  lv_obj_t* es = g_edit_scr;
  g_edit_scr = nullptr;
  g_edit_id = -1;
  build_list();                       // rebuild list (reflects edits), shows it
  if (es) lv_obj_del_async(es);
}

// Esc from any editor field returns to the list (autosaved).
void editor_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) editor_close_to_list();
}

// Enter on the one-line title jumps to the body.
void title_ready_cb(lv_event_t*) {
  if (g_edit_ta) lv_group_focus_obj(g_edit_ta);
}

// Fill the active size chip; apply its font to the body; persist.
void size_set(int idx) {
  if (idx < 0) idx = 0;
  if (idx > 2) idx = 2;
  g_size = idx;
  config_set_text_size(idx);
  if (g_edit_ta) lv_obj_set_style_text_font(g_edit_ta, kSizes[idx], 0);
  for (int i = 0; i < 3; i++) {
    if (!g_size_lbl[i]) continue;
    const bool on = i == idx;
    lv_obj_set_style_bg_color(g_size_lbl[i], on ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_size_lbl[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(g_size_lbl[i], on ? lv_color_white() : lv_color_black(), 0);
  }
}

// The size bar is a single focus stop: a thick border shows focus; Left/Right
// (or Enter to cycle) change the size live. Tab moves on to the title.
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
  else if (k == LV_KEY_ESC) editor_close_to_list();
}

void open_editor(int id, const char* seed) {
  g_edit_id = id;
  g_edit_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_edit_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(g_edit_scr, 0, 0);

  String title, body;
  split_note(seed ? String(seed) : read_note(id), title, body);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  // --- fixed dark title header (constant, doesn't scroll with the body) ---
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
  lv_textarea_set_placeholder_text(tt, "Title");
  lv_textarea_set_text(tt, title.c_str());
  lv_obj_add_event_cb(tt, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(tt, title_ready_cb, LV_EVENT_READY, nullptr);  // Enter -> body
  lv_group_add_obj(g, tt);
  g_title_ta = tt;

  // --- bounded body box ---
  lv_obj_t* ta = lv_textarea_create(g_edit_scr);
  lv_obj_set_size(ta, ST7305_W - 12, ST7305_H - 32 - 30 - 8);
  lv_obj_set_pos(ta, 6, 36);
  lv_obj_set_style_radius(ta, 2, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, lv_color_black(), 0);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);  // steady (no blink) cursor
  lv_textarea_set_placeholder_text(ta, "Write... (Tab = size, Esc = back)");
  lv_textarea_set_text(ta, body.c_str());
  lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
  lv_obj_add_event_cb(ta, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(ta, body_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_group_add_obj(g, ta);
  g_edit_ta = ta;

  // --- bottom size bar: ONE focusable control (Tab-stop), Left/Right change ---
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

  g_wc_lbl = lv_label_create(bar);       // live word count, right-aligned
  lv_obj_add_flag(g_wc_lbl, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_align(g_wc_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_label_set_text(g_wc_lbl, "");
  lv_obj_add_event_cb(bar, size_bar_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(bar, size_bar_focus_cb, LV_EVENT_FOCUSED, nullptr);
  lv_obj_add_event_cb(bar, size_bar_focus_cb, LV_EVENT_DEFOCUSED, nullptr);
  lv_group_add_obj(g, bar);

  lv_scr_load(g_edit_scr);
  g_size = config_get_text_size();
  size_set(g_size);                          // body font + active chip fill
  wc_update();
  // New note (from a template) starts on the title; existing note on the body.
  lv_group_focus_obj(seed ? g_title_ta : g_edit_ta);
  g_autosave = lv_timer_create(autosave_cb, 3000, nullptr);
}

// --- SD backup / restore ---------------------------------------------------
// X = copy every note to SD /export/notes/<id>.txt; I = copy them back
// (overwriting by id). A plain file-level backup/restore pair.
void backup_to_sd() {
  if (!storage_sd_mount()) { g_status = "SD not available"; build_list(); return; }
  SD_MMC.mkdir("/export");
  SD_MMC.mkdir("/export/notes");
  int n = 0;
  for (const auto& nm : list_notes()) {
    File in = LittleFS.open(note_path(nm.id), "r");
    if (!in) continue;
    File out = SD_MMC.open(String("/export/notes/") + nm.id + ".txt", "w");
    if (!out) { in.close(); continue; }
    uint8_t buf[256];
    while (in.available()) out.write(buf, in.read(buf, sizeof buf));
    in.close();
    out.close();
    n++;
  }
  g_status = String("backed up ") + n + " to SD";
  build_list();
}

void restore_from_sd() {
  if (!storage_sd_mount()) { g_status = "SD not available"; build_list(); return; }
  File dir = SD_MMC.open("/export/notes");
  if (!dir || !dir.isDirectory()) {
    g_status = "no /export/notes on SD";
    build_list();
    return;
  }
  int n = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String nm = f.name();
    const int slash = nm.lastIndexOf('/');
    if (slash >= 0) nm = nm.substring(slash + 1);
    if (!nm.endsWith(".txt")) continue;
    const int id = nm.substring(0, nm.length() - 4).toInt();
    if (id <= 0) continue;
    File out = LittleFS.open(note_path(id), "w");
    if (!out) continue;
    uint8_t buf[256];
    while (f.available()) out.write(buf, f.read(buf, sizeof buf));
    out.close();
    n++;
  }
  g_status = String("restored ") + n + " from SD";
  build_list();
}

// --- search ----------------------------------------------------------------
// Title is line 1 of the note file, so one lowercase haystack covers both
// "search titles" and "search bodies".
bool note_matches(int id, const String& q) {
  if (q.isEmpty()) return true;
  String hay = read_note(id);
  hay.toLowerCase();
  return hay.indexOf(q) >= 0;
}

void search_apply_cb(lv_event_t*) {  // Enter in the search box
  if (!g_search_ta) return;
  g_filter = lv_textarea_get_text(g_search_ta);
  g_filter.trim();
  g_filter.toLowerCase();
  // READY fires on Enter *press*; the release leaks a click onto whatever the
  // rebuilt list focuses (mono_ui_lessons). Keep focus on the search box so
  // the stray click lands there harmlessly instead of opening "New note".
  g_focus_search = true;
  build_list();
}

// One-line box: Up/Down leave it (a one-line textarea otherwise eats arrows);
// Esc clears an active filter first, then backs out to the launcher.
void search_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN) lv_group_focus_next(g);
  else if (k == LV_KEY_UP) lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC) {
    if (g_filter.isEmpty()) launcher_go_home();
    else { g_filter = ""; build_list(); }
  }
}

// --- note list -------------------------------------------------------------
void new_note_cb(lv_event_t*) { build_picker(); }  // choose a template first

void open_note_cb(lv_event_t* e) {
  open_editor((int)(intptr_t)lv_event_get_user_data(e));  // seed=null -> file
}

void list_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC) {
    if (g_filter.isEmpty()) launcher_go_home();      // -> notes_teardown()
    else { g_filter = ""; build_list(); }            // Esc clears the search
  }
  else if (k == LV_KEY_DEL) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id >= 0) { delete_note(id); build_list(); }
  }
  else if (k == '/') { if (g_search_ta) lv_group_focus_obj(g_search_ta); }
  else if (k == 'x' || k == 'X') backup_to_sd();
  else if (k == 'i' || k == 'I') restore_from_sd();
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

// A full-width list row with explicit mono styling (lv_list renders invisibly
// on the 1-bit panel, so we build rows by hand like the launcher tiles).
lv_obj_t* make_row(lv_obj_t* parent, const char* icon, const char* text,
                   void* ud, lv_event_cb_t click_cb, lv_event_cb_t key_cb) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, 44);
  lv_obj_set_style_radius(row, 0, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(row, lv_color_black(), 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_pad_left(row, 10, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* lbl = lv_label_create(row);
  lv_label_set_text_fmt(lbl, "%s  %s", icon, text);
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
  lv_label_set_text(title, "Notes");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  // One-shot status / hint line (top right, next to the title).
  lv_obj_t* hint = lv_label_create(g_list_scr);
  lv_label_set_text(hint, g_status.isEmpty()
                              ? "/=find  X/I=SD backup/restore"
                              : g_status.c_str());
  g_status = "";
  lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -8, 10);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  // Search box — Enter filters titles+bodies, Esc clears the filter.
  lv_obj_t* st = lv_textarea_create(g_list_scr);
  g_search_ta = st;
  lv_obj_set_size(st, ST7305_W - 16, 28);
  lv_obj_set_pos(st, 8, 32);
  lv_textarea_set_one_line(st, true);
  lv_obj_set_style_radius(st, 2, 0);
  lv_obj_set_style_border_width(st, 1, 0);
  lv_obj_set_style_border_color(st, lv_color_black(), 0);
  lv_obj_set_style_pad_all(st, 3, 0);
  lv_obj_set_style_anim_time(st, 0, LV_PART_CURSOR);
  lv_textarea_set_placeholder_text(st, LV_SYMBOL_EYE_OPEN "  search notes...");
  lv_textarea_set_text(st, g_filter.c_str());
  lv_obj_add_event_cb(st, search_apply_cb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(st, search_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(g, st);

  lv_obj_t* cont = lv_obj_create(g_list_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 66);
  lv_obj_set_pos(cont, 0, 66);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* nb = make_row(cont, LV_SYMBOL_PLUS, "New note",
                          (void*)(intptr_t)-1, new_note_cb, list_key_cb);

  auto notes = list_notes();
  int shown = 0;
  for (const auto& n : notes) {
    if (!note_matches(n.id, g_filter)) continue;
    make_row(cont, LV_SYMBOL_FILE, n.title.c_str(), (void*)(intptr_t)n.id,
             open_note_cb, list_key_cb);
    shown++;
  }
  Serial.printf("[notes] build_list: %d/%u notes (filter='%s')\n", shown,
                (unsigned)notes.size(), g_filter.c_str());
  if (!g_filter.isEmpty()) {
    lv_label_set_text_fmt(hint, "%d match%s - Esc clears", shown,
                          shown == 1 ? "" : "es");
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -8, 10);
    if (shown == 0) {
      lv_obj_t* none = lv_label_create(cont);
      lv_label_set_text(none, "\n   no matches - Esc clears the search");
      lv_obj_set_style_text_color(none, lv_color_black(), 0);
    }
  }

  lv_scr_load(g_list_scr);
  lv_group_focus_obj(g_focus_search ? g_search_ta : nb);
  g_focus_search = false;
  if (old) lv_obj_del_async(old);
}

// --- template picker (shown when creating a new note) ----------------------
void close_picker() {
  if (g_pick_scr) { lv_obj_del_async(g_pick_scr); g_pick_scr = nullptr; }
}

void template_cb(lv_event_t* e) {
  const int idx = (int)(intptr_t)lv_event_get_user_data(e);
  const int id = next_id();
  // Substitute {date} (Daily Log titles by date) with the RTC's MM-DD-YY.
  String seed = kTemplates[idx].body;
  char ds[9];
  rtc_date_mmddyy(ds);
  seed.replace("{date}", ds);
  Serial.printf("[notes] new from template '%s' id=%d\n", kTemplates[idx].name, id);
  open_editor(id, seed.c_str());  // editor now active
  close_picker();
}

void picker_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC) { build_list(); close_picker(); }  // cancel -> list
}

void build_picker() {
  g_pick_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_pick_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_pick_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_label_set_text(title, "New note");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* cont = lv_obj_create(g_pick_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 36);
  lv_obj_set_pos(cont, 0, 36);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  lv_group_remove_all_objs(lv_group_get_default());
  for (int i = 0; i < (int)(sizeof(kTemplates) / sizeof(kTemplates[0])); i++)
    make_row(cont, kTemplates[i].icon, kTemplates[i].name, (void*)(intptr_t)i,
             template_cb, picker_key_cb);

  lv_scr_load(g_pick_scr);
  lv_group_focus_obj(lv_obj_get_child(cont, 0));
}

}  // namespace

void notes_open() {
  launcher_set_leave_hook(notes_teardown);
  g_list_scr = nullptr;  // fresh entry
  build_list();
}
