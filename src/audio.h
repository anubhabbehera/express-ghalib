/**
 * audio.h — ES8311 codec (I2S) minimal DAC playback, just enough for the
 * reminder beep (M3.5). Full music playback is M4.
 *
 * Pins (Waveshare ESP32-S3-RLCD-4.2, confirmed from the factory demo):
 *   I2S  MCLK=16 BCLK=9 WS=45 DOUT=8   |   PA enable=46 (active-high)
 *   I2C  SDA=13 SCL=14 (shared Wire bus)   |   ES8311 @ 0x18
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

// Bring up the ES8311 (I2C register init) + I2S TX. Call once at boot AFTER the
// I2C bus is up (rtc_init() calls Wire.begin). No-op-safe if the codec is absent.
void audio_init();

// Play a short reminder tone on the speaker. Raises PA enable for the duration.
// No-op while music is playing (the visual reminder still shows).
void audio_beep();

// --- streaming playback (music player) -------------------------------------
// Reconfigure I2S + codec for a sample rate (no-op if unchanged). Returns false
// if the codec isn't up. Expects 16-bit; mono must be expanded to stereo first.
bool audio_prepare(uint32_t sample_rate);

// Enable/disable the speaker amp around a playback session.
void audio_play_on();
void audio_play_off();

// Write 16-bit stereo PCM to I2S. Blocks until the DMA accepts it (backpressure).
size_t audio_write(const uint8_t* buf, size_t len);
