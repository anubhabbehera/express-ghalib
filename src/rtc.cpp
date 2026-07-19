/**
 * rtc.cpp — PCF85063 RTC minimal driver. See rtc.h.
 * Registers: 0x04 sec (bit7 = OS/oscillator-stop), 0x05 min, 0x06 hour,
 * 0x07 day, 0x08 weekday, 0x09 month, 0x0A year (all BCD).
 */
#include "rtc.h"
#include <Arduino.h>
#include <Wire.h>

namespace {
constexpr uint8_t ADDR = 0x51;

uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

uint8_t read_reg(uint8_t r) {
  Wire.beginTransmission(ADDR);
  Wire.write(r);
  Wire.endTransmission(false);
  Wire.requestFrom((int)ADDR, 1);
  return Wire.available() ? Wire.read() : 0xFF;
}

// Set the clock (also clears the oscillator-stop flag).
void set_datetime(int y, int mo, int d, int h, int mi, int s) {
  Wire.beginTransmission(ADDR);
  Wire.write(0x04);
  Wire.write(dec2bcd(s) & 0x7F);   // clear OS
  Wire.write(dec2bcd(mi));
  Wire.write(dec2bcd(h));
  Wire.write(dec2bcd(d));
  Wire.write(0);                   // weekday (unused for now)
  Wire.write(dec2bcd(mo));
  Wire.write(dec2bcd((uint8_t)(y % 100)));
  Wire.endTransmission();
}

// Parse the compiler's __DATE__ ("Mmm dd yyyy") / __TIME__ ("hh:mm:ss").
void seed_from_build_time() {
  static const char* mons = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  int d = 1, y = 2026, h = 0, mi = 0, s = 0;
  sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
  sscanf(__TIME__, "%d:%d:%d", &h, &mi, &s);
  const char* p = strstr(mons, mon);
  const int mo = p ? (int)((p - mons) / 3 + 1) : 1;
  set_datetime(y, mo, d, h, mi, s);
  Serial.printf("[RTC] seeded from build time: %04d-%02d-%02d %02d:%02d\n",
                y, mo, d, h, mi);
}
}  // namespace

void rtc_init() {
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
  Wire.begin(13, 14, 100000);

  const uint8_t sec = read_reg(0x04);
  if (sec == 0xFF) { Serial.println("[RTC] not responding"); return; }
  if (sec & 0x80) seed_from_build_time();  // oscillator stopped -> never set
  else Serial.println("[RTC] clock is running");
}

void rtc_date_mmddyy(char* out) {
  const uint8_t d  = bcd2dec(read_reg(0x07) & 0x3F);
  const uint8_t mo = bcd2dec(read_reg(0x09) & 0x1F);
  const uint8_t y  = bcd2dec(read_reg(0x0A));
  snprintf(out, 9, "%02d-%02d-%02d", mo, d, y);
}
