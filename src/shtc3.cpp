/**
 * shtc3.cpp — minimal SHTC3 driver. Datasheet commands:
 *   wakeup 0x3517, sleep 0xB098, measure (T-first, normal, no clock-stretch)
 *   0x7866. Reads 6 bytes: T[msb,lsb,crc], RH[msb,lsb,crc]. CRC-8 poly 0x31.
 */
#include "shtc3.h"
#include <Arduino.h>
#include <Wire.h>

namespace {
constexpr uint8_t ADDR = 0x70;
bool g_begun = false;

uint8_t crc8(const uint8_t* d, int n) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

void cmd(uint16_t c) {
  Wire.beginTransmission(ADDR);
  Wire.write((uint8_t)(c >> 8));
  Wire.write((uint8_t)(c & 0xFF));
  Wire.endTransmission();
}
}  // namespace

void shtc3_init(int sda, int scl) {
  // Waveshare enables internal pull-ups on this bus; Arduino Wire doesn't do it
  // strongly by default. Force them on so the sensor can ACK.
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  Wire.begin(sda, scl, 100000);  // 100 kHz
  g_begun = true;
  cmd(0xB098);  // sleep, known clean state
}

bool shtc3_read(float& tC, float& rh) {
  if (!g_begun) return false;

  cmd(0x3517);              // wakeup
  delayMicroseconds(300);   // t_wakeup >= 240us
  cmd(0x7866);              // measure, T first, normal power, no clock stretch

  // Clock stretching is disabled, so the sensor NAKs the read until the
  // measurement is ready (~12.1ms max). Poll instead of a single fixed delay.
  uint8_t buf[6];
  int got = 0;
  for (int tries = 0; tries < 20; tries++) {
    delay(2);
    got = Wire.requestFrom((int)ADDR, 6);
    if (got == 6) break;
  }
  if (got != 6) { cmd(0xB098); return false; }

  for (int i = 0; i < 6; i++) buf[i] = Wire.read();
  cmd(0xB098);              // sleep

  if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) return false;

  const uint16_t rawT = ((uint16_t)buf[0] << 8) | buf[1];
  const uint16_t rawH = ((uint16_t)buf[3] << 8) | buf[4];
  tC = -45.0f + 175.0f * (float)rawT / 65535.0f;
  rh = 100.0f * (float)rawH / 65535.0f;
  return true;
}
