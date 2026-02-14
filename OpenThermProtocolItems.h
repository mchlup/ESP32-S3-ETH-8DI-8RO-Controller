#pragma once
#include <Arduino.h>

namespace OpenThermProtocol {
  struct DataIdInfo {
    uint8_t id;
    const char* name;
    const char* type; // e.g. f8.8, u8, u16, flag8, u8/u8
    const char* unit; // e.g. °C, bar, %, kW
    const char* rw;   // "R", "W", "R/W"
  };

  // Extracted from OpenTherm Protocol Specification v2.2 (table pages 24-30).
  extern const DataIdInfo DATA_IDS[];
  extern const uint8_t DATA_ID_COUNT;

  const DataIdInfo* find(uint8_t id);
}
