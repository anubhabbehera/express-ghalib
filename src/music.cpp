/**
 * music.cpp — Music player (M4), step 1: mount microSD + browse /music.
 * See music.h. Playback (WAV via audio.cpp, then MP3) lands in later steps.
 *
 * The SD card is SDMMC 1-bit (CLK38/CMD21/D0 39) on a peripheral separate from
 * the display SPI and the audio I2S, so there's no bus contention. It's mounted
 * lazily on first open and kept mounted.
 */
#include "music.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <lvgl.h>
#include <algorithm>
#include <vector>
#include "launcher.h"
#include "st7305.h"

namespace {
constexpr int SD_CLK = 38, SD_CMD = 21, SD_D0 = 39;

bool g_mounted = false;

bool ensure_mounted() {
  if (g_mounted) return true;
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true /*1-bit*/, false /*no format*/)) {
    Serial.println("[MUS] SD mount failed (card inserted? FAT32?)");
    return false;
  }
  g_mounted = true;
  Serial.printf("[MUS] SD mounted: type=%d size=%lluMB\n", SD_MMC.cardType(),
                SD_MMC.cardSize() / (1024ULL * 1024ULL));
  return true;
}

bool is_audio(const String& n) {
  String l = n;
  l.toLowerCase();
  return l.endsWith(".wav") || l.endsWith(".mp3");
}

std::vector<String> list_tracks() {
  std::vector<String> v;
  File dir = SD_MMC.open("/music");
  if (!dir || !dir.isDirectory()) return v;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String nm = f.name();
    const int slash = nm.lastIndexOf('/');
    if (slash >= 0) nm = nm.substring(slash + 1);
    // Skip hidden / macOS AppleDouble sidecars (e.g. "._song.wav", ".DS_Store")
    // that a Mac drops next to each copied file — they'd double the list.
    if (nm.startsWith(".")) continue;
    if (is_audio(nm)) { Serial.printf("[MUS] track: %s\n", nm.c_str()); v.push_back(nm); }
  }
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());  // collapse any dup names
  return v;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
lv_obj_t* g_scr = nullptr;
std::vector<String> g_tracks;  // outlives rows (row labels point at c_str())

void teardown() {
  if (g_scr) { lv_obj_del_async(g_scr); g_scr = nullptr; }
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

void track_click_cb(lv_event_t* e) {
  const char* name = static_cast<const char*>(lv_event_get_user_data(e));
  if (name) Serial.printf("[MUS] selected: %s (playback = step 2)\n", name);
}

void list_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC)
    launcher_go_home();  // -> teardown()
}

// Full-width focusable row (lv_list is invisible on the 1-bit panel).
lv_obj_t* make_row(lv_obj_t* parent, const char* text, void* ud) {
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
  lv_obj_add_event_cb(row, track_click_cb, LV_EVENT_CLICKED, ud);
  lv_group_add_obj(lv_group_get_default(), row);
  return row;
}

}  // namespace

void music_open() {
  launcher_set_leave_hook(teardown);
  g_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_label_set_text(title, LV_SYMBOL_AUDIO "  Music");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* cont = lv_obj_create(g_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 36);
  lv_obj_set_pos(cont, 0, 36);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  // A focusable status/empty row is always present, so Esc always has a target.
  lv_obj_t* first = nullptr;
  if (!ensure_mounted()) {
    first = make_row(cont, LV_SYMBOL_WARNING "  No SD card  (Esc = back)",
                     (void*)nullptr);
  } else {
    g_tracks = list_tracks();
    Serial.printf("[MUS] /music: %u track(s)\n", (unsigned)g_tracks.size());
    if (g_tracks.empty()) {
      first = make_row(cont, "(no tracks in /music)", (void*)nullptr);
    } else {
      int shown = 0;
      for (const String& t : g_tracks) {
        lv_obj_t* row = make_row(cont, t.c_str(), (void*)t.c_str());
        if (!first) first = row;
        if (++shown >= 16) break;
      }
    }
  }

  lv_scr_load(g_scr);
  if (first) lv_group_focus_obj(first);
}
