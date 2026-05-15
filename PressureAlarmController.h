#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

struct PressureAlarmConfig {
  bool enabled = true;
  float minBar = 0.8f;
  float maxBar = 2.8f;
  float hysteresisBar = 0.05f;
};

struct PressureAlarmStatus {
  bool enabled = false;
  bool sensorValid = false;
  float pressureBar = NAN;
  bool lowActive = false;
  bool highActive = false;
  bool active = false;
  String state;
  uint32_t lastChangeMs = 0;
};

void pressureAlarmInit();
void pressureAlarmLoop();
void pressureAlarmReloadFromStore();

PressureAlarmConfig pressureAlarmGetConfig();
PressureAlarmStatus pressureAlarmGetStatus();
void pressureAlarmFillFastJson(JsonObject& out);
