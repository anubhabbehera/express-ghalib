/**
 * music.cpp — Music player (M4). Steps 1-2: mount microSD, browse /music, and
 * stream 16-bit PCM WAV to the ES8311 (via audio.cpp). MP3 decode is step 3;
 * transport controls (pause/next/seek/volume) are step 4.
 *
 * Playback is driven from loop() via music_task(), one small chunk per call, so
 * the UI / BLE / reminders stay responsive between chunks and Esc can stop it
 * (a blocking play loop can't call lv_timer_handler — it's non-reentrant). SDMMC
 * is a separate peripheral from the display SPI and audio I2S: no bus contention.
 */
#include "music.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <lvgl.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include "audio.h"
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
// WAV parsing (16-bit PCM). Leaves the file positioned at the start of `data`.
// ---------------------------------------------------------------------------
struct WavInfo { uint32_t rate; uint16_t channels; uint16_t bits; uint32_t data_size; };

bool parse_wav(File& f, WavInfo& w) {
  char tag[4];
  uint32_t sz;
  if (f.read((uint8_t*)tag, 4) != 4 || memcmp(tag, "RIFF", 4)) return false;
  f.seek(f.position() + 4);                                   // skip RIFF size
  if (f.read((uint8_t*)tag, 4) != 4 || memcmp(tag, "WAVE", 4)) return false;

  bool have_fmt = false;
  while (f.available() >= 8) {
    if (f.read((uint8_t*)tag, 4) != 4) break;
    if (f.read((uint8_t*)&sz, 4) != 4) break;                 // little-endian
    if (!memcmp(tag, "fmt ", 4)) {
      uint16_t fmt, ch, blk, bits;
      uint32_t sr, br;
      f.read((uint8_t*)&fmt, 2); f.read((uint8_t*)&ch, 2);
      f.read((uint8_t*)&sr, 4);  f.read((uint8_t*)&br, 4);
      f.read((uint8_t*)&blk, 2); f.read((uint8_t*)&bits, 2);
      if (sz > 16) f.seek(f.position() + (sz - 16));          // skip fmt extras
      if (fmt != 1) { Serial.printf("[MUS] WAV not PCM (fmt=%u)\n", fmt); return false; }
      w.rate = sr; w.channels = ch; w.bits = bits;
      have_fmt = true;
    } else if (!memcmp(tag, "data", 4)) {
      if (!have_fmt) return false;
      w.data_size = sz;
      return true;                                            // positioned at data
    } else {
      f.seek(f.position() + sz + (sz & 1));                   // skip chunk (even pad)
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Playback state (advanced one chunk per music_task() call from loop())
// ---------------------------------------------------------------------------
File          g_file;
bool          g_playing  = false;
volatile bool g_stop     = false;
uint32_t      g_remaining = 0;          // bytes left in the WAV data chunk
uint16_t      g_channels  = 2;
String        g_now_name;

lv_obj_t* g_scr = nullptr;              // current music screen (browser or now-playing)
std::vector<String> g_tracks;          // outlives rows (labels/ud point at c_str())

void build_browser();
void build_now_playing();

void stop_playback() {
  audio_play_off();
  g_playing = false;
  if (g_file) g_file.close();
  Serial.println("[MUS] stopped");
}

void start_playback(const String& name) {
  String path = String("/music/") + name;
  g_file = SD_MMC.open(path);
  if (!g_file) { Serial.printf("[MUS] open failed: %s\n", path.c_str()); return; }

  WavInfo w;
  if (!parse_wav(g_file, w) || w.bits != 16) {
    Serial.println("[MUS] unsupported (need 16-bit PCM WAV; MP3 is step 3)");
    g_file.close();
    return;
  }
  if (!audio_prepare(w.rate)) { g_file.close(); return; }

  g_channels = w.channels;
  g_remaining = w.data_size;
  g_stop = false;
  g_now_name = name;
  audio_play_on();
  g_playing = true;
  Serial.printf("[MUS] play %s: %luHz %uch 16bit, %lu bytes\n", name.c_str(),
                (unsigned long)w.rate, w.channels, (unsigned long)w.data_size);
  build_now_playing();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
void teardown() {
  if (g_playing) stop_playback();        // Home/leave while playing -> stop audio
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
  if (name) start_playback(String(name));   // -> now-playing screen
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

// Now-playing: Esc stops and returns to the list (music_task does the teardown).
void np_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) g_stop = true;
}

// Full-width focusable row (lv_list is invisible on the 1-bit panel).
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
  if (click_cb) lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, ud);
  lv_group_add_obj(lv_group_get_default(), row);
  return row;
}

void build_browser() {
  lv_obj_t* old = g_scr;
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
                     nullptr, nullptr, list_key_cb);
  } else {
    g_tracks = list_tracks();
    Serial.printf("[MUS] /music: %u track(s)\n", (unsigned)g_tracks.size());
    if (g_tracks.empty()) {
      first = make_row(cont, "(no tracks in /music)", nullptr, nullptr, list_key_cb);
    } else {
      int shown = 0;
      for (const String& t : g_tracks) {
        lv_obj_t* row = make_row(cont, t.c_str(), (void*)t.c_str(),
                                 track_click_cb, list_key_cb);
        if (!first) first = row;
        if (++shown >= 16) break;
      }
    }
  }

  lv_scr_load(g_scr);
  if (first) lv_group_focus_obj(first);
  if (old) lv_obj_del_async(old);
}

void build_now_playing() {
  lv_obj_t* old = g_scr;
  g_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* hdr = lv_label_create(g_scr);
  lv_label_set_text(hdr, LV_SYMBOL_PLAY "  Now playing");
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_16, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t* name = lv_label_create(g_scr);
  lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
  lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(name, ST7305_W - 40);
  lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(name, g_now_name.c_str());
  lv_obj_align(name, LV_ALIGN_CENTER, 0, 0);

  // A focusable (invisible) object carries the Esc-to-stop key event.
  lv_obj_t* k = lv_obj_create(g_scr);
  lv_obj_set_size(k, 1, 1);
  lv_obj_set_style_border_width(k, 0, 0);
  lv_obj_set_style_bg_opa(k, LV_OPA_TRANSP, 0);
  lv_obj_add_event_cb(k, np_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_remove_all_objs(lv_group_get_default());
  lv_group_add_obj(lv_group_get_default(), k);
  lv_group_focus_obj(k);

  lv_obj_t* hint = lv_label_create(g_scr);
  lv_label_set_text(hint, "Esc / KEY = stop");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  lv_scr_load(g_scr);
  if (old) lv_obj_del_async(old);
}

}  // namespace

void music_open() {
  launcher_set_leave_hook(teardown);
  g_scr = nullptr;
  build_browser();
}

void music_task() {
  if (!g_playing) return;
  if (g_stop || g_remaining == 0) {           // stopped by user or reached EOF
    stop_playback();
    build_browser();                          // back to the track list
    return;
  }
  static uint8_t rbuf[2048];
  const size_t want = g_remaining < sizeof(rbuf) ? g_remaining : sizeof(rbuf);
  const int n = g_file.read(rbuf, want);
  if (n <= 0) { stop_playback(); build_browser(); return; }
  g_remaining -= n;

  if (g_channels == 1) {                       // expand mono -> stereo (L==R)
    static int16_t obuf[2048];                 // holds up to 1024 frames
    const int samples = n / 2;
    const int16_t* in = (const int16_t*)rbuf;
    for (int i = 0; i < samples; i++) { obuf[2 * i] = in[i]; obuf[2 * i + 1] = in[i]; }
    audio_write((uint8_t*)obuf, samples * 4);
  } else {
    audio_write(rbuf, n);
  }
}
