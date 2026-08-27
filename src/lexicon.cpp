/**
 * lexicon.cpp — offline dictionary. See lexicon.h.
 *
 * Lookup: normalize the query (lowercase, spaces -> _), find its two-letter
 * bucket in the RAM-cached dict.idx, then scan just that bucket of dict.txt
 * with buffered reads. Lines are sorted within a bucket, so the scan stops
 * as soon as keys pass the query. On a miss, nearby keys become suggestions.
 */
#include "lexicon.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <lvgl.h>
#include <algorithm>
#include <vector>

#include "launcher.h"
#include "st7305.h"
#include "storage.h"

namespace {

constexpr const char* kDictPath = "/lexicon/dict.txt";
constexpr const char* kIdxPath = "/lexicon/dict.idx";

// Both files come off the SD card, i.e. they are arbitrary input. Reading a
// "line" from one with no newline in it would grow a single String until the
// heap runs out, so every line read here is capped. Real entries are ~100
// bytes; anything past the cap is dropped, not buffered.
constexpr unsigned kMaxLine = 512;

// Bounded replacement for File::readStringUntil('\n').
String read_line_capped(File& f) {
  String line;
  line.reserve(80);
  while (f.available()) {
    const char c = (char)f.read();
    if (c == '\n') break;
    if (line.length() < kMaxLine) line += c;
  }
  return line;
}

// ---------------------------------------------------------------------------
// Index + lookup
// ---------------------------------------------------------------------------
struct Bucket { char b0, b1; uint32_t off; };
std::vector<Bucket> g_idx;   // loaded once per app entry
size_t g_dict_size = 0;

// Must match tools/build_lexicon.py: a-z pass through, everything else '_';
// keys shorter than 2 chars pad with '_'.
char bch(char c) { return (c >= 'a' && c <= 'z') ? c : '_'; }

String normalize(String q) {
  q.trim();
  q.toLowerCase();
  q.replace(" ", "_");
  return q;
}

bool load_idx() {
  if (!g_idx.empty()) return true;
  if (!storage_sd_mount()) return false;
  File f = SD_MMC.open(kIdxPath, "r");
  if (!f) return false;
  while (f.available()) {
    String line = read_line_capped(f);
    if (line.length() < 4) continue;
    g_idx.push_back({line[0], line[1],
                     (uint32_t)line.substring(3).toInt()});
  }
  f.close();
  File d = SD_MMC.open(kDictPath, "r");
  if (!d) { g_idx.clear(); return false; }
  g_dict_size = d.size();
  d.close();
  Serial.printf("[lex] idx: %u buckets, dict %uK\n", (unsigned)g_idx.size(),
                (unsigned)(g_dict_size / 1024));
  return !g_idx.empty();
}

// Byte range [start, end) of the query's bucket.
bool bucket_range(const String& key, uint32_t& start, uint32_t& end) {
  const char b0 = bch(key[0]);
  const char b1 = bch(key.length() > 1 ? key[1] : '_');
  for (size_t i = 0; i < g_idx.size(); i++) {
    if (g_idx[i].b0 == b0 && g_idx[i].b1 == b1) {
      start = g_idx[i].off;
      end = i + 1 < g_idx.size() ? g_idx[i + 1].off : (uint32_t)g_dict_size;
      return true;
    }
  }
  return false;
}

// Scan the bucket for exact-key lines; on miss fill `nearby` with the keys
// that would have sorted around the query.
bool lookup(const String& key, std::vector<String>& defs,
            std::vector<String>& nearby) {
  defs.clear();
  nearby.clear();
  uint32_t start, end;
  if (!bucket_range(key, start, end)) return false;

  File f = SD_MMC.open(kDictPath, "r");
  if (!f) return false;
  f.seek(start);
  String line;
  line.reserve(160);
  uint8_t buf[2048];
  uint32_t pos = start;
  bool done = false;
  while (pos < end && !done) {
    const int got = f.read(buf, std::min((uint32_t)sizeof buf, end - pos));
    if (got <= 0) break;
    pos += got;
    for (int i = 0; i < got && !done; i++) {
      const char c = (char)buf[i];
      if (c != '\n') {
        if (line.length() < kMaxLine) line += c;   // cap: see kMaxLine
        continue;
      }
      const int tab = line.indexOf('\t');
      if (tab > 0) {
        const String k = line.substring(0, tab);
        if (k == key) {
          defs.push_back(line.substring(tab + 1));
        } else if (k > key) {
          if (defs.empty() && nearby.size() < 6) nearby.push_back(k);
          else done = true;  // sorted: nothing more to find
          if (nearby.size() >= 6) done = true;
        }
      }
      line = "";
    }
  }
  f.close();
  return !defs.empty();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
lv_obj_t* g_scr = nullptr;
lv_obj_t* g_input = nullptr;
lv_obj_t* g_word_lbl = nullptr;   // "word  (2/3)"
lv_obj_t* g_def_lbl = nullptr;    // definition text
std::vector<String> g_defs;
String g_word;
int g_sense = 0;

void show_sense() {
  if (!g_def_lbl) return;
  if (g_defs.empty()) return;
  char h[64];
  if (g_defs.size() > 1)
    snprintf(h, sizeof h, "%.32s  (%d/%d - Up/Dn)", g_word.c_str(),
             g_sense + 1, (int)g_defs.size());
  else
    snprintf(h, sizeof h, "%.40s", g_word.c_str());
  lv_label_set_text(g_word_lbl, h);
  lv_label_set_text(g_def_lbl, g_defs[g_sense].c_str());
}

void do_lookup() {
  if (!g_input) return;
  const String key = normalize(lv_textarea_get_text(g_input));
  if (key.isEmpty()) return;
  if (!load_idx()) {
    lv_label_set_text(g_word_lbl, "no dictionary");
    lv_label_set_text(g_def_lbl,
                      "build with tools/build_lexicon.py and copy\n"
                      "/lexicon/dict.txt + dict.idx to the SD card\n"
                      "(Files app: USB transfer)");
    return;
  }
  std::vector<String> nearby;
  if (lookup(key, g_defs, nearby)) {
    g_word = key;
    g_sense = 0;
    show_sense();
  } else {
    g_defs.clear();
    lv_label_set_text_fmt(g_word_lbl, "'%s' not found", key.c_str());
    String s = "nearby:";
    for (const String& n : nearby) s += "\n  " + n;
    if (nearby.empty()) s = "(nothing nearby either)";
    lv_label_set_text(g_def_lbl, s.c_str());
  }
}

void input_ready_cb(lv_event_t*) { do_lookup(); }  // fires in-place: no leak

void input_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  if (k == LV_KEY_ESC) {
    launcher_go_home();
  } else if (k == LV_KEY_DOWN && g_defs.size() > 1) {
    g_sense = (g_sense + 1) % (int)g_defs.size();
    show_sense();
  } else if (k == LV_KEY_UP && g_defs.size() > 1) {
    g_sense = (g_sense + (int)g_defs.size() - 1) % (int)g_defs.size();
    show_sense();
  }
}

void build_ui() {
  g_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_scr);
  lv_obj_set_style_text_font(title, &pixel_operator_bold_16, 0);
  lv_label_set_text(title, "Lexicon");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* ta = lv_textarea_create(g_scr);
  g_input = ta;
  lv_obj_set_size(ta, ST7305_W - 16, 30);
  lv_obj_set_pos(ta, 8, 34);
  lv_textarea_set_one_line(ta, true);
  lv_obj_set_style_radius(ta, 2, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, lv_color_black(), 0);
  lv_obj_set_style_pad_all(ta, 4, 0);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);
  lv_textarea_set_placeholder_text(ta, "word + Enter");
  lv_obj_add_event_cb(ta, input_ready_cb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(ta, input_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(g, ta);

  g_word_lbl = lv_label_create(g_scr);
  lv_obj_set_style_text_font(g_word_lbl, &pixel_operator_bold_16, 0);
  lv_obj_set_pos(g_word_lbl, 12, 76);
  lv_label_set_text(g_word_lbl, "");

  lv_obj_t* box = lv_obj_create(g_scr);
  lv_obj_set_size(box, ST7305_W - 16, ST7305_H - 104);
  lv_obj_set_pos(box, 8, 100);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_pad_all(box, 2, 0);

  g_def_lbl = lv_label_create(box);
  lv_obj_set_width(g_def_lbl, ST7305_W - 24);
  lv_obj_set_style_text_color(g_def_lbl, lv_color_black(), 0);
  lv_label_set_text(g_def_lbl, "type a word, Enter looks it up\nEsc = home");

  lv_scr_load(g_scr);
  lv_group_focus_obj(ta);
}

void app_teardown() {
  if (g_scr) { lv_obj_del_async(g_scr); g_scr = nullptr; }
  g_input = g_word_lbl = g_def_lbl = nullptr;
  g_defs.clear();
  g_idx.clear();          // free the index; reloaded on next app entry
  g_idx.shrink_to_fit();
}

}  // namespace

void lexicon_open() {
  launcher_set_leave_hook(app_teardown);
  g_scr = nullptr;
  build_ui();
}
