#pragma once

#include <Arduino.h>

// Very small config store.
// Currently used for input polarities (active LOW/HIGH).

namespace ConfigStore {
  void begin();

  uint8_t getInputActiveLevel(uint8_t inputIndex); // 0=active LOW, 1=active HIGH
  void setInputActiveLevels(const uint8_t* levels, uint8_t count);
}
