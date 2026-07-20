/**
 * main.cpp — express-ghalib bring-up skeleton (M0 + M1 stubs)
 *
 * Wires LVGL (1-bit mono) -> ST7305 flush, and sketches the NimBLE HOGP-host
 * scan. Fill the TODOs milestone by milestone (see PLAN.md).
 */
#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include "ble_kbd.h"
#include "buttons.h"
#include "launcher.h"
#include "rtc.h"
#include "settings.h"
#include "shtc3.h"
#include "st7305.h"
#include "storage.h"

// ---------------------------------------------------------------------------
// Display: LVGL 1-bit monochrome -> ST7305 native framebuffer
// ---------------------------------------------------------------------------
// The ST7305 packing is non-linear (2x4 blocks), so we make LVGL's draw buffer
// BE the ST7305 native framebuffer and let set_px_cb do the packing. With
// full_refresh, LVGL redraws the whole frame each cycle, then we push it once.
static uint8_t g_fb[ST7305_BUF_BYTES];  // 15000 B; fits internal RAM
static lv_disp_draw_buf_t g_lv_draw_buf;
static lv_disp_drv_t g_lv_disp_drv;

static void mono_set_px_cb(lv_disp_drv_t*, uint8_t* buf, lv_coord_t /*buf_w*/,
                           lv_coord_t x, lv_coord_t y, lv_color_t color,
                           lv_opa_t /*opa*/) {
  // MONO theme: brightness 0 -> ink (black). `buf` is g_fb (native packed).
  st7305_buf_set(buf, x, y, lv_color_brightness(color) < 128);
}

static void mono_flush_cb(lv_disp_drv_t* drv, const lv_area_t* /*area*/,
                          lv_color_t* /*color_p*/) {
  st7305_flush_full(g_fb);  // full_refresh -> whole native buffer already packed
  lv_disp_flush_ready(drv);
}

static void display_init() {
  st7305_init();  // default Waveshare pins: sck39 mosi38 cs40 dc5 rst41

  lv_init();
  // Buffer size is in PIXELS for LVGL bookkeeping; the bytes live in g_fb.
  lv_disp_draw_buf_init(&g_lv_draw_buf, g_fb, nullptr, ST7305_W * ST7305_H);

  lv_disp_drv_init(&g_lv_disp_drv);
  g_lv_disp_drv.hor_res      = ST7305_W;   // 400 (native landscape)
  g_lv_disp_drv.ver_res      = ST7305_H;   // 300
  g_lv_disp_drv.draw_buf     = &g_lv_draw_buf;
  g_lv_disp_drv.flush_cb     = mono_flush_cb;
  g_lv_disp_drv.set_px_cb    = mono_set_px_cb;
  g_lv_disp_drv.full_refresh = 1;          // reflective panel: redraw whole frame
  // For a portrait 300x400 UI, set g_lv_disp_drv.rotated + sw_rotate here.
  lv_disp_drv_register(&g_lv_disp_drv);
}

// ---------------------------------------------------------------------------
// Input: BLE HID (HOGP) host  — highest-risk item, see M1
// ---------------------------------------------------------------------------
static lv_indev_drv_t g_kbd_indev_drv;
static lv_group_t*    g_ui_group = nullptr;

static void kbd_read_cb(lv_indev_drv_t*, lv_indev_data_t* data) {
  // Report each queued key as PRESSED, then RELEASED on the next poll, so LVGL
  // registers a full keystroke.
  static uint32_t cur = 0;
  static bool held = false;
  if (held) {
    data->key = cur;
    data->state = LV_INDEV_STATE_RELEASED;
    held = false;
    return;
  }
  uint32_t k;
  if (ble_kbd_pop(&k)) {
    cur = k;
    data->key = k;
    data->state = LV_INDEV_STATE_PRESSED;
    held = true;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void input_init() {
  g_ui_group = lv_group_create();
  lv_group_set_default(g_ui_group);

  lv_indev_drv_init(&g_kbd_indev_drv);
  g_kbd_indev_drv.type = LV_INDEV_TYPE_KEYPAD;
  g_kbd_indev_drv.read_cb = kbd_read_cb;
  lv_indev_t* kbd = lv_indev_drv_register(&g_kbd_indev_drv);
  lv_indev_set_group(kbd, g_ui_group);

  // TODO(M1): NimBLE init as a *central*; scan for HID service (0x1812),
  //   connect + bond a BLE keyboard, subscribe to its Report characteristics,
  //   push decoded key events into the queue kbd_read_cb drains.
  //   Persist the bonded MAC in NVS for auto-reconnect.
  //
  // NOTE: I2C for the RTC (PCF85063A) + SHTC3 is on SDA=GPIO13, SCL=GPIO14
  //       (from the board devicetree) — wire that up at M3.
}

// ---------------------------------------------------------------------------
// M0 test pattern — reveals orientation / packing / address-window errors on
// the FIRST flash. Kept for future display debugging (not shown by default).
// ---------------------------------------------------------------------------
[[maybe_unused]] static void build_hello_screen() {
  lv_obj_t* scr = lv_scr_act();

  // Full-screen border box.
  lv_obj_t* box = lv_obj_create(scr);
  lv_obj_set_size(box, ST7305_W, ST7305_H);
  lv_obj_set_pos(box, 0, 0);
  lv_obj_set_style_radius(box, 0, 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(box, 2, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  // Diagonal TL -> BR (must be straight; catches x/y transposition).
  static lv_point_t diag[] = {{0, 0}, {ST7305_W - 1, ST7305_H - 1}};
  lv_obj_t* line = lv_line_create(scr);
  lv_line_set_points(line, diag, 2);
  lv_obj_set_style_line_width(line, 1, 0);

  // Asymmetric corner tags -> orientation is unambiguous.
  struct Corner { const char* txt; lv_align_t align; };
  static const Corner corners[] = {
      {"TL", LV_ALIGN_TOP_LEFT},     {"TR", LV_ALIGN_TOP_RIGHT},
      {"BL", LV_ALIGN_BOTTOM_LEFT},  {"BR", LV_ALIGN_BOTTOM_RIGHT}};
  for (auto& c : corners) {
    lv_obj_t* t = lv_label_create(scr);
    lv_label_set_text(t, c.txt);
    lv_obj_align(t, c.align, 0, 0);
  }

  // 2x4 tick grid near center: short vertical + horizontal ticks 8 px apart.
  // Reflective 2x4 packing mistakes show up as staggered/broken ticks here.
  static lv_point_t vseg[8][2];
  static lv_point_t hseg[8][2];
  const lv_coord_t cx = ST7305_W / 2, cy = ST7305_H / 2;
  for (int i = 0; i < 8; i++) {
    lv_coord_t gx = cx - 32 + i * 8;
    vseg[i][0] = {gx, (lv_coord_t)(cy - 20)};
    vseg[i][1] = {gx, (lv_coord_t)(cy - 4)};
    lv_obj_t* v = lv_line_create(scr);
    lv_line_set_points(v, vseg[i], 2);
    lv_obj_set_style_line_width(v, 1, 0);

    lv_coord_t gy = cy + 4 + i * 4;
    hseg[i][0] = {(lv_coord_t)(cx - 32), gy};
    hseg[i][1] = {(lv_coord_t)(cx + 32), gy};
    lv_obj_t* h = lv_line_create(scr);
    lv_line_set_points(h, hseg[i], 2);
    lv_obj_set_style_line_width(h, 1, 0);
  }

  // Center label with the native resolution, so a mismatch is obvious.
  lv_obj_t* label = lv_label_create(scr);
  lv_label_set_text(label, "express-ghalib\nST7305 400x300");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, -60);
}

// ---------------------------------------------------------------------------
// M0.5 status screen — live SHTC3 temp/humidity + async Wi-Fi SSID scan.
// A mock "sensors + radio" screen to exercise I2C and Wi-Fi on the panel.
// ---------------------------------------------------------------------------
static lv_obj_t* g_temp_label = nullptr;
static lv_obj_t* g_hum_label  = nullptr;
static lv_obj_t* g_wifi_label = nullptr;
static int g_wifi_cooldown = 0;  // seconds until next scan (see wifi_timer_cb)

static void sensor_timer_cb(lv_timer_t*) {
  float tC, rh;
  if (shtc3_read(tC, rh)) {
    // NOTE: use newlib snprintf, not lv_label_set_text_fmt — LVGL's built-in
    // printf drops %f unless LV_SPRINTF_USE_FLOAT is enabled.
    char tbuf[24], hbuf[24];
    snprintf(tbuf, sizeof(tbuf), "Temp   %.1f C", tC);
    snprintf(hbuf, sizeof(hbuf), "Hum    %.1f %%", rh);
    lv_label_set_text(g_temp_label, tbuf);
    lv_label_set_text(g_hum_label, hbuf);
  } else {
    lv_label_set_text(g_temp_label, "Temp   -- C");
    lv_label_set_text(g_hum_label,  "Hum    -- %");
  }
}

static void render_wifi(int n) {
  if (n <= 0) { lv_label_set_text(g_wifi_label, "(no networks found)"); return; }
  String s;
  const int show = n < 8 ? n : 8;  // panel space: cap the list
  for (int i = 0; i < show; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) ssid = "<hidden>";
    const bool locked = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    s += (locked ? "* " : "  ");
    s += ssid;
    s += "  ";
    s += String(WiFi.RSSI(i));
    s += "dBm\n";
  }
  if (n > show) s += "... +" + String(n - show) + " more";
  lv_label_set_text(g_wifi_label, s.c_str());
}

static void wifi_timer_cb(lv_timer_t*) {  // runs every 1000 ms
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (n >= 0) {                       // scan finished
    render_wifi(n);
    WiFi.scanDelete();
    g_wifi_cooldown = 15;             // re-scan every ~15 s
    return;
  }
  if (g_wifi_cooldown > 0) { g_wifi_cooldown--; return; }
  WiFi.scanNetworks(true /*async*/);  // kick a new scan
}

[[maybe_unused]] static void build_status_screen() {
  lv_obj_t* scr = lv_scr_act();

  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "express-ghalib  -  status");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  g_temp_label = lv_label_create(scr);
  lv_obj_set_style_text_font(g_temp_label, &lv_font_montserrat_20, 0);
  lv_label_set_text(g_temp_label, "Temp   -- C");
  lv_obj_align(g_temp_label, LV_ALIGN_TOP_LEFT, 16, 40);

  g_hum_label = lv_label_create(scr);
  lv_obj_set_style_text_font(g_hum_label, &lv_font_montserrat_20, 0);
  lv_label_set_text(g_hum_label, "Hum    -- %");
  lv_obj_align(g_hum_label, LV_ALIGN_TOP_LEFT, 16, 72);

  lv_obj_t* wifi_hdr = lv_label_create(scr);
  lv_label_set_text(wifi_hdr, "Wi-Fi networks  (* = secured):");
  lv_obj_align(wifi_hdr, LV_ALIGN_TOP_LEFT, 16, 116);

  g_wifi_label = lv_label_create(scr);
  lv_label_set_text(g_wifi_label, "scanning...");
  lv_obj_set_width(g_wifi_label, ST7305_W - 32);
  lv_obj_align(g_wifi_label, LV_ALIGN_TOP_LEFT, 16, 140);

  // Wi-Fi in station mode, not connecting to anything — just scanning.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  lv_timer_create(sensor_timer_cb, 2000, nullptr);  // temp/humidity every 2 s
  lv_timer_create(wifi_timer_cb, 1000, nullptr);     // drive the async scan
  sensor_timer_cb(nullptr);                          // first reading immediately
}

// ---------------------------------------------------------------------------
// M1 (iteration 1) — BLE scan screen. Lists nearby BLE devices; HID devices
// (like the 8BitDo keyboard in Bluetooth mode) are marked with '*'.
// ---------------------------------------------------------------------------
static lv_obj_t* g_ble_label = nullptr;

[[maybe_unused]] static void ble_timer_cb(lv_timer_t*) {
  static uint32_t tick = 0;
  tick++;
  lv_label_set_text(g_ble_label, ble_status_text().c_str());
  Serial.printf("[HB] tick=%lu heap=%lu\n", (unsigned long)tick,
                (unsigned long)ESP.getFreeHeap());
}

[[maybe_unused]] static void build_ble_screen() {
  lv_obj_t* scr = lv_scr_act();

  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "BLE keyboard host");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  g_ble_label = lv_label_create(scr);
  lv_label_set_text(g_ble_label, "scanning...");
  lv_obj_set_width(g_ble_label, ST7305_W - 32);
  lv_obj_align(g_ble_label, LV_ALIGN_TOP_LEFT, 16, 34);

  // Typing test: a textarea in the keypad group receives decoded BLE keys.
  lv_obj_t* ta = lv_textarea_create(scr);
  lv_obj_set_size(ta, ST7305_W - 32, 150);
  lv_obj_align(ta, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_textarea_set_placeholder_text(ta, "type on the BLE keyboard...");
  lv_group_add_obj(lv_group_get_default(), ta);
  lv_group_focus_obj(ta);

  lv_timer_create(ble_timer_cb, 1000, nullptr);  // refresh status every 1s
}

// ---------------------------------------------------------------------------
// BLE pairing overlay — a modal on lv_layer_top() that floats above whatever
// screen is active while a long-press re-pair runs. Pure visual feedback (no key
// input), so it works even when there is no functioning keyboard to navigate.
// ---------------------------------------------------------------------------
static lv_obj_t* g_pair_box    = nullptr;
static lv_obj_t* g_pair_label  = nullptr;
static int       g_pair_linger = 0;   // 200ms ticks to linger after pairing ends

static void pairing_overlay_show() {
  if (g_pair_box) return;
  g_pair_box = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_pair_box, ST7305_W - 60, 96);
  lv_obj_center(g_pair_box);
  lv_obj_set_style_bg_color(g_pair_box, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(g_pair_box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(g_pair_box, lv_color_black(), 0);
  lv_obj_set_style_border_width(g_pair_box, 2, 0);
  lv_obj_set_style_radius(g_pair_box, 6, 0);
  lv_obj_clear_flag(g_pair_box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* hdr = lv_label_create(g_pair_box);
  lv_label_set_text(hdr, LV_SYMBOL_BLUETOOTH "  Pairing");
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);

  g_pair_label = lv_label_create(g_pair_box);
  lv_obj_set_width(g_pair_label, ST7305_W - 96);
  lv_label_set_long_mode(g_pair_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(g_pair_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(g_pair_label, "");
  lv_obj_align(g_pair_label, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void pairing_overlay_hide() {
  if (!g_pair_box) return;
  lv_obj_del(g_pair_box);
  g_pair_box = g_pair_label = nullptr;
}

static void pairing_timer_cb(lv_timer_t*) {
  if (ble_kbd_pairing()) {                 // window open (or connect pending)
    pairing_overlay_show();
    lv_label_set_text(g_pair_label, ble_status_text().c_str());
    g_pair_linger = 8;                      // ~1.6 s linger once it ends
  } else if (g_pair_box) {                  // finished: show outcome, then fade
    lv_label_set_text(g_pair_label, ble_status_text().c_str());
    if (g_pair_linger > 0) g_pair_linger--;
    else pairing_overlay_hide();
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("express-ghalib boot");

  display_init();
  input_init();
  buttons_init();      // physical KEY/BOOT buttons
  storage_init();      // mount LittleFS for notes/config
  rtc_init();          // PCF85063 RTC (I2C) — dates for Daily Log
  launcher_build();    // home-screen app launcher (keyboard-navigated)
  settings_boot_sync();// if Wi-Fi creds saved: connect + NTP-sync the RTC, then off
  ble_init();          // start NimBLE central scan + auto-connect
  lv_timer_create(pairing_timer_cb, 200, nullptr);  // BLE re-pair overlay
}

void loop() {
  ble_loop();          // drive the BLE connect state machine
  buttons_poll();      // physical KEY/BOOT buttons
  lv_timer_handler();  // LVGL tick is driven by millis() via LV_TICK_CUSTOM
  delay(5);
}
