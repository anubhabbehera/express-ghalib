/**
 * st7305.cpp — ST7305 reflective-LCD driver. See st7305.h for provenance.
 *
 * Init sequence + address window + 2x4 pixel packing are ported verbatim from
 * the Waveshare ESP32-S3-RLCD-4.2 reference driver. Verified against the panel's
 * WAVESHARE_400X300 model settings.
 */
#include "st7305.h"
#include <Arduino.h>
#include <SPI.h>

namespace {
St7305Pins g_pins{};
SPIClass* g_spi = nullptr;
volatile uint32_t g_flush_us = 0;   // SPI transfer time of the last flush
SPISettings g_spi_cfg(ST7305_SPI_HZ, MSBFIRST, SPI_MODE0);

// WAVESHARE_400X300 address window (from the reference driver).
constexpr uint8_t COL_START = 0x12, COL_END = 0x2A;  // column address (0x2A)
constexpr uint8_t ROW_START = 0x00, ROW_END = 0xC7;  // row address (0x2B)

inline void dc_cmd()  { digitalWrite(g_pins.dc, LOW); }
inline void dc_data() { digitalWrite(g_pins.dc, HIGH); }
inline void cs_lo()   { digitalWrite(g_pins.cs, LOW); }
inline void cs_hi()   { digitalWrite(g_pins.cs, HIGH); }

void cmd(uint8_t c) {
  dc_cmd(); cs_lo();
  g_spi->transfer(c);
  cs_hi();
}
void dat(uint8_t d) {
  dc_data(); cs_lo();
  g_spi->transfer(d);
  cs_hi();
}

void hardware_reset() {
  digitalWrite(g_pins.rst, HIGH); delay(50);
  digitalWrite(g_pins.rst, LOW);  delay(20);
  digitalWrite(g_pins.rst, HIGH); delay(50);
}

void init_sequence() {
  cmd(0xD6); dat(0x17); dat(0x02);                       // NVM load control
  cmd(0xD1); dat(0x01);                                   // Booster enable
  cmd(0xC0); dat(0x11); dat(0x04);                        // Gate voltage (VGH/VGL)
  cmd(0xC1); dat(0x69); dat(0x69); dat(0x69); dat(0x69);  // VSHP (source +, HPM)
  cmd(0xC2); dat(0x19); dat(0x19); dat(0x19); dat(0x19);  // VSLP (source +, LPM)
  cmd(0xC4); dat(0x4B); dat(0x4B); dat(0x4B); dat(0x4B);  // VSHN (source -, HPM)
  cmd(0xC5); dat(0x19); dat(0x19); dat(0x19); dat(0x19);  // VSLN (source -, LPM)
  cmd(0xD8); dat(0x80); dat(0xE9);                        // OSC frequency
  cmd(0xB2); dat(0x02);                                   // Frame rate
  cmd(0xB3); dat(0xE5); dat(0xF6); dat(0x05); dat(0x46);  // Gate EQ (HPM)
             dat(0x77); dat(0x77); dat(0x77); dat(0x77);
             dat(0x76); dat(0x45);
  cmd(0xB4); dat(0x05); dat(0x46); dat(0x77); dat(0x77);  // Gate EQ (LPM)
             dat(0x77); dat(0x77); dat(0x76); dat(0x45);
  cmd(0x62); dat(0x32); dat(0x03); dat(0x1F);             // Gate timing
  cmd(0xB7); dat(0x13);                                   // Source EQ enable
  cmd(0xB0); dat(0x64);                                   // Gate lines: 100*3 = 300
  cmd(0x11); delay(200);                                  // Sleep out
  cmd(0xC9); dat(0x00);                                   // Source voltage select
  cmd(0x36); dat(0x48);                                   // MADCTL (MX=1, DO=1)
  cmd(0x3A); dat(0x11);                                   // Data format: 1-bit mono
  cmd(0xB9); dat(0x20);                                   // Gamma: mono
  cmd(0xB8); dat(0x29);                                   // Panel: 1-dot inversion
  cmd(0x21);                                              // Display inversion on
  cmd(0x2A); dat(COL_START); dat(COL_END);               // Column address set
  cmd(0x2B); dat(ROW_START); dat(ROW_END);               // Row address set
  cmd(0x35); dat(0x00);                                   // Tearing effect on
  cmd(0xD0); dat(0xFF);                                   // Auto power down
  cmd(0x38);                                              // High power mode on
  cmd(0x29);                                              // Display on
}
}  // namespace

void st7305_init(const St7305Pins& pins) {
  g_pins = pins;
  pinMode(g_pins.cs, OUTPUT);
  pinMode(g_pins.dc, OUTPUT);
  pinMode(g_pins.rst, OUTPUT);
  cs_hi();

  g_spi = new SPIClass(HSPI);
  g_spi->begin(g_pins.sck, -1 /*miso*/, g_pins.mosi, g_pins.cs);
  g_spi->beginTransaction(g_spi_cfg);  // held open; single writer

  hardware_reset();
  init_sequence();
}

// Native 2x4-block packing (landscape), Y inverted to match MADCTL 0x48.
// byte holds 2 cols x 4 rows; see st7305.h. Reference: InitLandscapeLUT().
void st7305_buf_set(uint8_t* buf, uint16_t x, uint16_t y, bool on) {
  if (x >= ST7305_W || y >= ST7305_H) return;
  constexpr uint16_t H4 = ST7305_H >> 2;            // vertical blocks (75)
  const uint16_t inv_y   = ST7305_H - 1 - y;
  const uint16_t block_y = inv_y >> 2;
  const uint8_t  local_y = inv_y & 3;
  const uint16_t byte_x  = x >> 1;
  const uint8_t  local_x = x & 1;
  const uint32_t idx  = (uint32_t)byte_x * H4 + block_y;
  const uint8_t  mask = 1 << (7 - ((local_y << 1) | local_x));
  if (on) buf[idx] &= ~mask;   // black = bit clear
  else    buf[idx] |=  mask;   // white = bit set
}

void st7305_flush_full(const uint8_t* buf) {
  cmd(0x38);                                   // ensure HPM
  cmd(0x29);                                   // ensure display on
  cmd(0x2A); dat(COL_START); dat(COL_END);
  cmd(0x2B); dat(ROW_START); dat(ROW_END);

  // Memory write: CS stays LOW across the command + the whole framebuffer.
  dc_cmd(); cs_lo();
  g_spi->transfer(0x2C);
  dc_data();
  const uint32_t t0 = micros();
  g_spi->writeBytes(buf, ST7305_BUF_BYTES);
  g_flush_us = micros() - t0;
  cs_hi();
}

uint32_t st7305_last_flush_us() { return g_flush_us; }
