#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace HistoryBuffer {
  void begin();
  void loop();
  void clear();
  void fillJson(JsonArray out, size_t maxItems = 180);
  String toJson(size_t maxItems = 180);
}
