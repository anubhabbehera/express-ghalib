/**
 * music.cpp — Music player (M4 + M6 UX). Browse /music, stream WAV (16-bit PCM)
 * and MP3 (Helix decoder) to the ES8311, with a now-playing screen: play/pause,
 * prev/next, volume, progress bar with elapsed/total time, shuffle + repeat,
 * and auto-advance.
 *
 * Threading: playback runs in a dedicated FreeRTOS task pinned to core 0, so it
 * keeps I2S fed even while the UI loop on core 1 does long blocking work (the
 * reflective panel's full-screen SPI flush, or the reminder poll's LittleFS
 * read). The UI sends commands via a queue and polls player state for display;
 * it never touches the audio path directly. This is what keeps MP3 glitch-free.
 *
 * Elapsed time is tracked by counting output stereo frames fed to I2S
 * (feed()), divided by the output rate — accurate for both formats and it
 * freezes naturally on pause. Total time is exact for WAV (from the header) and
 * estimated for MP3 (file size / Helix-reported bitrate).
 *
 * SDMMC is a separate peripheral from the display SPI and audio I2S — no bus
 * contention. The FAT VFS serializes the rare concurrent SD access.
 */
#include "music.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_random.h>
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
#include "storage.h"

namespace {
// SD mount is shared with journal/notes backups — see storage_sd_mount().
bool ensure_mounted() { return storage_sd_mount(); }

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

// Scan /music into `out`. Deliberately touches neither g_tracks nor g_mux: the
// SD directory walk + sort + unique is slow, and the audio task blocks on g_mux
// on every track boundary (pick_next/task_open), so holding the lock across this
// would stall playback (DMA underrun) whenever a track advances during a browse.
// The caller publishes the result with an O(1) swap under a brief lock instead.
void scan_tracks(std::vector<String>& out) {
  out.clear();
  File dir = SD_MMC.open("/music");
  if (!dir || !dir.isDirectory()) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String nm = f.name();
    const int slash = nm.lastIndexOf('/');
    if (slash >= 0) nm = nm.substring(slash + 1);
    // Skip hidden / macOS AppleDouble sidecars ("._song.wav") — they'd double up.
    if (nm.startsWith(".")) continue;
    if (is_audio(nm)) out.push_back(nm);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
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
      // Sanity-check the header before it reaches the I2S config: files on the
      // SD card are arbitrary input, and a bogus rate/channel count would
      // either fail the I2S re-init (leaving playback at the previous rate) or
      // stream frames the mono/stereo path can't interpret.
      if (sr < 8000 || sr > 48000 || (ch != 1 && ch != 2)) {
        Serial.printf("[MUS] WAV rate/channels out of range (%lu Hz, %u ch)\n",
                      (unsigned long)sr, ch);
        return false;
      }
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

// ---------------------------------------------------------------------------
// Player state — the audio task owns playback; UI reads these for display.
// Plain scalars with benign display races; only g_tracks needs the mutex.
// ---------------------------------------------------------------------------
enum Fmt { FMT_WAV, FMT_MP3 };
enum { C_PLAY, C_STOP, C_NEXT, C_PREV, C_PAUSE, C_VOLUP, C_VOLDN,
       C_SHUFFLE, C_REPEAT };
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

// Playback clock + modes (written by the audio task, read by the UI).
volatile uint32_t g_out_frames = 0;   // stereo frames fed to I2S this track
volatile uint32_t g_out_rate   = 0;   // output sample rate (Hz)
volatile uint32_t g_total_ms   = 0;   // track duration; 0 = unknown
volatile bool     g_shuffle    = false;
volatile int      g_repeat     = 0;   // 0 = off, 1 = all, 2 = one

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
// Every path to I2S goes through here so the playback clock stays exact.
// All output is stereo 16-bit, so one frame = 4 bytes.
void feed(const uint8_t* p, size_t bytes) {
  audio_write(p, bytes);
  g_out_frames += bytes / 4;
}

// --- MP3: Helix streams decoded PCM to this callback -> I2S ------------------
void mp3_data_cb(MP3FrameInfo& info, short* pcm, size_t len, void*) {
  if (info.samprate > 0) { audio_prepare((uint32_t)info.samprate); g_out_rate = info.samprate; }
  if (info.bitrate > 0 && g_size)   // estimate duration from CBR bitrate
    g_total_ms = (uint32_t)((uint64_t)g_size * 8000 / (uint32_t)info.bitrate);
  if (info.nChans >= 2) {
    feed((const uint8_t*)pcm, len * sizeof(short));
  } else {
    static int16_t ob[2 * 1152];
    size_t s = len < 1152 ? len : 1152;
    for (size_t i = 0; i < s; i++) { ob[2 * i] = pcm[i]; ob[2 * i + 1] = pcm[i]; }
    feed((const uint8_t*)ob, s * 2 * sizeof(int16_t));
  }
}
libhelix::MP3DecoderHelix g_mp3(mp3_data_cb);

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
  g_out_frames = 0;
  g_total_ms = 0;
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

  g_out_frames = 0;
  g_out_rate = 0;
  g_total_ms = 0;
  if (is_mp3(name)) {
    g_tfmt = FMT_MP3;
    g_mp3.begin();                 // g_out_rate/g_total_ms set from first frame
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
    g_out_rate = w.rate;
    const uint32_t frame = (uint32_t)w.channels * 2;   // source bytes/frame
    if (w.rate && frame)
      g_total_ms = (uint32_t)((uint64_t)w.data_size * 1000 / ((uint64_t)w.rate * frame));
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

// Pick the next track honouring shuffle / repeat; -1 = stop. auto_end=true when
// a track reached its end (repeat-one replays); manual Next ignores repeat-one.
int pick_next(bool auto_end) {
  int count;
  xSemaphoreTake(g_mux, portMAX_DELAY);
  count = (int)g_tracks.size();
  xSemaphoreGive(g_mux);
  if (count <= 0) return -1;
  if (auto_end && g_repeat == 2) return g_index;           // repeat-one
  if (g_shuffle) {
    if (count == 1) return 0;
    int r = (int)(esp_random() % count);
    if (r == g_index) r = (r + 1) % count;
    return r;
  }
  int n = g_index + 1;
  if (n >= count) return g_repeat == 1 ? 0 : -1;           // wrap only if repeat-all
  return n;
}

void task_advance(bool auto_end) {
  const int n = pick_next(auto_end);
  if (n < 0) task_end();
  else task_open(n);
}

void task_prev() {
  if (g_shuffle) { task_advance(false); return; }          // shuffle: just re-pick
  int n = g_index - 1;
  if (n < 0) n = g_repeat == 1 ? g_count - 1 : 0;          // wrap or stay on first
  task_open(n);
}

void handle_cmd(const Cmd& c) {
  switch (c.type) {
    case C_PLAY:  task_open(c.arg); break;
    case C_STOP:  task_end(); break;
    case C_NEXT:  task_advance(false); break;
    case C_PREV:  task_prev(); break;
    case C_PAUSE: g_paused = !g_paused; break;
    case C_VOLUP: g_volume = g_volume + 10 > 100 ? 100 : g_volume + 10;
                  audio_set_volume(g_volume); break;
    case C_VOLDN: g_volume = g_volume - 10 < 0 ? 0 : g_volume - 10;
                  audio_set_volume(g_volume); break;
    case C_SHUFFLE: g_shuffle = !g_shuffle; break;
    case C_REPEAT:  g_repeat = (g_repeat + 1) % 3; break;
  }
}

void stream_chunk() {
  if (g_tfmt == FMT_WAV) {
    if (g_trem == 0) { task_advance(true); return; }        // auto-advance
    static uint8_t rbuf[2048];
    const size_t want = g_trem < sizeof(rbuf) ? g_trem : sizeof(rbuf);
    const int n = g_tf.read(rbuf, want);
    if (n <= 0) { task_advance(true); return; }
    g_trem -= n;
    if (g_tch == 1) {                                       // mono -> stereo
      static int16_t obuf[2048];
      const int samples = n / 2;
      const int16_t* in = (const int16_t*)rbuf;
      for (int i = 0; i < samples; i++) { obuf[2 * i] = in[i]; obuf[2 * i + 1] = in[i]; }
      feed((uint8_t*)obuf, samples * 4);
    } else {
      feed(rbuf, n);
    }
  } else {                                                  // FMT_MP3
    static uint8_t mbuf[1024];
    const int n = g_tf.read(mbuf, sizeof(mbuf));
    if (n <= 0) { task_advance(true); return; }             // EOF -> next
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
lv_obj_t*  g_state_lbl  = nullptr;
lv_obj_t*  g_name_lbl   = nullptr;
lv_obj_t*  g_elapsed_lbl = nullptr;
lv_obj_t*  g_total_lbl  = nullptr;
lv_obj_t*  g_vol_bar    = nullptr;
lv_obj_t*  g_vol_lbl    = nullptr;
lv_obj_t*  g_mode_lbl   = nullptr;
lv_timer_t* g_np_timer  = nullptr;

void build_browser();
void build_now_playing();

// Change-detection caches for np_poll_cb — reset whenever the now-playing
// screen (and its labels) is rebuilt, so the first poll after a rebuild always
// repaints. These are only touched on the UI task (core 1).
char np_last_name[64]  = "";
char np_last_state[40] = "";
char np_last_elapsed[8] = "";
char np_last_total[8]  = "";
char np_last_mode[48]  = "";
int  np_last_vol       = -1;

void np_reset_cache() {
  np_last_name[0] = np_last_state[0] = np_last_elapsed[0] = '\0';
  np_last_total[0] = np_last_mode[0] = '\0';
  np_last_vol = -1;
}

void kill_np_timer() {
  if (g_np_timer) { lv_timer_del(g_np_timer); g_np_timer = nullptr; }
  np_reset_cache();
}

void teardown() {
  kill_np_timer();
  if (g_playing) send_cmd(C_STOP);           // Home/leave while playing -> stop
  if (g_scr) { lv_obj_del_async(g_scr); g_scr = nullptr; }
  g_bar = g_state_lbl = g_name_lbl = g_elapsed_lbl = nullptr;
  g_total_lbl = g_vol_bar = g_vol_lbl = g_mode_lbl = nullptr;
}

void fmt_mmss(uint32_t ms, char* out, size_t n) {
  if (!ms) { snprintf(out, n, "--:--"); return; }
  const uint32_t s = ms / 1000;
  snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// mm:ss elapsed from the frame counter (exact; frozen while paused).
uint32_t elapsed_ms() {
  return g_out_rate ? (uint32_t)((uint64_t)g_out_frames * 1000 / g_out_rate) : 0;
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

// Return to the browser one tick later so the Esc *release* doesn't land as a
// click on the browser's focused row (Enter/Esc-release leak — see settings.cpp).
void back_to_browser_deferred(lv_timer_t* t) {
  lv_timer_del(t);
  build_browser();
}

// Now-playing transport: Enter=pause, L/R=prev/next, U/D=vol, s=shuffle,
// r=repeat, Esc=back to list (keeps playing).
void np_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  if (k == LV_KEY_ESC) { kill_np_timer(); lv_timer_create(back_to_browser_deferred, 40, nullptr); }
  else if (k == LV_KEY_ENTER || k == ' ') send_cmd(C_PAUSE);
  else if (k == LV_KEY_RIGHT || k == LV_KEY_NEXT) send_cmd(C_NEXT);
  else if (k == LV_KEY_LEFT || k == LV_KEY_PREV) send_cmd(C_PREV);
  else if (k == LV_KEY_UP) send_cmd(C_VOLUP);
  else if (k == LV_KEY_DOWN) send_cmd(C_VOLDN);
  else if (k == 's' || k == 'S') send_cmd(C_SHUFFLE);
  else if (k == 'r' || k == 'R') send_cmd(C_REPEAT);
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
  g_bar = g_state_lbl = g_name_lbl = g_elapsed_lbl = nullptr;
  g_total_lbl = g_vol_bar = g_vol_lbl = g_mode_lbl = nullptr;
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_scr);
  lv_obj_set_style_text_font(title, &pixel_operator_bold_16, 0);
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

  lv_obj_t* first = nullptr;      // fallback focus
  lv_obj_t* playing_row = nullptr;
  if (!ensure_mounted()) {
    first = make_row(cont, LV_SYMBOL_WARNING "  No SD card  (Esc = back)", nullptr, nullptr);
  } else {
    std::vector<String> scanned;
    scan_tracks(scanned);                      // slow SD work, no lock held
    xSemaphoreTake(g_mux, portMAX_DELAY);
    g_tracks.swap(scanned);                    // O(1) publish; audio task waits µs
    const int n = (int)g_tracks.size();
    xSemaphoreGive(g_mux);
    g_count = n;
    Serial.printf("[MUS] /music: %d track(s)\n", n);
    if (n == 0) {
      first = make_row(cont, "(no tracks in /music)", nullptr, nullptr);
    } else {
      const int cur = g_playing ? g_index : -1;   // mark the live track with a >
      // g_tracks is UI-owned (the audio task only reads it, never writes), and
      // no other UI code runs concurrently, so reading it here without the lock
      // is safe now that the list is published.
      for (int i = 0; i < n; i++) {
        String label = (i == cur ? String(LV_SYMBOL_PLAY "  ")
                                 : String("     ")) + g_tracks[i];
        lv_obj_t* row = make_row(cont, label.c_str(), (void*)(intptr_t)i, track_click_cb);
        if (!first) first = row;
        if (i == cur) playing_row = row;
      }
    }
  }

  lv_scr_load(g_scr);
  lv_group_focus_obj(playing_row ? playing_row : first);  // land on the live track
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
  // Diff before writing: lv_label_set_text always invalidates (even for
  // identical text) and any invalidation triggers a full-panel flush, so an
  // unconditional set per field would flush the whole screen every tick for
  // values that mostly change once a second or on keypress. We compose into
  // fixed char buffers (no String heap churn) and only write on a real change.
  // The shared audio state read here (g_name/g_index/g_paused/g_volume/...) is
  // owned by the core-0 audio task but already read lock-free here — this only
  // adds local compares, no new cross-core access.
  if (g_name_lbl && strcmp(np_last_name, g_name) != 0) {
    strncpy(np_last_name, g_name, sizeof(np_last_name) - 1);
    np_last_name[sizeof(np_last_name) - 1] = 0;
    lv_label_set_text(g_name_lbl, g_name);
  }
  if (g_state_lbl) {
    char sb[40];
    snprintf(sb, sizeof sb, "%s%d/%d",
             g_paused ? LV_SYMBOL_PAUSE "  Paused   "
                      : LV_SYMBOL_PLAY "  Playing   ",
             (int)g_index + 1, (int)g_count);
    if (strcmp(np_last_state, sb) != 0) {
      strcpy(np_last_state, sb);
      lv_label_set_text(g_state_lbl, sb);
    }
  }

  const uint32_t el = elapsed_ms(), tot = g_total_ms;
  char eb[8], tb[8];
  fmt_mmss(el, eb, sizeof eb);
  fmt_mmss(tot, tb, sizeof tb);
  if (g_elapsed_lbl && strcmp(np_last_elapsed, eb) != 0) {
    strcpy(np_last_elapsed, eb);
    lv_label_set_text(g_elapsed_lbl, eb);
  }
  if (g_total_lbl && strcmp(np_last_total, tb) != 0) {
    strcpy(np_last_total, tb);
    lv_label_set_text(g_total_lbl, tb);
  }
  if (g_bar) {  // lv_bar_set_value self-no-ops when unchanged (no invalidate)
    int pct = tot ? (int)((uint64_t)el * 100 / tot)
                  : (g_size ? (int)((uint64_t)g_pos * 100 / g_size) : 0);
    lv_bar_set_value(g_bar, pct > 100 ? 100 : pct, LV_ANIM_OFF);
  }
  if (g_vol_bar) lv_bar_set_value(g_vol_bar, g_volume, LV_ANIM_OFF);
  if (g_vol_lbl && g_volume != np_last_vol) {
    np_last_vol = g_volume;
    char vb[8];
    snprintf(vb, sizeof vb, "%d%%", (int)g_volume);
    lv_label_set_text(g_vol_lbl, vb);
  }
  if (g_mode_lbl) {
    const char* rep = g_repeat == 0 ? "Off" : g_repeat == 1 ? "All" : "One";
    char mb[48];
    snprintf(mb, sizeof mb, "%s%s%s %s", LV_SYMBOL_SHUFFLE,
             g_shuffle ? " On    " : " Off    ", LV_SYMBOL_LOOP, rep);
    if (strcmp(np_last_mode, mb) != 0) {
      strcpy(np_last_mode, mb);
      lv_label_set_text(g_mode_lbl, mb);
    }
  }
}

void build_now_playing() {
  lv_obj_t* old = g_scr;
  g_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

  g_name_lbl = lv_label_create(g_scr);
  lv_obj_set_style_text_font(g_name_lbl, &pixel_operator_bold_16, 0);
  lv_label_set_long_mode(g_name_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_name_lbl, ST7305_W - 40);
  lv_obj_set_style_text_align(g_name_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(g_name_lbl, g_name);
  lv_obj_align(g_name_lbl, LV_ALIGN_TOP_MID, 0, 12);

  g_state_lbl = lv_label_create(g_scr);
  lv_obj_set_style_text_font(g_state_lbl, &pixel_operator_bold_16, 0);
  lv_label_set_text(g_state_lbl, LV_SYMBOL_PLAY "  Playing");
  lv_obj_align(g_state_lbl, LV_ALIGN_TOP_MID, 0, 74);

  g_bar = lv_bar_create(g_scr);
  lv_obj_set_size(g_bar, ST7305_W - 64, 12);
  lv_obj_align(g_bar, LV_ALIGN_TOP_MID, 0, 110);
  lv_obj_set_style_bg_opa(g_bar, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_bar, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(g_bar, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(g_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_bar, lv_color_black(), LV_PART_INDICATOR);
  lv_obj_set_style_radius(g_bar, 0, LV_PART_INDICATOR);
  lv_bar_set_range(g_bar, 0, 100);
  lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);

  g_elapsed_lbl = lv_label_create(g_scr);
  lv_obj_set_style_text_font(g_elapsed_lbl, &pixel_operator_16, 0);
  lv_label_set_text(g_elapsed_lbl, "0:00");
  lv_obj_align(g_elapsed_lbl, LV_ALIGN_TOP_LEFT, 32, 126);

  g_total_lbl = lv_label_create(g_scr);
  lv_obj_set_style_text_font(g_total_lbl, &pixel_operator_16, 0);
  lv_label_set_text(g_total_lbl, "--:--");
  lv_obj_align(g_total_lbl, LV_ALIGN_TOP_RIGHT, -32, 126);

  // Volume row: speaker icon + bar + percent.
  lv_obj_t* vrow = lv_obj_create(g_scr);
  lv_obj_set_size(vrow, 240, 22);
  lv_obj_align(vrow, LV_ALIGN_TOP_MID, 0, 152);
  lv_obj_set_style_border_width(vrow, 0, 0);
  lv_obj_set_style_bg_opa(vrow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(vrow, 0, 0);
  lv_obj_clear_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(vrow, 8, 0);

  lv_obj_t* vicon = lv_label_create(vrow);
  lv_label_set_text(vicon, LV_SYMBOL_VOLUME_MAX);
  g_vol_bar = lv_bar_create(vrow);
  lv_obj_set_size(g_vol_bar, 140, 10);
  lv_obj_set_style_bg_opa(g_vol_bar, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_vol_bar, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(g_vol_bar, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(g_vol_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_vol_bar, lv_color_black(), LV_PART_INDICATOR);
  lv_obj_set_style_radius(g_vol_bar, 0, LV_PART_INDICATOR);
  lv_bar_set_range(g_vol_bar, 0, 100);
  lv_bar_set_value(g_vol_bar, g_volume, LV_ANIM_OFF);
  g_vol_lbl = lv_label_create(vrow);
  lv_obj_set_style_text_font(g_vol_lbl, &pixel_operator_16, 0);
  lv_label_set_text(g_vol_lbl, "75%");

  // Shuffle / repeat state.
  g_mode_lbl = lv_label_create(g_scr);
  lv_obj_set_style_text_font(g_mode_lbl, &pixel_operator_16, 0);
  lv_label_set_text(g_mode_lbl, LV_SYMBOL_SHUFFLE " Off    " LV_SYMBOL_LOOP " Off");
  lv_obj_align(g_mode_lbl, LV_ALIGN_TOP_MID, 0, 186);

  lv_obj_t* hint = lv_label_create(g_scr);
  lv_obj_set_style_text_font(hint, &pixel_operator_16, 0);
  lv_label_set_text(hint,
                    LV_SYMBOL_LEFT "/" LV_SYMBOL_RIGHT " prev/next   Enter pause   s shuffle   r repeat\n"
                    LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " vol   Esc/KEY back");
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

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
  // 1 s poll: the only sub-second-visible field is the mm:ss elapsed clock,
  // which ticks at 1 Hz. Combined with the diffing in np_poll_cb, idle/paused
  // ticks now cause no invalidation and no flush at all.
  g_np_timer = lv_timer_create(np_poll_cb, 1000, nullptr);
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

bool music_playing() { return g_playing && !g_paused; }
