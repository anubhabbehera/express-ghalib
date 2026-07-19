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

// Set the clock (e.g. from NTP or manual entry). Clears the oscillator-stop flag.
void rtc_set(int year, int month, int day, int hour, int minute, int second);

// Write current date+time as "YYYY-MM-DD HH:MM" into out (needs >= 17 bytes).
void rtc_datetime(char* out);
