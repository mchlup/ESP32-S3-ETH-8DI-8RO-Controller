#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace EventLog {
  void begin();
  void clear();
  void record(const char* source, const char* event, const char* detail = nullptr, const char* level = "info");
  void fillJson(JsonArray out, size_t maxItems = 64);
  String toJson(size_t maxItems = 64);
}
