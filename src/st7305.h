/**
 * st7305.h — ST7305 reflective-LCD driver (1-bit mono, 4-wire SPI)
 *
 * Ported for Arduino/LVGL from the Waveshare ESP32-S3-RLCD-4.2 reference driver
 * (via github.com/kylehase/ESPHome-ST7305-RLCD, itself from Waveshare's
 * custom_lcd_display.cc / display_bsp.cpp). Datasheet:
 *   https://files.waveshare.com/wiki/common/ST_7305_V0_2.pdf
 *
 * NOTE ON CONTROLLER: Waveshare/this driver call it ST7305; Zephyr's board port
 * calls it ST7306. Same command family (ST7306 adds 4-level grayscale). This
 * driver runs the panel in 1-bit monochrome, which works for both.
 *
 * PANEL IS NATIVELY LANDSCAPE 400x300. For a portrait 300x400 UI, let LVGL
 * software-rotate (sw_rotate/rotation) rather than fighting the packing.
 *
 * Pixel packing is NON-LINEAR (2x4 blocks, Y inverted) — never hand this panel
 * a plain horizontal bitmap. Use st7305_buf_set() to write pixels.
 */
#pragma once
#include <stdint.h>

// Native (landscape) geometry.
static constexpr uint16_t ST7305_W = 400;
static constexpr uint16_t ST7305_H = 300;
static constexpr uint32_t ST7305_BUF_BYTES = (uint32_t)ST7305_W * ST7305_H / 8;  // 15000

// Waveshare board pin map — from the official Arduino example user_config.h
// (RLCD_*_PIN). NOTE: these are GPIO11/12, NOT the 39/38 in the ESPHome port.
struct St7305Pins {
  int sck  = 11;  // RLCD_SCK_PIN
  int mosi = 12;  // RLCD_MOSI_PIN
  int cs   = 40;  // RLCD_CS_PIN
  int dc   = 5;   // RLCD_DC_PIN
  int rst  = 41;  // RLCD_RST_PIN
};

// SPI clock. Waveshare's own LVGL demo drives this panel at 10 MHz (pclk_hz),
// so 10 MHz is proven safe here. At 10 MHz a full 15 KB frame flushes in ~12 ms
// (vs ~60 ms at 2 MHz) — this is what keeps typing/redraws responsive.
static constexpr uint32_t ST7305_SPI_HZ = 10'000'000;

// Bring up SPI + run the full ST7305 power-on/init sequence. Leaves display on.
void st7305_init(const St7305Pins& pins = St7305Pins{});

// Set one pixel in a NATIVE-packed framebuffer (`buf` is ST7305_BUF_BYTES long).
// on == true  -> black (ink);  on == false -> white (background).
void st7305_buf_set(uint8_t* buf, uint16_t x, uint16_t y, bool on);

// Push a full native framebuffer to the panel (set address window + mem-write).
void st7305_flush_full(const uint8_t* buf);

// Duration (microseconds) of the SPI transfer in the last st7305_flush_full().
uint32_t st7305_last_flush_us();

// Switch the panel to Low Power Mode (~1 Hz self-refresh; image retained).
// Used before ESP32 deep sleep — the standby dashboard stays visible at ~zero
// power. Any later st7305_flush_full() re-asserts High Power Mode (0x38).
void st7305_low_power();
