/**
 * audio.cpp — ES8311 minimal DAC playback for the reminder beep. See audio.h.
 *
 * The register sequence below is the esp_codec_dev ES8311 driver's DAC-playback
 * init (es8311.c: es8311_open -> set_fs -> start), flattened and resolved for
 * this board's case: ES8311 = I2S slave, ESP32 = master providing MCLK on GPIO16
 * at 256xfs, 16-bit standard-I2S, 16 kHz. Values are copied verbatim from that
 * driver so we don't hand-guess the ~30 clock/power registers.
 *
 * PA (GPIO46) is left LOW after init and only raised around a beep, so the amp
 * doesn't idle-hiss or draw current between reminders.
 */
#include "audio.h"
#include <Arduino.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include <math.h>

namespace {
constexpr uint8_t ES8311_ADDR = 0x18;
constexpr int PIN_MCLK = 16, PIN_BCLK = 9, PIN_WS = 45, PIN_DOUT = 8;
constexpr int PIN_PA   = 46;                 // speaker amp enable (active-high)
constexpr uint32_t SAMPLE_RATE = 16000;      // MCLK = 256*fs = 4.096 MHz

I2SClass g_i2s;
bool     g_ok = false;                       // codec detected + I2S up
uint32_t g_rate = SAMPLE_RATE;               // current I2S sample rate
bool     g_playing = false;                  // music streaming in progress

void es8311_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t es8311_read(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)ES8311_ADDR, 1);
  return Wire.available() ? Wire.read() : 0xFF;
}

// DAC-playback init, resolved for slave / MCLK 256xfs / 16-bit I2S / 16 kHz.
// {reg, val}; the 0x00 reset write gets an extra settle delay (see loop).
const uint8_t kInit[][2] = {
    // --- es8311_open(): power-on + clock-manager reset ---
    {0x44, 0x08}, {0x44, 0x08},              // GPIO44 noise immunity (x2 per driver)
    {0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10},
    {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00},
    {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F},
    {0x00, 0x80},                            // release reset, slave mode (+settle)
    {0x01, 0x3F},                            // CLK mgr: use MCLK pin, not inverted
    {0x06, 0x03}, {0x13, 0x10}, {0x1B, 0x0A}, {0x1C, 0x6A},
    {0x44, 0x58},                            // internal DAC/ADC reference
    // --- set_fs: format + clock coefficients (16-bit I2S, 16 kHz, 256fs) ---
    {0x09, 0x0C}, {0x0A, 0x0C},              // SDP in/out: 16-bit, standard I2S
    {0x02, 0x00}, {0x05, 0x00}, {0x03, 0x10},
    {0x04, 0x20},                            // dac_osr (16 kHz)
    {0x07, 0x00}, {0x08, 0xFF},              // LRCK divider = 0x00FF (256)
    {0x06, 0x03},                            // bclk_div
    // --- es8311_start(): power up DAC + unmute ---
    {0x00, 0x80}, {0x01, 0x3F}, {0x09, 0x0C}, {0x0A, 0x0C},
    {0x17, 0xBF}, {0x0E, 0x02},
    {0x12, 0x00},                            // enable/power up DAC
    {0x14, 0x1A}, {0x0D, 0x01}, {0x15, 0x40}, {0x37, 0x08}, {0x45, 0x00},
    {0x32, 0xBF},                            // DAC volume ~0 dB
    {0x31, 0x00},                            // unmute (clear bits 6:5)
};
}  // namespace

void audio_init() {
  pinMode(PIN_PA, OUTPUT);
  digitalWrite(PIN_PA, LOW);                 // amp off until a beep

  // Confirm the codec is on the bus (chip ID regs 0xFD=0x83, 0xFE=0x11).
  const uint8_t id1 = es8311_read(0xFD), id2 = es8311_read(0xFE);
  if (id1 != 0x83) {
    Serial.printf("[AUD] ES8311 not found (id=0x%02X 0x%02X)\n", id1, id2);
    return;
  }

  // I2S TX master: this starts MCLK/BCLK/WS so the codec has its clock during
  // register init. Mono source is duplicated to both slots at beep time.
  g_i2s.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, -1, PIN_MCLK);
  if (!g_i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO)) {
    Serial.println("[AUD] I2S begin failed");
    return;
  }

  for (auto& rv : kInit) {
    es8311_write(rv[0], rv[1]);
    if (rv[0] == 0x00) delay(20);            // let the reset/CSM settle
  }

  g_ok = true;
  Serial.println("[AUD] ES8311 ready (beep armed)");
}

void audio_beep() {
  if (!g_ok || g_playing) return;            // don't fight active music playback
  audio_prepare(SAMPLE_RATE);                // beep tone assumes 16 kHz

  digitalWrite(PIN_PA, HIGH);                // enable amp
  delay(2);                                  // let it settle

  // 320 frames = 20 whole periods of 1 kHz @ 16 kHz, so chunks concatenate
  // phase-continuously (no click). Stereo: L==R.
  constexpr int kFreq = 1000, kFrames = 320, kChunks = 10;  // ~200 ms
  static int16_t buf[kFrames * 2];
  for (int i = 0; i < kFrames; i++) {
    const int16_t s =
        (int16_t)(sinf(2.0f * PI * kFreq * i / SAMPLE_RATE) * 9000.0f);
    buf[2 * i] = s;
    buf[2 * i + 1] = s;
  }
  for (int c = 0; c < kChunks; c++)
    g_i2s.write((uint8_t*)buf, sizeof(buf));

  delay(20);                                 // let the DMA tail drain
  digitalWrite(PIN_PA, LOW);                 // amp off (no idle hiss)
}

bool audio_prepare(uint32_t rate) {
  if (!g_ok) return false;
  if (rate == g_rate) return true;
  g_i2s.end();
  if (!g_i2s.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO)) {
    Serial.printf("[AUD] I2S re-begin @%lu failed\n", (unsigned long)rate);
    g_ok = false;
    return false;
  }
  // Only the DAC oversample-ratio register tracks the rate; MCLK stays 256xfs so
  // the LRCK/BCLK dividers are unchanged. (16k uses 0x20, higher rates 0x10.)
  es8311_write(0x04, rate <= 24000 ? 0x20 : 0x10);
  g_rate = rate;
  Serial.printf("[AUD] rate -> %lu Hz\n", (unsigned long)rate);
  return true;
}

void audio_play_on()  { if (g_ok) { digitalWrite(PIN_PA, HIGH); g_playing = true; } }
void audio_play_off() { digitalWrite(PIN_PA, LOW); g_playing = false; }

size_t audio_write(const uint8_t* buf, size_t len) {
  return g_ok ? g_i2s.write(buf, len) : 0;
}

void audio_set_volume(uint8_t percent) {
  if (!g_ok) return;
  if (percent > 100) percent = 100;
  es8311_write(0x32, (uint8_t)((uint16_t)percent * 255 / 100));  // 0x32 DAC vol
}
