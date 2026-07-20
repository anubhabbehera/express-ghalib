/**
 * music.cpp — Music player (M4). Browse /music, stream WAV (16-bit PCM) and MP3
 * (Helix decoder) to the ES8311, with a now-playing screen: play/pause,
 * prev/next, volume, progress bar, auto-advance.
 *
 * Threading: playback runs in a dedicated FreeRTOS task pinned to core 0, so it
 * keeps I2S fed even while the UI loop on core 1 does long blocking work (the
 * reflective panel's full-screen SPI flush, or the reminder poll's LittleFS
 * read). The UI sends commands via a queue and polls player state for display;
 * it never touches the audio path directly. This is what keeps MP3 glitch-free.
 *
 * SDMMC is a separate peripheral from the display SPI and audio I2S — no bus
 * contention. The FAT VFS serializes the rare concurrent SD access.
 */
#include "music.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include "MP3DecoderHelix.h"
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
  String l = n; l.toLowerCase();
  return l.endsWith(".wav") || l.endsWith(".mp3");
}
bool is_mp3(const String& n) {
  String l = n; l.toLowerCase();
  return l.endsWith(".mp3");
}

// Shared track list: written by the UI (browser), read by the audio task.
std::vector<String> g_tracks;
SemaphoreHandle_t   g_mux = nullptr;

void tracks_reload() {                          // caller holds g_mux
  g_tracks.clear();
  File dir = SD_MMC.open("/music");
  if (!dir || !dir.isDirectory()) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String nm = f.name();
    const int slash = nm.lastIndexOf('/');
    if (slash >= 0) nm = nm.substring(slash + 1);
    // Skip hidden / macOS AppleDouble sidecars ("._song.wav") — they'd double up.
    if (nm.startsWith(".")) continue;
    if (is_audio(nm)) g_tracks.push_back(nm);
  }
  std::sort(g_tracks.begin(), g_tracks.end());
  g_tracks.erase(std::unique(g_tracks.begin(), g_tracks.end()), g_tracks.end());
}

// --- WAV parsing (16-bit PCM); leaves the file positioned at `data` ----------
struct WavInfo { uint32_t rate; uint16_t channels; uint16_t bits; uint32_t data_size; };

bool parse_wav(File& f, WavInfo& w) {
  char tag[4];
  uint32_t sz;
  if (f.read((uint8_t*)tag, 4) != 4 || memcmp(tag, "RIFF", 4)) return false;
  f.seek(f.position() + 4);
  if (f.read((uint8_t*)tag, 4) != 4 || memcmp(tag, "WAVE", 4)) return false;
  bool have_fmt = false;
  while (f.available() >= 8) {
    if (f.read((uint8_t*)tag, 4) != 4) break;
    if (f.read((uint8_t*)&sz, 4) != 4) break;
    if (!memcmp(tag, "fmt ", 4)) {
      uint16_t fmt, ch, blk, bits;
      uint32_t sr, br;
      f.read((uint8_t*)&fmt, 2); f.read((uint8_t*)&ch, 2);
      f.read((uint8_t*)&sr, 4);  f.read((uint8_t*)&br, 4);
      f.read((uint8_t*)&blk, 2); f.read((uint8_t*)&bits, 2);
      if (sz > 16) f.seek(f.position() + (sz - 16));
      if (fmt != 1) { Serial.printf("[MUS] WAV not PCM (fmt=%u)\n", fmt); return false; }
      w.rate = sr; w.channels = ch; w.bits = bits;
      have_fmt = true;
    } else if (!memcmp(tag, "data", 4)) {
      if (!have_fmt) return false;
      w.data_size = sz;
      return true;
    } else {
      f.seek(f.position() + sz + (sz & 1));
    }
  }
  return false;
}

// --- MP3: Helix streams decoded PCM to this callback -> I2S ------------------
void mp3_data_cb(MP3FrameInfo& info, short* pcm, size_t len, void*) {
  if (info.samprate > 0) audio_prepare((uint32_t)info.samprate);
  if (info.nChans >= 2) {
    audio_write((const uint8_t*)pcm, len * sizeof(short));
  } else {
    static int16_t ob[2 * 1152];
    size_t s = len < 1152 ? len : 1152;
    for (size_t i = 0; i < s; i++) { ob[2 * i] = pcm[i]; ob[2 * i + 1] = pcm[i]; }
    audio_write((const uint8_t*)ob, s * 2 * sizeof(int16_t));
  }
}
libhelix::MP3DecoderHelix g_mp3(mp3_data_cb);

// ---------------------------------------------------------------------------
// Player state — the audio task owns playback; UI reads these for display.
// Plain scalars with benign display races; only g_tracks needs the mutex.
// ---------------------------------------------------------------------------
enum Fmt { FMT_WAV, FMT_MP3 };
enum { C_PLAY, C_STOP, C_NEXT, C_PREV, C_PAUSE, C_VOLUP, C_VOLDN };
struct Cmd { uint8_t type; int arg; };

QueueHandle_t g_q = nullptr;
TaskHandle_t  g_task = nullptr;

volatile bool     g_playing = false;
volatile bool     g_paused  = false;
volatile int      g_index   = -1;
volatile int      g_count   = 0;
volatile int      g_volume  = 75;
volatile uint32_t g_pos = 0, g_size = 0;
char              g_name[64] = {0};

// Task-local playback handles (touched only by the audio task).
File     g_tf;
Fmt      g_tfmt = FMT_WAV;
uint32_t g_trem = 0;      // WAV bytes remaining
uint16_t g_tch  = 2;      // WAV channels

void send_cmd(uint8_t type, int arg = 0) {
  if (!g_q) return;
  Cmd c{type, arg};
  xQueueSend(g_q, &c, 0);
}

// --- audio task (core 0) ----------------------------------------------------
void task_close_file() {
  if (g_tfmt == FMT_MP3 && g_playing) g_mp3.end();
  if (g_tf) g_tf.close();
}

void task_end() {                 // fully stop: silence + release
  task_close_file();
  audio_play_off();
  g_playing = false;
  g_paused = false;
  g_pos = g_size = 0;
}

// Open + start track i; out-of-range (past either end) ends playback.
void task_open(int i) {
  task_close_file();
  String name;
  int count;
  xSemaphoreTake(g_mux, portMAX_DELAY);
  count = (int)g_tracks.size();
  if (i >= 0 && i < count) name = g_tracks[i];
  xSemaphoreGive(g_mux);
  if (name.isEmpty()) { task_end(); return; }

  g_tf = SD_MMC.open(String("/music/") + name);
  if (!g_tf) { Serial.printf("[MUS] open failed: %s\n", name.c_str()); task_end(); return; }

  if (is_mp3(name)) {
    g_tfmt = FMT_MP3;
    g_mp3.begin();
  } else {
    WavInfo w;
    if (!parse_wav(g_tf, w) || w.bits != 16) {
      Serial.println("[MUS] unsupported WAV (need 16-bit PCM)");
      g_tf.close();
      task_end();
      return;
    }
    audio_prepare(w.rate);
    g_tfmt = FMT_WAV;
    g_tch = w.channels;
    g_trem = w.data_size;
  }
  audio_set_volume(g_volume);
  audio_play_on();
  g_index = i;
  g_count = count;
  strncpy(g_name, name.c_str(), sizeof(g_name) - 1);
  g_name[sizeof(g_name) - 1] = 0;
  g_size = g_tf.size();
  g_pos = g_tf.position();
  g_playing = true;
  g_paused = false;
  Serial.printf("[MUS] play [%d/%d] %s\n", i + 1, count, name.c_str());
}

void handle_cmd(const Cmd& c) {
  switch (c.type) {
    case C_PLAY:  task_open(c.arg); break;
    case C_STOP:  task_end(); break;
    case C_NEXT:  task_open(g_index + 1); break;
    case C_PREV:  task_open(g_index - 1); break;
    case C_PAUSE: g_paused = !g_paused; break;
    case C_VOLUP: g_volume = g_volume + 10 > 100 ? 100 : g_volume + 10;
                  audio_set_volume(g_volume); break;
    case C_VOLDN: g_volume = g_volume - 10 < 0 ? 0 : g_volume - 10;
                  audio_set_volume(g_volume); break;
  }
}

void stream_chunk() {
  if (g_tfmt == FMT_WAV) {
    if (g_trem == 0) { task_open(g_index + 1); return; }        // auto-advance
    static uint8_t rbuf[2048];
    const size_t want = g_trem < sizeof(rbuf) ? g_trem : sizeof(rbuf);
    const int n = g_tf.read(rbuf, want);
    if (n <= 0) { task_open(g_index + 1); return; }
    g_trem -= n;
    if (g_tch == 1) {                                           // mono -> stereo
      static int16_t obuf[2048];
      const int samples = n / 2;
      const int16_t* in = (const int16_t*)rbuf;
      for (int i = 0; i < samples; i++) { obuf[2 * i] = in[i]; obuf[2 * i + 1] = in[i]; }
      audio_write((uint8_t*)obuf, samples * 4);
    } else {
      audio_write(rbuf, n);
    }
  } else {                                                      // FMT_MP3
    static uint8_t mbuf[1024];
    const int n = g_tf.read(mbuf, sizeof(mbuf));
    if (n <= 0) { task_open(g_index + 1); return; }             // EOF -> next
    g_mp3.write(mbuf, n);
  }
  if (g_tf) g_pos = g_tf.position();
}

void audio_task(void*) {
  Cmd c;
  for (;;) {
    while (xQueueReceive(g_q, &c, 0) == pdTRUE) handle_cmd(c);
    if (!g_playing || g_paused) { vTaskDelay(pdMS_TO_TICKS(8)); continue; }
    stream_chunk();                          // blocks on I2S write -> paces us
  }
}

// ---------------------------------------------------------------------------
// UI (core 1) — browser + now-playing view. Sends commands, polls state.
// ---------------------------------------------------------------------------
lv_obj_t*  g_scr        = nullptr;
lv_obj_t*  g_bar        = nullptr;
lv_obj_t*  g_status_lbl = nullptr;
lv_obj_t*  g_vol_lbl    = nullptr;
lv_obj_t*  g_name_lbl   = nullptr;
lv_timer_t* g_np_timer  = nullptr;

void build_browser();
void build_now_playing();

void kill_np_timer() {
  if (g_np_timer) { lv_timer_del(g_np_timer); g_np_timer = nullptr; }
}

void teardown() {
  kill_np_timer();
  if (g_playing) send_cmd(C_STOP);           // Home/leave while playing -> stop
  if (g_scr) { lv_obj_del_async(g_scr); g_scr = nullptr; }
  g_bar = g_status_lbl = g_vol_lbl = g_name_lbl = nullptr;
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
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  send_cmd(C_PLAY, i);
  build_now_playing();
}

void list_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT)
    lv_group_focus_next(g);
  else if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV)
    lv_group_focus_prev(g);
  else if (k == LV_KEY_ESC)
    launcher_go_home();
}

// Now-playing transport: Enter/Space=pause, L/R=prev/next, U/D=vol, Esc=stop.
void np_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  if (k == LV_KEY_ESC) send_cmd(C_STOP);
  else if (k == LV_KEY_ENTER || k == ' ') send_cmd(C_PAUSE);
  else if (k == LV_KEY_RIGHT || k == LV_KEY_NEXT) send_cmd(C_NEXT);
  else if (k == LV_KEY_LEFT || k == LV_KEY_PREV) send_cmd(C_PREV);
  else if (k == LV_KEY_UP) send_cmd(C_VOLUP);
  else if (k == LV_KEY_DOWN) send_cmd(C_VOLDN);
}

lv_obj_t* make_row(lv_obj_t* parent, const char* text, void* ud, lv_event_cb_t click_cb) {
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
  lv_obj_add_event_cb(row, list_key_cb, LV_EVENT_KEY, nullptr);
  if (click_cb) lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, ud);
  lv_group_add_obj(lv_group_get_default(), row);
  return row;
}

void build_browser() {
  kill_np_timer();
  lv_obj_t* old = g_scr;
  g_scr = lv_obj_create(nullptr);
  g_bar = g_status_lbl = g_vol_lbl = g_name_lbl = nullptr;
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

  lv_obj_t* first = nullptr;
  if (!ensure_mounted()) {
    first = make_row(cont, LV_SYMBOL_WARNING "  No SD card  (Esc = back)", nullptr, nullptr);
  } else {
    xSemaphoreTake(g_mux, portMAX_DELAY);
    tracks_reload();
    const int n = (int)g_tracks.size();
    std::vector<String> snapshot = g_tracks;   // copy for row labels
    xSemaphoreGive(g_mux);
    g_count = n;
    Serial.printf("[MUS] /music: %d track(s)\n", n);
    if (n == 0) {
      first = make_row(cont, "(no tracks in /music)", nullptr, nullptr);
    } else {
      int shown = 0;
      for (int i = 0; i < n; i++) {
        lv_obj_t* row = make_row(cont, snapshot[i].c_str(), (void*)(intptr_t)i, track_click_cb);
        if (!first) first = row;
        if (++shown >= 16) break;
      }
    }
  }

  lv_scr_load(g_scr);
  if (first) lv_group_focus_obj(first);
  if (old) lv_obj_del_async(old);
}

// Poll player state (250 ms) and reflect it; when playback ends, go to the list.
void np_poll_cb(lv_timer_t* t) {
  if (!g_playing) {                            // stopped / reached end of list
    lv_timer_del(t);
    g_np_timer = nullptr;
    build_browser();
    return;
  }
  if (g_name_lbl) lv_label_set_text(g_name_lbl, g_name);
  if (g_status_lbl)
    lv_label_set_text(g_status_lbl,
                      (String(g_paused ? "Paused  " : "Playing  ") +
                       String((int)g_index + 1) + "/" + String((int)g_count)).c_str());
  if (g_vol_lbl) lv_label_set_text(g_vol_lbl, (String("Vol ") + (int)g_volume + "%").c_str());
  if (g_bar) lv_bar_set_value(g_bar, g_size ? (int)((uint64_t)g_pos * 100 / g_size) : 0, LV_ANIM_OFF);
}

void build_now_playing() {
  lv_obj_t* old = g_scr;
  g_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

  g_name_lbl = lv_label_create(g_scr);
  lv_obj_set_style_text_font(g_name_lbl, &lv_font_montserrat_20, 0);
  lv_label_set_long_mode(g_name_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_name_lbl, ST7305_W - 40);
  lv_obj_set_style_text_align(g_name_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(g_name_lbl, g_name);
  lv_obj_align(g_name_lbl, LV_ALIGN_TOP_MID, 0, 30);

  g_status_lbl = lv_label_create(g_scr);
  lv_label_set_text(g_status_lbl, "Playing");
  lv_obj_align(g_status_lbl, LV_ALIGN_CENTER, 0, -10);

  g_bar = lv_bar_create(g_scr);
  lv_obj_set_size(g_bar, ST7305_W - 48, 12);
  lv_obj_align(g_bar, LV_ALIGN_CENTER, 0, 20);
  lv_obj_set_style_bg_opa(g_bar, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_bar, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(g_bar, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(g_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_bar, lv_color_black(), LV_PART_INDICATOR);
  lv_obj_set_style_radius(g_bar, 0, LV_PART_INDICATOR);
  lv_bar_set_range(g_bar, 0, 100);
  lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);

  g_vol_lbl = lv_label_create(g_scr);
  lv_label_set_text(g_vol_lbl, "Vol");
  lv_obj_align(g_vol_lbl, LV_ALIGN_CENTER, 0, 50);

  lv_obj_t* hint = lv_label_create(g_scr);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_label_set_text(hint,
                    LV_SYMBOL_LEFT "/" LV_SYMBOL_RIGHT " prev/next   Enter pause\n"
                    LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " vol   Esc/KEY stop");
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

  lv_obj_t* kc = lv_obj_create(g_scr);
  lv_obj_set_size(kc, 1, 1);
  lv_obj_set_style_border_width(kc, 0, 0);
  lv_obj_set_style_bg_opa(kc, LV_OPA_TRANSP, 0);
  lv_obj_add_event_cb(kc, np_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_remove_all_objs(lv_group_get_default());
  lv_group_add_obj(lv_group_get_default(), kc);
  lv_group_focus_obj(kc);

  lv_scr_load(g_scr);
  if (old) lv_obj_del_async(old);

  kill_np_timer();
  g_np_timer = lv_timer_create(np_poll_cb, 250, nullptr);
}

}  // namespace

void music_init() {
  g_mux = xSemaphoreCreateMutex();
  g_q = xQueueCreate(8, sizeof(Cmd));
  xTaskCreatePinnedToCore(audio_task, "music", 10240, nullptr, 5, &g_task, 0);
  Serial.println("[MUS] audio task started (core 0)");
}

void music_open() {
  launcher_set_leave_hook(teardown);
  g_scr = nullptr;
  build_browser();
}
