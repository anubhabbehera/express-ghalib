/**
 * audio.h — ES8311 codec (I2S) minimal DAC playback, just enough for the
 * reminder beep (M3.5). Full music playback is M4.
 *
 * Pins (Waveshare ESP32-S3-RLCD-4.2, confirmed from the factory demo):
 *   I2S  MCLK=16 BCLK=9 WS=45 DOUT=8   |   PA enable=46 (active-high)
 *   I2C  SDA=13 SCL=14 (shared Wire bus)   |   ES8311 @ 0x18
 */
#pragma once

// Bring up the ES8311 (I2C register init) + I2S TX. Call once at boot AFTER the
// I2C bus is up (rtc_init() calls Wire.begin). No-op-safe if the codec is absent.
void audio_init();

// Play a short reminder tone on the speaker. Raises PA enable for the duration.
void audio_beep();
