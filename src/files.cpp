/**
 * files.cpp — FileWiz file manager. See files.h.
 *
 * Screens: root (volumes + recents) -> browser (one directory) -> editor or
 * rename prompt. All hand-built rows with inverted focus (repo idiom — lv_list
 * is invisible on the 1-bit panel).
 *
 * Recents: /recents.txt on LittleFS, "fs|/path" or "sd|/path" per line, most
 * recent first, max 10. Digits 0-9 reopen them from the root or browser.
 */
#include "files.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <lvgl.h>
#include <algorithm>
#include <vector>

#include "launcher.h"
#include "st7305.h"
#include "storage.h"

// USB-MSC SD transfer needs the USB-OTG (TinyUSB) build (ARDUINO_USB_MODE=0).
#if !ARDUINO_USB_MODE
#define FILES_USB_MSC 1
#include <USB.h>
#include <USBMSC.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#endif

namespace {

constexpr const char* kRecentsPath = "/recents.txt";
constexpr int kMaxRecents = 10;
constexpr size_t kMaxEditBytes = 32 * 1024;  // textarea sanity cap

// ---------------------------------------------------------------------------
// Volumes + paths
// ---------------------------------------------------------------------------
fs::FS& vol_fs(bool sd) { return sd ? (fs::FS&)SD_MMC : (fs::FS&)LittleFS; }
const char* vol_tag(bool sd) { return sd ? "sd" : "fs"; }

String path_join(const String& dir, const String& name) {
  return dir.endsWith("/") ? dir + name : dir + "/" + name;
}

String path_parent(const String& p) {
  const int slash = p.lastIndexOf('/');
  if (slash <= 0) return "/";
  return p.substring(0, slash);
}

String path_base(const String& p) {
  const int slash = p.lastIndexOf('/');
  return slash < 0 ? p : p.substring(slash + 1);
}

bool is_text(const String& name) {
  String l = name;
  l.toLowerCase();
  return l.endsWith(".txt") || l.endsWith(".md") || l.endsWith(".log") ||
         l.endsWith(".csv") || l.endsWith(".json");
}

String size_label(size_t n) {
  char b[16];
  if (n < 1024) snprintf(b, sizeof b, "%uB", (unsigned)n);
  else snprintf(b, sizeof b, "%uK", (unsigned)((n + 512) / 1024));
  return String(b);
}

struct Entry {
  String name;
  bool dir;
  size_t size;
};

std::vector<Entry> list_dir(bool sd, const String& path) {
  std::vector<Entry> v;
  File d = vol_fs(sd).open(path);
  if (!d || !d.isDirectory()) return v;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String nm = path_base(f.name());
    if (nm.startsWith(".")) continue;  // macOS ._ sidecars etc.
    v.push_back({nm, f.isDirectory(), (size_t)f.size()});
  }
  std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) {
    if (a.dir != b.dir) return a.dir;
    return a.name.compareTo(b.name) < 0;
  });
  return v;
}

// ---------------------------------------------------------------------------
// Recents
// ---------------------------------------------------------------------------
struct Recent { bool sd; String path; };

std::vector<Recent> load_recents() {
  std::vector<Recent> v;
  File f = LittleFS.open(kRecentsPath, "r");
  if (!f) return v;
  while (f.available() && (int)v.size() < kMaxRecents) {
    String line = f.readStringUntil('\n');
    line.trim();
    const int bar = line.indexOf('|');
    if (bar < 0) continue;
    v.push_back({line.substring(0, bar) == "sd", line.substring(bar + 1)});
  }
  f.close();
  return v;
}

void push_recent(bool sd, const String& path) {
  auto v = load_recents();
  v.erase(std::remove_if(v.begin(), v.end(),
                         [&](const Recent& r) {
                           return r.sd == sd && r.path == path;
                         }),
          v.end());
  v.insert(v.begin(), {sd, path});
  if ((int)v.size() > kMaxRecents) v.resize(kMaxRecents);
  File f = LittleFS.open(kRecentsPath, "w");
  if (!f) return;
  for (const Recent& r : v)
    f.printf("%s|%s\n", vol_tag(r.sd), r.path.c_str());
  f.close();
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
lv_obj_t*   g_root_scr = nullptr;   // volumes + recents
lv_obj_t*   g_brow_scr = nullptr;   // directory browser
lv_obj_t*   g_edit_scr = nullptr;   // text editor
lv_obj_t*   g_ren_scr  = nullptr;   // rename prompt
lv_obj_t*   g_edit_ta  = nullptr;
lv_obj_t*   g_ren_ta   = nullptr;
bool        g_sd       = false;     // current volume
String      g_path     = "/";       // current browser dir
String      g_edit_path;            // file open in the editor
String      g_ren_path;             // file being renamed
String      g_status;               // one-shot message
std::vector<Entry> g_entries;       // rows shown in the browser

void build_root();
void build_browser();
void open_path(bool sd, const String& path);

// --- shared row helpers (repo idiom) ---------------------------------------
void row_focus_cb(lv_event_t* e) {
  lv_obj_t* row = lv_event_get_target(e);
  const bool f = lv_event_get_code(e) == LV_EVENT_FOCUSED;
  lv_obj_set_style_bg_color(row, f ? lv_color_black() : lv_color_white(), 0);
  lv_obj_set_style_bg_opa(row, f ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(row); i++)
    lv_obj_set_style_text_color(lv_obj_get_child(row, i),
                                f ? lv_color_white() : lv_color_black(), 0);
}

lv_obj_t* make_row(lv_obj_t* parent, const String& left, const String& right,
                   int idx, lv_event_cb_t click_cb, lv_event_cb_t key_cb) {
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
  lv_obj_add_event_cb(row, key_cb, LV_EVENT_KEY, (void*)(intptr_t)idx);
  lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
  lv_group_add_obj(lv_group_get_default(), row);
  return row;
}

bool nav_keys(uint32_t k) {  // true if the key was arrow navigation
  lv_group_t* g = lv_group_get_default();
  if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT || k == LV_KEY_NEXT) {
    lv_group_focus_next(g);
    return true;
  }
  if (k == LV_KEY_UP || k == LV_KEY_LEFT || k == LV_KEY_PREV) {
    lv_group_focus_prev(g);
    return true;
  }
  return false;
}

// Digits 0-9 reopen a recent file (works on the root and browser screens).
bool recent_digit(uint32_t k) {
  if (k < '0' || k > '9') return false;
  auto rec = load_recents();
  const int i = (int)(k - '0');
  if (i < (int)rec.size()) open_path(rec[i].sd, rec[i].path);
  else { g_status = "no recent #" + String(i); build_root(); }
  return true;
}

// --- editor ----------------------------------------------------------------
void editor_save() {
  if (!g_edit_ta || g_edit_path.isEmpty()) return;
  File f = vol_fs(g_sd).open(g_edit_path, "w");
  if (!f) { Serial.printf("[fil] write failed %s\n", g_edit_path.c_str()); return; }
  f.print(lv_textarea_get_text(g_edit_ta));
  f.close();
}

void editor_close() {
  editor_save();
  g_edit_ta = nullptr;
  g_edit_path = "";
  lv_obj_t* es = g_edit_scr;
  g_edit_scr = nullptr;
  build_browser();
  if (es) lv_obj_del_async(es);
}

void editor_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) editor_close();
}

void open_editor(const String& path) {
  File f = vol_fs(g_sd).open(path, "r");
  if (!f) { g_status = "open failed"; build_browser(); return; }
  if ((size_t)f.size() > kMaxEditBytes) {
    f.close();
    g_status = "too big to edit (>32K)";
    build_browser();
    return;
  }
  String body = f.readString();
  f.close();

  g_edit_path = path;
  push_recent(g_sd, path);

  g_edit_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_edit_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(g_edit_scr, 0, 0);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  // Dark filename header (Notes idiom).
  lv_obj_t* hdr = lv_obj_create(g_edit_scr);
  lv_obj_set_size(hdr, ST7305_W, 32);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_radius(hdr, 0, 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_pad_hor(hdr, 8, 0);
  lv_obj_set_style_bg_color(hdr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* nm = lv_label_create(hdr);
  lv_label_set_text(nm, path_base(path).c_str());
  lv_obj_set_style_text_color(nm, lv_color_white(), 0);
  lv_obj_set_style_text_font(nm, &pixel_operator_16, 0);
  lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
  lv_obj_set_width(nm, ST7305_W - 16);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* ta = lv_textarea_create(g_edit_scr);
  lv_obj_set_size(ta, ST7305_W - 12, ST7305_H - 32 - 8);
  lv_obj_set_pos(ta, 6, 36);
  lv_obj_set_style_radius(ta, 2, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, lv_color_black(), 0);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);  // steady cursor
  lv_textarea_set_text(ta, body.c_str());
  lv_textarea_set_cursor_pos(ta, 0);
  lv_obj_add_event_cb(ta, editor_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(g, ta);
  g_edit_ta = ta;

  lv_scr_load(g_edit_scr);
  lv_group_focus_obj(ta);
}

// Open any recents/browser target: descend dirs, edit text files.
void open_path(bool sd, const String& path) {
  if (sd && !storage_sd_mount()) {
    g_status = "SD not available";
    build_root();
    return;
  }
  g_sd = sd;
  File f = vol_fs(sd).open(path, "r");
  if (!f) { g_status = "missing: " + path_base(path); build_root(); return; }
  const bool dir = f.isDirectory();
  f.close();
  if (dir) {
    g_path = path;
    build_browser();
  } else if (is_text(path)) {
    g_path = path_parent(path);
    open_editor(path);
  } else {
    g_path = path_parent(path);
    g_status = "not a text file";
    build_browser();
  }
}

// --- rename ----------------------------------------------------------------
// READY fires on Enter *press* (mono_ui_lessons): apply on a one-shot timer so
// the release lands on the still-loaded rename screen, not the rebuilt browser.
void rename_apply_cb(lv_timer_t* t) {
  lv_timer_del(t);
  if (!g_ren_ta) return;
  String nn = lv_textarea_get_text(g_ren_ta);
  nn.trim();
  nn.replace("/", "-");
  if (!nn.isEmpty() && nn != path_base(g_ren_path)) {
    const String np = path_join(path_parent(g_ren_path), nn);
    if (vol_fs(g_sd).rename(g_ren_path, np))
      g_status = "renamed";
    else
      g_status = "rename failed";
  }
  g_ren_ta = nullptr;
  g_ren_path = "";
  lv_obj_t* rs = g_ren_scr;
  g_ren_scr = nullptr;
  build_browser();
  if (rs) lv_obj_del_async(rs);
}

void rename_ready_cb(lv_event_t*) {
  lv_timer_create(rename_apply_cb, 60, nullptr);
}

void rename_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) != LV_KEY_ESC) return;
  g_ren_ta = nullptr;
  g_ren_path = "";
  lv_obj_t* rs = g_ren_scr;
  g_ren_scr = nullptr;
  build_browser();
  if (rs) lv_obj_del_async(rs);
}

void open_rename(const String& path) {
  g_ren_path = path;
  g_ren_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_ren_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_ren_scr);
  lv_obj_set_style_text_font(title, &pixel_operator_bold_16, 0);
  lv_label_set_text(title, "Rename");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* old = lv_label_create(g_ren_scr);
  lv_label_set_text_fmt(old, "%s ->", path_base(path).c_str());
  lv_obj_align(old, LV_ALIGN_TOP_LEFT, 12, 36);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* ta = lv_textarea_create(g_ren_scr);
  g_ren_ta = ta;
  lv_obj_set_size(ta, ST7305_W - 24, 30);
  lv_obj_set_pos(ta, 12, 58);
  lv_textarea_set_one_line(ta, true);
  lv_obj_set_style_radius(ta, 2, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, lv_color_black(), 0);
  lv_obj_set_style_pad_all(ta, 3, 0);
  lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR);
  lv_textarea_set_text(ta, path_base(path).c_str());
  lv_obj_add_event_cb(ta, rename_ready_cb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(ta, rename_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(g, ta);

  lv_obj_t* hint = lv_label_create(g_ren_scr);
  lv_label_set_text(hint, "Enter = rename    Esc = cancel");
  lv_obj_set_pos(hint, 12, 96);

  lv_scr_load(g_ren_scr);
  lv_group_focus_obj(ta);
}

// --- browser ---------------------------------------------------------------
void brow_click_cb(lv_event_t* e) {
  const int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0) {  // ".." row
    if (g_path == "/") build_root();
    else { g_path = path_parent(g_path); build_browser(); }
    return;
  }
  if (idx >= (int)g_entries.size()) return;
  const Entry& en = g_entries[idx];
  open_path(g_sd, path_join(g_path, en.name));
}

void brow_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  if (nav_keys(k) || recent_digit(k)) return;
  const int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (k == LV_KEY_ESC) {
    if (g_path == "/") build_root();
    else { g_path = path_parent(g_path); build_browser(); }
  } else if ((k == 'r' || k == 'R') && idx >= 0 &&
             idx < (int)g_entries.size() && !g_entries[idx].dir) {
    open_rename(path_join(g_path, g_entries[idx].name));
  } else if ((k == LV_KEY_DEL || k == LV_KEY_BACKSPACE) && idx >= 0 &&
             idx < (int)g_entries.size()) {
    const Entry& en = g_entries[idx];
    const String p = path_join(g_path, en.name);
    const bool ok = en.dir ? vol_fs(g_sd).rmdir(p) : vol_fs(g_sd).remove(p);
    g_status = ok ? "deleted" : (en.dir ? "dir not empty?" : "delete failed");
    build_browser();
  }
}

void build_browser() {
  g_entries = list_dir(g_sd, g_path);

  lv_obj_t* old = g_brow_scr;
  g_brow_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_brow_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_brow_scr);
  lv_obj_set_style_text_font(title, &pixel_operator_bold_16, 0);
  lv_label_set_text_fmt(title, "%s %s", g_sd ? "SD" : "Flash", g_path.c_str());
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, ST7305_W - 24);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* hint = lv_label_create(g_brow_scr);
  lv_label_set_text(hint, g_status.isEmpty()
                              ? "R=rename  Del=delete  0-9=recent"
                              : g_status.c_str());
  g_status = "";
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 12, 30);

  lv_obj_t* cont = lv_obj_create(g_brow_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 48);
  lv_obj_set_pos(cont, 0, 48);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(cont, 0, 0);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* up = make_row(cont, LV_SYMBOL_LEFT "  ..", "", -1, brow_click_cb,
                          brow_key_cb);

  int shown = 0;
  for (size_t i = 0; i < g_entries.size() && shown < 40; i++, shown++) {
    const Entry& en = g_entries[i];
    make_row(cont,
             String(en.dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE) + "  " +
                 en.name,
             en.dir ? "" : size_label(en.size), (int)i, brow_click_cb,
             brow_key_cb);
  }
  if (g_entries.empty()) {
    lv_obj_t* empty = lv_label_create(cont);
    lv_label_set_text(empty, "\n   (empty)");
    lv_obj_set_style_text_color(empty, lv_color_black(), 0);
  }

  lv_scr_load(g_brow_scr);
  lv_group_focus_obj(up);
  if (old) lv_obj_del_async(old);
}

// --- USB-MSC transfer mode -------------------------------------------------
// The SD card is unmounted locally and handed raw to TinyUSB MSC so a PC sees
// it as a plain USB drive. The global USBMSC's constructor registers the MSC
// interface — that must happen before USB.begin(), which the core calls
// before setup() (CDC_ON_BOOT); capacity + callbacks are filled in on entry.
#if FILES_USB_MSC
USBMSC        g_msc;
sdmmc_card_t  g_card_mem;
sdmmc_card_t* g_card = nullptr;
bool          g_usb_active = false;
volatile bool g_msc_closing = false;
volatile bool g_host_ejected = false;
lv_obj_t*     g_usb_scr = nullptr;
lv_obj_t*     g_usb_lbl = nullptr;
lv_timer_t*   g_usb_tim = nullptr;

extern "C" bool tud_mounted(void);
extern "C" bool tud_disconnect(void);
extern "C" bool tud_connect(void);

// A failed callback must never return instantly: the usbd task (prio 24) and
// a retrying host turn that into a hot loop on core 0 that starves IDLE0 and
// trips the 5 s task WDT (coredump-verified). One tick of delay paces it.
int32_t msc_fail() {
  vTaskDelay(1);
  return -1;
}

int32_t msc_read(uint32_t lba, uint32_t offset, void* buf, uint32_t bufsize) {
  if (g_msc_closing || !g_card || offset) return msc_fail();
  const uint32_t n = bufsize / g_card->csd.sector_size;
  if (!n || sdmmc_read_sectors(g_card, buf, lba, n) != ESP_OK) return msc_fail();
  return (int32_t)(n * g_card->csd.sector_size);
}

int32_t msc_write(uint32_t lba, uint32_t offset, uint8_t* buf,
                  uint32_t bufsize) {
  if (g_msc_closing || !g_card || offset) return msc_fail();
  const uint32_t n = bufsize / g_card->csd.sector_size;
  if (!n || sdmmc_write_sectors(g_card, buf, lba, n) != ESP_OK) return msc_fail();
  return (int32_t)(n * g_card->csd.sector_size);
}

bool msc_start_stop(uint8_t, bool start, bool load_eject) {
  if (load_eject && !start) g_host_ejected = true;  // PC clicked "eject"
  return true;
}

bool raw_sd_mount() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = GPIO_NUM_38;
  slot.cmd = GPIO_NUM_21;
  slot.d0 = GPIO_NUM_39;
  if (sdmmc_host_init() != ESP_OK) return false;
  if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot) != ESP_OK ||
      sdmmc_card_init(&host, &g_card_mem) != ESP_OK) {
    sdmmc_host_deinit();
    return false;
  }
  g_card = &g_card_mem;
  return true;
}

void usb_exit() {
  if (!g_usb_active) return;
  // Detach at the BUS level before touching MSC state. Once media_present is
  // false (end() also sets it), the core's read10 wrapper answers a retrying
  // host with 0 = "poll me again" WITHOUT calling our callback; TinyUSB then
  // re-queues that retry forever, the usbd task (prio 24, core 0) never
  // blocks, IDLE0 starves and the 5 s task WDT reboots the box (two
  // coredumps: USBMSC.cpp:114 mid-command, usbd.c:705 event pump). A host
  // that saw the device unplug sends nothing, so the storm can't start.
  g_msc_closing = true;   // fail any in-flight I/O, paced by msc_fail()
  tud_disconnect();       // host sees a physical unplug (CDC drops too)
  delay(300);             // let the bus settle and in-flight commands die
  g_msc.mediaPresent(false);
  g_msc.end();
  g_card = nullptr;
  sdmmc_host_deinit();
  g_usb_active = false;
  g_msc_closing = false;
  storage_sd_mount();  // give the VFS back to the rest of the firmware
  tud_connect();       // re-enumerate: CDC returns, MSC reports no medium
  Serial.println("[fil] USB-MSC ejected, SD remounted");
}

void usb_close_screen() {
  if (g_usb_tim) { lv_timer_del(g_usb_tim); g_usb_tim = nullptr; }
  usb_exit();
  g_usb_lbl = nullptr;
  lv_obj_t* us = g_usb_scr;
  g_usb_scr = nullptr;
  build_root();
  if (us) lv_obj_del_async(us);
}

void usb_key_cb(lv_event_t* e) {
  if (lv_event_get_key(e) == LV_KEY_ESC) usb_close_screen();
}

void usb_tick_cb(lv_timer_t*) {
  if (!g_usb_lbl) return;
  lv_label_set_text(g_usb_lbl, g_host_ejected ? "ejected by PC"
                               : tud_mounted() ? "connected to PC"
                                               : "waiting for PC...");
}

void build_usb() {
  g_usb_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_usb_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_usb_scr, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* icon = lv_label_create(g_usb_scr);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
  lv_label_set_text(icon, LV_SYMBOL_USB "  USB drive");
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);

  g_usb_lbl = lv_label_create(g_usb_scr);
  lv_label_set_text(g_usb_lbl, "waiting for PC...");
  lv_obj_align(g_usb_lbl, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* hint = lv_label_create(g_usb_scr);
  lv_label_set_text(hint, "SD is exposed to the PC\nEsc = eject + return");
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 48);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);
  lv_obj_add_event_cb(g_usb_scr, usb_key_cb, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(g, g_usb_scr);

  lv_scr_load(g_usb_scr);
  lv_group_focus_obj(g_usb_scr);
  g_usb_tim = lv_timer_create(usb_tick_cb, 500, nullptr);
}

void usb_enter() {
  if (!g_usb_active) {
    if (!storage_sd_mount()) {  // prove a card is present before handing off
      g_status = "SD not available";
      build_root();
      return;
    }
    storage_sd_unmount();
    if (!raw_sd_mount()) {
      storage_sd_mount();
      g_status = "SD raw init failed";
      build_root();
      return;
    }
    g_msc.vendorID("ghalib");
    g_msc.productID("SD Card");
    g_msc.productRevision("1.0");
    g_msc.onRead(msc_read);
    g_msc.onWrite(msc_write);
    g_msc.onStartStop(msc_start_stop);
    g_msc.isWritable(true);
    g_msc.begin(g_card->csd.capacity, g_card->csd.sector_size);
    g_msc.mediaPresent(true);
    g_host_ejected = false;
    g_usb_active = true;
    Serial.printf("[fil] USB-MSC up: %u sectors x %u\n",
                  (unsigned)g_card->csd.capacity,
                  (unsigned)g_card->csd.sector_size);
  }
  build_usb();
}
#endif  // FILES_USB_MSC

// --- root (volumes + recents) ----------------------------------------------
void root_click_cb(lv_event_t* e) {
  const int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx == -1) {  // Flash
    g_sd = false;
    g_path = "/";
    build_browser();
  } else if (idx == -2) {  // SD
    open_path(true, "/");
  } else if (idx == -3) {  // USB transfer
#if FILES_USB_MSC
    usb_enter();
#else
    g_status = "needs the USB-OTG build";
    build_root();
#endif
  } else {  // recent row
    auto rec = load_recents();
    if (idx >= 0 && idx < (int)rec.size()) open_path(rec[idx].sd, rec[idx].path);
  }
}

void root_key_cb(lv_event_t* e) {
  const uint32_t k = lv_event_get_key(e);
  if (nav_keys(k) || recent_digit(k)) return;
  if (k == LV_KEY_ESC) launcher_go_home();
}

void build_root() {
  lv_obj_t* old = g_root_scr;
  g_root_scr = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_root_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(g_root_scr);
  lv_obj_set_style_text_font(title, &pixel_operator_bold_16, 0);
  lv_label_set_text(title, "Files");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

  lv_obj_t* hint = lv_label_create(g_root_scr);
  lv_label_set_text(hint, g_status.isEmpty() ? "0-9 = recent file"
                                             : g_status.c_str());
  g_status = "";
  lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -8, 10);

  lv_obj_t* cont = lv_obj_create(g_root_scr);
  lv_obj_set_size(cont, ST7305_W, ST7305_H - 36);
  lv_obj_set_pos(cont, 0, 36);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(cont, 0, 0);

  lv_group_t* g = lv_group_get_default();
  lv_group_remove_all_objs(g);

  lv_obj_t* first = make_row(cont, LV_SYMBOL_DRIVE "  Flash (LittleFS)", "",
                             -1, root_click_cb, root_key_cb);
  make_row(cont, LV_SYMBOL_SD_CARD "  SD card", "", -2, root_click_cb,
           root_key_cb);
  make_row(cont, LV_SYMBOL_USB "  USB transfer -> PC", "", -3, root_click_cb,
           root_key_cb);

  auto rec = load_recents();
  if (!rec.empty()) {
    lv_obj_t* cap = lv_label_create(cont);
    lv_label_set_text(cap, "  Recent");
    lv_obj_set_style_text_color(cap, lv_color_black(), 0);
    lv_obj_set_style_pad_top(cap, 8, 0);
    for (int i = 0; i < (int)rec.size(); i++)
      make_row(cont,
               String(i) + "  " + path_base(rec[i].path),
               rec[i].sd ? "SD" : "Flash", i, root_click_cb, root_key_cb);
  }

  lv_scr_load(g_root_scr);
  lv_group_focus_obj(first);
  if (old) lv_obj_del_async(old);
}

void app_teardown() {
#if FILES_USB_MSC
  if (g_usb_tim) { lv_timer_del(g_usb_tim); g_usb_tim = nullptr; }
  usb_exit();  // Home while exposed: eject cleanly + remount
  g_usb_lbl = nullptr;
  if (g_usb_scr) { lv_obj_del_async(g_usb_scr); g_usb_scr = nullptr; }
#endif
  if (g_edit_ta) { editor_save(); g_edit_ta = nullptr; }
  if (g_edit_scr) { lv_obj_del_async(g_edit_scr); g_edit_scr = nullptr; }
  if (g_ren_scr) { lv_obj_del_async(g_ren_scr); g_ren_scr = nullptr; }
  if (g_brow_scr) { lv_obj_del_async(g_brow_scr); g_brow_scr = nullptr; }
  if (g_root_scr) { lv_obj_del_async(g_root_scr); g_root_scr = nullptr; }
  g_ren_ta = nullptr;
  g_edit_path = g_ren_path = "";
}

}  // namespace

void files_open() {
  launcher_set_leave_hook(app_teardown);
  g_root_scr = g_brow_scr = g_edit_scr = g_ren_scr = nullptr;
  build_root();
}

bool files_usb_active() {
#if FILES_USB_MSC
  return g_usb_active;
#else
  return false;
#endif
}
