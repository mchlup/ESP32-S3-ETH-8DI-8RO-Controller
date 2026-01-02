#pragma once
#include <Arduino.h>

// Minimal RTC wrapper (PCF85063 family, I2C addr 0x51)
// - safe even when RTC is absent (functions return false)

void rtcInit();
bool rtcIsPresent();

// Read RTC time to epoch (UTC). Returns false if RTC missing or invalid.
bool rtcGetEpoch(time_t &outEpoch);

// Write epoch (UTC) to RTC. Returns false if RTC missing.
bool rtcSetEpoch(time_t epoch);
