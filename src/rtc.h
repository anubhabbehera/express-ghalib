/**
 * rtc.h — PCF85063 real-time clock (I2C 0x51). Minimal read for now (M2 needs a
 * date for the Daily Log template); full time-setting / NTP sync is M3.
 */
#pragma once

// Init the I2C bus + RTC. If the clock was never set (oscillator-stop flag),
// seed it from the firmware build date/time so we have a sensible date.
void rtc_init();

// Write today's date as "MM-DD-YY" into out (needs >= 9 bytes).
void rtc_date_mmddyy(char* out);
