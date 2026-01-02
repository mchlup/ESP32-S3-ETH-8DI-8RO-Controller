#pragma once
#include <Arduino.h>

void networkInit();
bool networkIsConnected();
String networkGetIp();

// Config + time helpers (NTP/RTC)
void networkApplyConfig(const String& json);
void networkLoop();
bool networkIsTimeValid();
uint32_t networkGetTimeEpoch();
String networkGetTimeIso();
String networkGetTimeSource();
bool networkIsRtcPresent();
