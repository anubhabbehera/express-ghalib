/**
 * music.h — Music player (M4). Step 1: mount the microSD (SDMMC 1-bit) and
 * browse /music for audio files. WAV playback (reusing audio.cpp's ES8311/I2S)
 * and MP3 decode come in later steps.
 *
 * SD pins (Waveshare ESP32-S3-RLCD-4.2, confirmed from factory demo):
 *   SDMMC 1-bit: CLK=38 CMD=21 D0=39, mount "/sdcard". No bus conflict w/ display.
 */
#pragma once

// Open the Music app: mounts the SD card (lazily) and shows the /music list.
void music_open();

// Pump WAV streaming (one chunk per call). Call from loop(); no-op when idle.
void music_task();
