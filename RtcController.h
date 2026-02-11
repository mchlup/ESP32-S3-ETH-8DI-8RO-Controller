#pragma once

#include <Arduino.h>

void rtcInit();
bool rtcIsPresent();

bool rtcGetEpoch(time_t& out);
bool rtcSetEpoch(time_t t);
