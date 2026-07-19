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
#include <lvgl.h>
#include <algorithm>
#include <vector>
#include "launcher.h"
#include "st7305.h"

namespace {

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
struct NoteMeta { int id; String title; };

String note_path(int id) { return String("/notes/") + id + ".txt"; }

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
    v.push_back({id, title});
  }
  std::sort(v.begin(), v.end(),
            [](const NoteMeta& a, const NoteMeta& b) { return a.id > b.id; });
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
lv_obj_t*    g_edit_scr = nullptr;
lv_obj_t*    g_edit_ta  = nullptr;
lv_timer_t*  g_autosave = nullptr;
int          g_edit_id  = -1;

void build_list();  // fwd

void save_current() {
  if (!g_edit_ta) return;
  const char* txt = lv_textarea_get_text(g_edit_ta);
  if (!txt || txt[0] == '\0') delete_note(g_edit_id);  // drop empty notes
  else write_note(g_edit_id, txt);
}

void autosave_cb(lv_timer_t*) {
  if (!g_edit_ta) return;
  const char* txt = lv_textarea_get_text(g_edit_ta);
  if (txt && txt[0]) write_note(g_edit_id, txt);
}

// Runs when returning to the launcher (Home button / list Esc).
void notes_teardown() {
  if (g_autosave) { lv_timer_del(g_autosave); g_autosave = nullptr; }
  if (g_edit_ta)  { save_current(); g_edit_ta = nullptr; }
  if (g_edit_scr) { lv_obj_del_async(g_edit_scr); g_edit_scr = nullptr; }
  if (g_list_scr) { lv_obj_del_async(g_list_scr); g_list_scr = nullptr; }
  g_edit_id = -1;
}

// --- editor ----------------------------------------------------------------
void editor_close_to_list() {
  if (g_autosave) { lv_timer_del(g_autosave); g_autosave = nullptr; }
  save_current();
  g_edit_ta = nullptr;
  lv_obj_t* es = g_edit_scr;
  g_edit_scr = nullptr;
  g_edit_id = -1;
  build_list();                       // rebuild list (reflects edits), shows it
  if (es) lv_obj_del_async(es);
}

void editor_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) editor_close_to_list();
}

void open_editor(int id) {
  g_edit_id = id;
  g_edit_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_edit_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* ta = lv_textarea_create(g_edit_scr);
  lv_obj_set_size(ta, ST7305_W, ST7305_H);
  lv_obj_set_pos(ta, 0, 0);
  lv_obj_set_style_radius(ta, 0, 0);
  lv_obj_set_style_border_width(ta, 0, 0);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);  // steady (no blink) cursor
  lv_textarea_set_placeholder_text(ta, "Write... (Esc = back)");
  lv_textarea_set_text(ta, read_note(id).c_str());
  lv_obj_add_event_cb(ta, editor_key_cb, LV_EVENT_KEY, nullptr);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);
  lv_group_add_obj(g, ta);

  g_edit_ta = ta;
  lv_scr_load(g_edit_scr);
  lv_group_focus_obj(ta);
  g_autosave = lv_timer_create(autosave_cb, 3000, nullptr);
}

// --- note list -------------------------------------------------------------
void new_note_cb(lv_event_t*) { open_editor(next_id()); }

void open_note_cb(lv_event_t* e) {
  open_editor((int)(intptr_t)lv_event_get_user_data(e));
}

void list_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC)
    launcher_go_home();               // -> notes_teardown()
  else if (k == LV_KEY_DEL) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id >= 0) { delete_note(id); build_list(); }
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

// A full-width list row with explicit mono styling (lv_list renders invisibly
// on the 1-bit panel, so we build rows by hand like the launcher tiles).
lv_obj_t* make_row(lv_obj_t* parent, const char* icon, const char* text,
                   intptr_t id, bool is_new) {
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
  lv_obj_add_event_cb(row, list_key_cb, LV_EVENT_KEY, (void*)id);
  lv_obj_add_event_cb(row, is_new ? new_note_cb : open_note_cb,
                      LV_EVENT_CLICKED, (void*)id);
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

  lv_obj_t* cont = lv_obj_create(g_list_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 36);
  lv_obj_set_pos(cont, 0, 36);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* nb = make_row(cont, LV_SYMBOL_PLUS, "New note", -1, true);

  auto notes = list_notes();
  Serial.printf("[notes] build_list: %u notes\n", (unsigned)notes.size());
  for (const auto& n : notes)
    make_row(cont, LV_SYMBOL_FILE, n.title.c_str(), n.id, false);

  lv_scr_load(g_list_scr);
  lv_group_focus_obj(nb);
  if (old) lv_obj_del_async(old);
}

}  // namespace

void notes_open() {
  launcher_set_leave_hook(notes_teardown);
  g_list_scr = nullptr;  // fresh entry
  build_list();
}
