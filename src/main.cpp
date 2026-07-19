/**
 * main.cpp — express-ghalib bring-up skeleton (M0 + M1 stubs)
 *
 * Wires LVGL (1-bit mono) -> ST7305 flush, and sketches the NimBLE HOGP-host
 * scan. Fill the TODOs milestone by milestone (see PLAN.md).
 */
#include <Arduino.h>
#include <lvgl.h>
#include "st7305.h"

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
  // TODO(M1): pop the next key from the BLE HID -> LVGL key queue.
  //   Map HID usage codes to LV_KEY_NEXT/PREV/ENTER/ESC + printable chars.
  data->state = LV_INDEV_STATE_RELEASED;
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
// the FIRST flash. What to look for:
//   * The border must hug all four panel edges (address window correct).
//   * Corner tags TL/TR/BL/BR must sit in the named corners (orientation ok).
//   * The diagonal must be a clean straight line corner-to-corner (no x/y swap).
//   * The 2x4 tick grid near center must be evenly spaced with no shear/stagger
//     (2x4-block packing correct). Staggered ticks => packing math is off.
// ---------------------------------------------------------------------------
static void build_hello_screen() {
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

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("express-ghalib boot");

  display_init();
  input_init();
  build_hello_screen();
}

void loop() {
  lv_timer_handler();  // LVGL tick is driven by millis() via LV_TICK_CUSTOM
  delay(5);
}
