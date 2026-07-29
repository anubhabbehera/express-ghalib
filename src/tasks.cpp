/**
 * tasks.cpp — Tasks (to-do) app. See tasks.h.
 *
 * List rows follow the Reminders/Settings idiom (hand-built rows, inverted
 * focus — lv_list is invisible on the 1-bit panel). The editor is a slimmed
 * Notes-style form: dark title header + optional due-date field.
 */
#include "tasks.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <lvgl.h>
#include <algorithm>
#include <vector>

#include "launcher.h"
#include "rtc.h"
#include "st7305.h"

namespace {

constexpr const char* kPath = "/tasks.txt";

struct Task {
  bool done;
  String due;    // "YYYY-MM-DD" or ""
  String title;
};

std::vector<Task> load_tasks() {
  std::vector<Task> v;
  File f = LittleFS.open(kPath, "r");
  if (!f) return v;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;
    const int p1 = line.indexOf('|');
    const int p2 = line.indexOf('|', p1 + 1);
    if (p1 < 0 || p2 < 0) continue;
    Task t;
    t.done  = line.substring(0, p1) == "1";
    t.due   = line.substring(p1 + 1, p2);
    t.title = line.substring(p2 + 1);
    if (!t.title.isEmpty()) v.push_back(t);
  }
  f.close();
  return v;
}

void save_tasks(const std::vector<Task>& v) {
  File f = LittleFS.open(kPath, "w");
  if (!f) return;
  for (const Task& t : v)
    f.printf("%d|%s|%s\n", t.done ? 1 : 0, t.due.c_str(), t.title.c_str());
  f.close();
}

// Undone first (dated by due date, undated after), done at the bottom.
void sort_tasks(std::vector<Task>& v) {
  std::stable_sort(v.begin(), v.end(), [](const Task& a, const Task& b) {
    if (a.done != b.done) return !a.done;
    if (a.due.isEmpty() != b.due.isEmpty()) return !a.due.isEmpty();
    if (a.due != b.due) return a.due < b.due;
    return false;
  });
}

String today_str() {
  char dt[17];
  rtc_local_datetime(dt);
  return String(dt).substring(0, 10);
}

// Short due tag for a row: "today", "tmrw", "Jul 30", or "OVER" when overdue.
String due_tag(const String& due, const String& today) {
  if (due.isEmpty()) return "";
  if (due < today) return "OVER";
  if (due == today) return "today";
  static const char* kMon3[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const int m = due.substring(5, 7).toInt(), d = due.substring(8, 10).toInt();
  if (m < 1 || m > 12) return due;
  return String(kMon3[m - 1]) + " " + d;
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
lv_obj_t* g_scr      = nullptr;   // list screen
lv_obj_t* g_edit_scr = nullptr;   // editor screen
lv_obj_t* g_title_ta = nullptr;
lv_obj_t* g_due_ta   = nullptr;
int       g_edit_idx = -1;        // index into the SORTED task vector, -1 = new
std::vector<Task> g_tasks;        // sorted working copy shown in the list

void build_list();

// --- editor ----------------------------------------------------------------
void editor_save_close() {
  String title = g_title_ta ? String(lv_textarea_get_text(g_title_ta)) : "";
  String due   = g_due_ta ? String(lv_textarea_get_text(g_due_ta)) : "";
  title.trim();
  title.replace("\n", " ");
  due.trim();
  int y, m, d;
  if (sscanf(due.c_str(), "%d-%d-%d", &y, &m, &d) != 3) due = "";  // optional
  if (!title.isEmpty()) {
    if (g_edit_idx >= 0 && g_edit_idx < (int)g_tasks.size()) {
      g_tasks[g_edit_idx].title = title;
      g_tasks[g_edit_idx].due = due;
    } else {
      g_tasks.push_back({false, due, title});
    }
    save_tasks(g_tasks);
  }
  g_title_ta = g_due_ta = nullptr;
  lv_obj_t* es = g_edit_scr;
  g_edit_scr = nullptr;
  g_edit_idx = -1;
  build_list();
  if (es) lv_obj_del_async(es);
}

void editor_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) editor_save_close();
}
void title_ready_cb(lv_event_t*) { if (g_due_ta) lv_group_focus_obj(g_due_ta); }
void due_ready_cb(lv_event_t*) { editor_save_close(); }  // Enter on date = save

void open_editor(int idx) {
  g_edit_idx = idx;
  const bool editing = idx >= 0 && idx < (int)g_tasks.size();

  g_edit_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_edit_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(g_edit_scr, 0, 0);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  // Dark title header (Notes idiom).
  lv_obj_t* ta = lv_textarea_create(g_edit_scr);
  g_title_ta = ta;
  lv_obj_set_size(ta, ST7305_W, 44);
  lv_obj_set_pos(ta, 0, 0);
  lv_textarea_set_one_line(ta, true);
  lv_obj_set_style_radius(ta, 0, 0);
  lv_obj_set_style_bg_color(ta, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(ta, lv_color_white(), 0);
  lv_obj_set_style_border_width(ta, 0, 0);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);
  lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);
  lv_textarea_set_placeholder_text(ta, "Task...");
  lv_textarea_set_text(ta, editing ? g_tasks[idx].title.c_str() : "");
  lv_obj_add_event_cb(ta, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(ta, title_ready_cb, LV_EVENT_READY, nullptr);
  lv_group_add_obj(g, ta);

  lv_obj_t* lbl = lv_label_create(g_edit_scr);
  lv_label_set_text(lbl, "Due:");
  lv_obj_set_pos(lbl, 12, 58);

  lv_obj_t* due = lv_textarea_create(g_edit_scr);
  g_due_ta = due;
  lv_obj_set_size(due, 150, 28);
  lv_obj_set_pos(due, 60, 52);
  lv_textarea_set_one_line(due, true);
  lv_obj_set_style_radius(due, 2, 0);
  lv_obj_set_style_border_width(due, 1, 0);
  lv_obj_set_style_border_color(due, lv_color_black(), 0);
  lv_obj_set_style_pad_all(due, 3, 0);
  lv_obj_set_style_anim_time(due, 0, LV_PART_CURSOR);
  lv_textarea_set_placeholder_text(due, "YYYY-MM-DD");
  lv_textarea_set_text(due, editing ? g_tasks[idx].due.c_str() : "");
  lv_obj_add_event_cb(due, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(due, due_ready_cb, LV_EVENT_READY, nullptr);
  lv_group_add_obj(g, due);

  lv_obj_t* hint = lv_label_create(g_edit_scr);
  lv_label_set_text(hint, "date is optional - clear it for someday\n"
                          "Enter = next/save    Esc = save + back");
  lv_obj_set_pos(hint, 12, 94);

  lv_scr_load(g_edit_scr);
  lv_group_focus_obj(ta);
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

void clear_done() {
  g_tasks.erase(std::remove_if(g_tasks.begin(), g_tasks.end(),
                               [](const Task& t) { return t.done; }),
                g_tasks.end());
  save_tasks(g_tasks);
  build_list();
}

void row_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  const int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC)
    launcher_go_home();
  else if (k == 'e' || k == 'E') {
    if (idx >= 0) open_editor(idx);
  } else if (k == LV_KEY_DEL || k == LV_KEY_BACKSPACE) {
    if (idx >= 0 && idx < (int)g_tasks.size()) {
      g_tasks.erase(g_tasks.begin() + idx);
      save_tasks(g_tasks);
      build_list();
    }
  } else if (k == 'c' || k == 'C') {
    clear_done();
  }
}

// Enter/click: new-task row opens the editor; task rows toggle done.
void row_click_cb(lv_event_t* e) {
  const int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0) {
    open_editor(-1);
    return;
  }
  if (idx < (int)g_tasks.size()) {
    g_tasks[idx].done = !g_tasks[idx].done;
    save_tasks(g_tasks);
    build_list();
  }
}

lv_obj_t* make_row(lv_obj_t* parent, const String& left, const String& right,
                   int idx) {
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

  lv_obj_t* l = lv_label_create(row);
  lv_label_set_text(l, left.c_str());
  lv_obj_set_style_text_color(l, lv_color_black(), 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, ST7305_W - (right.isEmpty() ? 30 : 110));
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

  if (!right.isEmpty()) {
    lv_obj_t* r = lv_label_create(row);
    lv_label_set_text(r, right.c_str());
    lv_obj_set_style_text_color(r, lv_color_black(), 0);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
  }

  lv_obj_add_event_cb(row, row_focus_cb, LV_EVENT_FOCUSED, nullptr);
  lv_obj_add_event_cb(row, row_focus_cb, LV_EVENT_DEFOCUSED, nullptr);
  lv_obj_add_event_cb(row, row_key_cb, LV_EVENT_KEY, (void*)(intptr_t)idx);
  lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
  lv_group_add_obj(lv_group_get_default(), row);
  return row;
}

void build_list() {
  g_tasks = load_tasks();
  sort_tasks(g_tasks);
  const String today = today_str();

  lv_obj_t* old = g_scr;
  g_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  int open_n = 0;
  for (const Task& t : g_tasks)
    if (!t.done) open_n++;
  char hdr[32];
  snprintf(hdr, sizeof hdr, "Tasks  (%d open)", open_n);
  lv_label_set_text(title, hdr);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* hint = lv_label_create(g_scr);
  lv_label_set_text(hint, "Enter=done E=edit Del=del C=clear");
  lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -8, 10);

  lv_obj_t* cont = lv_obj_create(g_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 36);
  lv_obj_set_pos(cont, 0, 36);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(cont, 0, 0);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* nb = make_row(cont, LV_SYMBOL_PLUS "  New task", "", -1);

  int shown = 0;
  for (size_t i = 0; i < g_tasks.size() && shown < 24; i++, shown++) {
    const Task& t = g_tasks[i];
    const String box = t.done ? "[x]  " : "[ ]  ";
    make_row(cont, box + t.title, t.done ? "" : due_tag(t.due, today), (int)i);
  }
  if (g_tasks.empty()) {
    lv_obj_t* empty = lv_label_create(cont);
    lv_label_set_text(empty, "\n   no tasks - Enter to add one");
    lv_obj_set_style_text_color(empty, lv_color_black(), 0);
  }

  lv_scr_load(g_scr);
  lv_group_focus_obj(nb);
  if (old) lv_obj_del_async(old);
}

void app_teardown() {
  if (g_edit_scr) { lv_obj_del_async(g_edit_scr); g_edit_scr = nullptr; }
  if (g_scr) { lv_obj_del_async(g_scr); g_scr = nullptr; }
  g_title_ta = g_due_ta = nullptr;
  g_edit_idx = -1;
}

}  // namespace

void tasks_open() {
  launcher_set_leave_hook(app_teardown);
  g_scr = g_edit_scr = nullptr;
  build_list();
}

int tasks_due_on(const String& date, String* titles, int max) {
  int n = 0;
  for (const Task& t : load_tasks()) {
    if (t.done || t.due.isEmpty()) continue;
    if (t.due <= date && n < max) titles[n++] = t.title;  // due or overdue
  }
  return n;
}
