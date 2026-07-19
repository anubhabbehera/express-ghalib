/**
 * shtc3.h — Sensirion SHTC3 temperature/humidity sensor (I2C).
 * On the ESP32-S3-RLCD-4.2 the SHTC3 shares the I2C bus (SDA=13, SCL=14) with
 * the PCF85063 RTC. I2C address 0x70.
 */
#pragma once

// Bring up the shared I2C bus. Safe to call once at startup.
void shtc3_init(int sda = 13, int scl = 14);

// One-shot measurement. Returns false on I2C/CRC failure (outputs untouched).
// tC = temperature in Celsius, rh = relative humidity in %.
bool shtc3_read(float& tC, float& rh);
