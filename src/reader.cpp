/**
 * reader.cpp — paginated .txt reader. See reader.h.
 *
 * Pagination is byte-window based: read ~a-page of bytes at the current
 * offset, cut at the last whitespace, render into a static label (one full
 * redraw per page — the reflective panel's sweet spot). Forward pages push
 * their start offsets onto a history stack so Prev is exact; position is
 * persisted per book in /reader_pos.txt.
 */
#include "reader.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <lvgl.h>
#include <algorithm>
#include <vector>

#include "config.h"
#include "launcher.h"
#include "st7305.h"
#include "storage.h"

namespace {

constexpr const char* kBooksDir = "/books";
constexpr const char* kPosPath = "/reader_pos.txt";  // LittleFS: "offset|name"

// Pixel Operator reads pixel-perfect only at grid multiples, so the S/M/L
// ladder is 16/32/48. Bytes per page are conservative so the label never
// clips.
const lv_font_t* kFonts[3] = {&pixel_operator_16, &pixel_operator_32,
                              &pixel_operator_48};
const int kPageBytes[3] = {720, 200, 90};
const char* kSizeName[3] = {"S", "M", "L"};

// ---------------------------------------------------------------------------
// Saved positions
// ---------------------------------------------------------------------------
size_t load_pos(const String& name) {
  File f = LittleFS.open(kPosPath, "r");
  if (!f) return 0;
  size_t pos = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    const int bar = line.indexOf('|');
    if (bar > 0 && line.substring(bar + 1) == name) {
      pos = (size_t)line.substring(0, bar).toInt();
      break;
    }
  }
  f.close();
  return pos;
}

void save_pos(const String& name, size_t pos) {
  // Rewrite the whole (tiny) file with this book's entry updated/prepended.
  std::vector<String> lines;
  File f = LittleFS.open(kPosPath, "r");
  if (f) {
    while (f.available() && lines.size() < 20) {
      String line = f.readStringUntil('\n');
      line.trim();
      const int bar = line.indexOf('|');
      if (bar > 0 && line.substring(bar + 1) != name) lines.push_back(line);
    }
    f.close();
  }
  f = LittleFS.open(kPosPath, "w");
  if (!f) return;
  f.printf("%u|%s\n", (unsigned)pos, name.c_str());
  for (const String& l : lines) f.println(l);
  f.close();
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
lv_obj_t* g_list_scr = nullptr;
lv_obj_t* g_page_scr = nullptr;
lv_obj_t* g_page_lbl = nullptr;   // the page text
lv_obj_t* g_hdr_lbl  = nullptr;   // "name  42%"
lv_obj_t* g_size_lbl[3] = {};
String    g_book;                 // open book filename (within /books)
size_t    g_book_size = 0;
size_t    g_offset = 0;           // current page start
size_t    g_next = 0;             // next page start (offset + shown bytes)
int       g_size = 1;
String    g_status;
std::vector<size_t> g_history;    // page-start stack for exact Prev

void build_list();

// Built-in sample page (g_book == "") so fonts can be judged with no SD card.
constexpr const char* kSample =
    "The quick brown fox jumps over the lazy dog; 0123456789.\n"
    "Pack my box with five dozen liquor jugs!\n"
    "Il1| O0o  rn/m  cl/d  8B3E  5S2Z  ?!,;:'\"()\n\n"
    "It was a bright cold day in April, and the clocks were striking "
    "thirteen. Winston Smith, his chin nuzzled into his breast in an "
    "effort to escape the vile wind, slipped quickly through the glass "
    "doors of Victory Mansions, though not quickly enough to prevent a "
    "swirl of gritty dust from entering along with him.";

// --- page rendering --------------------------------------------------------
String book_path() { return String(kBooksDir) + "/" + g_book; }

// Read one page starting at g_offset; sets g_next.
String read_page() {
  if (g_book.isEmpty()) {  // font test page
    g_book_size = strlen(kSample);
    g_next = g_book_size;
    return String(kSample);
  }
  File f = SD_MMC.open(book_path(), "r");
  if (!f) return "(book vanished - Esc)";
  g_book_size = f.size();
  const int want = kPageBytes[g_size];
  f.seek(g_offset);
  uint8_t buf[900];
  const int got = f.read(buf, std::min((size_t)(want + 100), sizeof buf));
  f.close();
  if (got <= 0) return "(end)";

  int cut = std::min(got, want);
  if (g_offset + (size_t)cut < g_book_size) {
    // Snap back to the last whitespace so words don't split across pages.
    int ws = cut;
    while (ws > want / 2 && buf[ws - 1] != ' ' && buf[ws - 1] != '\n') ws--;
    if (ws > want / 2) cut = ws;
  }
  g_next = g_offset + (size_t)cut;

  String s;
  s.reserve(cut + 1);
  for (int i = 0; i < cut; i++)
    if (buf[i] != '\r') s += (char)buf[i];
  return s;
}

void show_page() {
  if (!g_page_lbl) return;
  lv_label_set_text(g_page_lbl, read_page().c_str());
  char h[48];
  if (g_book.isEmpty()) {
    snprintf(h, sizeof h, "font test - S/M/L = PixelOperator 16/32/48");
  } else {
    const int pct =
        g_book_size ? (int)((uint64_t)g_offset * 100 / g_book_size) : 0;
    snprintf(h, sizeof h, "%.24s   %d%%", g_book.c_str(), pct);
  }
  lv_label_set_text(g_hdr_lbl, h);
}

void size_set(int idx) {
  if (idx < 0) idx = 0;
  if (idx > 2) idx = 2;
  g_size = idx;
  config_set_text_size(idx);
  if (g_page_lbl) lv_obj_set_style_text_font(g_page_lbl, kFonts[idx], 0);
  for (int i = 0; i < 3; i++) {
    if (!g_size_lbl[i]) continue;
    const bool on = i == idx;
    lv_obj_set_style_bg_color(g_size_lbl[i],
                              on ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_size_lbl[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(g_size_lbl[i],
                                on ? lv_color_white() : lv_color_black(), 0);
  }
  show_page();  // repaginate from the current offset
}

void page_close_to_list() {
  if (!g_book.isEmpty()) save_pos(g_book, g_offset);
  g_page_lbl = g_hdr_lbl = nullptr;
  g_book = "";
  lv_obj_t* ps = g_page_scr;
  g_page_scr = nullptr;
  build_list();
  if (ps) lv_obj_del_async(ps);
}

void page_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  if (k == LV_KEY_RIGHT || k == LV_KEY_DOWN || k == ' ' || k == LV_KEY_ENTER ||
      k == LV_KEY_NEXT) {
    if (g_next < g_book_size) {
      g_history.push_back(g_offset);
      g_offset = g_next;
      show_page();
    }
  } else if (k == LV_KEY_LEFT || k == LV_KEY_UP || k == LV_KEY_PREV) {
    if (!g_history.empty()) {
      g_offset = g_history.back();
      g_history.pop_back();
      show_page();
    } else if (g_offset > 0) {
      g_offset = 0;  // history exhausted (opened mid-book): jump to the top
      show_page();
    }
  } else if (k == 't' || k == 'T') {
    g_history.clear();
    g_offset = 0;
    show_page();
  } else if (k == 's' || k == 'S') {
    size_set((g_size + 1) % 3);
  } else if (k == LV_KEY_ESC) {
    page_close_to_list();
  }
}

void open_page_screen() {
  g_page_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_page_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_page_scr, LV_OBJ_FLAG_CLICKABLE);

  g_hdr_lbl = lv_label_create(g_page_scr);
  lv_obj_set_style_text_font(g_hdr_lbl, &pixel_operator_16, 0);
  lv_obj_align(g_hdr_lbl, LV_ALIGN_TOP_LEFT, 8, 4);

  g_page_lbl = lv_label_create(g_page_scr);
  lv_obj_set_width(g_page_lbl, ST7305_W - 16);
  lv_obj_set_pos(g_page_lbl, 8, 24);
  lv_label_set_long_mode(g_page_lbl, LV_LABEL_LONG_CLIP);
  lv_obj_set_height(g_page_lbl, ST7305_H - 24 - 26);
  lv_obj_set_style_text_color(g_page_lbl, lv_color_black(), 0);

  // Bottom bar: keys legend + S/M/L chips (S key cycles; kept non-focusable —
  // the page owns all keys, one focus stop total).
  lv_obj_t* bar = lv_obj_create(g_page_scr);
  lv_obj_set_size(bar, ST7305_W, 24);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_color(bar, lv_color_black(), 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_hor(bar, 8, 0);
  lv_obj_set_style_pad_ver(bar, 1, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(bar, 6, 0);

  lv_obj_t* cap = lv_label_create(bar);
  lv_obj_set_style_text_font(cap, &pixel_operator_16, 0);
  lv_label_set_text(cap, LV_SYMBOL_RIGHT "=next  " LV_SYMBOL_LEFT
                                         "=prev  T=top  S=size");
  for (int i = 0; i < 3; i++) {
    lv_obj_t* l = lv_label_create(bar);
    lv_obj_set_style_text_font(l, &pixel_operator_16, 0);
    lv_label_set_text(l, kSizeName[i]);
    lv_obj_set_style_pad_hor(l, 6, 0);
    lv_obj_set_style_radius(l, 2, 0);
    g_size_lbl[i] = l;
  }

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);
  lv_obj_add_event_cb(g_page_scr, page_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(g, g_page_scr);

  lv_scr_load(g_page_scr);
  lv_group_focus_obj(g_page_scr);
  g_size = config_get_text_size();
  size_set(g_size);  // applies font + renders the first page
}

void open_book(const String& name) {
  if (!storage_sd_mount()) { g_status = "SD not available"; build_list(); return; }
  g_book = name;
  g_offset = load_pos(name);
  g_history.clear();
  open_page_screen();
}

void open_sample() {  // font test page: no SD needed
  g_book = "";
  g_offset = 0;
  g_history.clear();
  open_page_screen();
}

// --- book list -------------------------------------------------------------
void row_focus_cb(lv_event_t* e) {
  lv_obj_t* row = lv_event_get_target(e);
  const bool f = lv_event_get_code(e) == LV_EVENT_FOCUSED;
  lv_obj_set_style_bg_color(row, f ? lv_color_black() : lv_color_white(), 0);
  lv_obj_set_style_bg_opa(row, f ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(row); i++)
    lv_obj_set_style_text_color(lv_obj_get_child(row, i),
                                f ? lv_color_white() : lv_color_black(), 0);
}

std::vector<String> g_books;

void row_click_cb(lv_event_t* e) {
  const int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx == -2) { open_sample(); return; }  // font test page
  if (idx < 0) { build_list(); return; }     // "rescan" row
  if (idx < (int)g_books.size()) open_book(g_books[idx]);
}

void row_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC)
    launcher_go_home();
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
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* l = lv_label_create(row);
  lv_label_set_text(l, left.c_str());
  lv_obj_set_style_text_color(l, lv_color_black(), 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, ST7305_W - (right.isEmpty() ? 30 : 90));
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
  g_books.clear();
  bool sd_ok = storage_sd_mount();
  if (sd_ok) {
    if (!SD_MMC.exists(kBooksDir)) SD_MMC.mkdir(kBooksDir);
    File d = SD_MMC.open(kBooksDir);
    for (File f = d ? d.openNextFile() : File(); f; f = d.openNextFile()) {
      if (f.isDirectory()) continue;
      String nm = f.name();
      const int slash = nm.lastIndexOf('/');
      if (slash >= 0) nm = nm.substring(slash + 1);
      if (nm.startsWith(".")) continue;
      String l = nm;
      l.toLowerCase();
      if (l.endsWith(".txt") || l.endsWith(".md")) g_books.push_back(nm);
    }
    std::sort(g_books.begin(), g_books.end(),
              [](const String& a, const String& b) { return a.compareTo(b) < 0; });
  }

  lv_obj_t* old = g_list_scr;
  g_list_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_list_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_list_scr);
  lv_obj_set_style_text_font(title, &pixel_operator_bold_16, 0);
  lv_label_set_text(title, "Reader");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* hint = lv_label_create(g_list_scr);
  lv_label_set_text(hint, g_status.isEmpty() ? "books: SD /books (*.txt)"
                                             : g_status.c_str());
  g_status = "";
  lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -8, 10);

  lv_obj_t* cont = lv_obj_create(g_list_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 36);
  lv_obj_set_pos(cont, 0, 36);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(cont, 0, 0);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* first =
      make_row(cont, "Aa  Font test page", "S/M/L", -2);
  for (int i = 0; i < (int)g_books.size() && i < 30; i++) {
    const size_t pos = load_pos(g_books[i]);
    make_row(cont, g_books[i], pos ? "resume" : "", i);
  }
  if (g_books.empty()) {
    lv_obj_t* empty = lv_label_create(cont);
    lv_label_set_text(empty,
                      sd_ok ? "\n   no books - copy .txt into /books on the\n"
                              "   SD card (Files app: USB transfer)"
                            : "\n   SD card not available");
    lv_obj_set_style_text_color(empty, lv_color_black(), 0);
    make_row(cont, LV_SYMBOL_REFRESH "  rescan", "", -1);
  }

  lv_scr_load(g_list_scr);
  lv_group_focus_obj(first);
  if (old) lv_obj_del_async(old);
}

void app_teardown() {
  if (!g_book.isEmpty()) save_pos(g_book, g_offset);
  g_book = "";
  g_page_lbl = g_hdr_lbl = nullptr;
  if (g_page_scr) { lv_obj_del_async(g_page_scr); g_page_scr = nullptr; }
  if (g_list_scr) { lv_obj_del_async(g_list_scr); g_list_scr = nullptr; }
}

}  // namespace

void reader_open() {
  launcher_set_leave_hook(app_teardown);
  g_list_scr = g_page_scr = nullptr;
  build_list();
}
