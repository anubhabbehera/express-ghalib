/**
 * rtc.cpp — PCF85063 RTC minimal driver. See rtc.h.
 * Registers: 0x04 sec (bit7 = OS/oscillator-stop), 0x05 min, 0x06 hour,
 * 0x07 day, 0x08 weekday, 0x09 month, 0x0A year (all BCD).
 */
#include "rtc.h"
#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>
#include <time.h>
#include "config.h"

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

// Mirror the RTC into the system clock so time()/LittleFS mtimes are real
// (before this, file timestamps counted up from the 1970 epoch each boot).
void sync_system_clock() {
  const time_t t = rtc_epoch_utc();
  if (t <= 0) return;
  struct timeval tv = {t, 0};
  settimeofday(&tv, nullptr);
  Serial.printf("[RTC] system clock synced (epoch %ld)\n", (long)t);
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
  setenv("TZ", "UTC0", 1);  // RTC holds UTC; mktime() normalization must be UTC
  tzset();
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
  Wire.begin(13, 14, 100000);

  const uint8_t sec = read_reg(0x04);
  if (sec == 0xFF) { Serial.println("[RTC] not responding"); return; }
  if (sec & 0x80) seed_from_build_time();  // oscillator stopped -> never set
  else Serial.println("[RTC] clock is running");
  sync_system_clock();
}

void rtc_date_mmddyy(char* out) {
  const uint8_t d  = bcd2dec(read_reg(0x07) & 0x3F);
  const uint8_t mo = bcd2dec(read_reg(0x09) & 0x1F);
  const uint8_t y  = bcd2dec(read_reg(0x0A));
  snprintf(out, 9, "%02d-%02d-%02d", mo, d, y);
}

void rtc_set(int y, int mo, int d, int h, int mi, int s) {
  set_datetime(y, mo, d, h, mi, s);
  sync_system_clock();
  Serial.printf("[RTC] set to %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
}

void rtc_datetime(char* out) {
  const uint8_t s  = bcd2dec(read_reg(0x04) & 0x7F);
  const uint8_t mi = bcd2dec(read_reg(0x05) & 0x7F);
  const uint8_t h  = bcd2dec(read_reg(0x06) & 0x3F);
  const uint8_t d  = bcd2dec(read_reg(0x07) & 0x3F);
  const uint8_t mo = bcd2dec(read_reg(0x09) & 0x1F);
  const uint8_t y  = bcd2dec(read_reg(0x0A));
  (void)s;
  snprintf(out, 17, "20%02d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
}

time_t rtc_epoch_utc() {
  const uint8_t sec = read_reg(0x04);
  if (sec == 0xFF) return 0;                    // not responding
  struct tm tm = {};
  tm.tm_sec  = bcd2dec(sec & 0x7F);
  tm.tm_min  = bcd2dec(read_reg(0x05) & 0x7F);
  tm.tm_hour = bcd2dec(read_reg(0x06) & 0x3F);
  tm.tm_mday = bcd2dec(read_reg(0x07) & 0x3F);
  tm.tm_mon  = bcd2dec(read_reg(0x09) & 0x1F) - 1;
  tm.tm_year = 100 + bcd2dec(read_reg(0x0A));
  tm.tm_isdst = 0;
  return mktime(&tm);                            // TZ=UTC0 -> UTC epoch
}

void rtc_local_datetime(char* out) {
  struct tm tm = {};
  tm.tm_sec  = bcd2dec(read_reg(0x04) & 0x7F);
  tm.tm_min  = bcd2dec(read_reg(0x05) & 0x7F);
  tm.tm_hour = bcd2dec(read_reg(0x06) & 0x3F);
  tm.tm_mday = bcd2dec(read_reg(0x07) & 0x3F);
  tm.tm_mon  = bcd2dec(read_reg(0x09) & 0x1F) - 1;
  tm.tm_year = 100 + bcd2dec(read_reg(0x0A));   // 2000+yy, as years since 1900
  tm.tm_min += config_get_tz_offset();          // shift; mktime rolls the overflow
  tm.tm_isdst = 0;
  mktime(&tm);                                   // normalize in place (TZ=UTC0)
  snprintf(out, 17, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
           tm.tm_mday, tm.tm_hour, tm.tm_min);
}
