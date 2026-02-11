#pragma once

#include <Arduino.h>

enum class RgbLedMode : uint8_t {
  OFF = 0,
  SOLID,
  BLINK,

  // Status modes used by Logic/BLE diagnostics
  BLE_DISABLED,
  BLE_IDLE,
  BLE_CONNECTED,
  BLE_PAIRING,
  ERROR
};

#if defined(FEATURE_RGB_LED)

void rgbLedInit();
void rgbLedLoop();

void rgbLedOff();
void rgbLedSetColor(uint8_t r, uint8_t g, uint8_t b);

// High-level state setter (used by LogicController)
void rgbLedSetMode(RgbLedMode mode);

// Blink with on/off period in ms
void rgbLedBlink(uint8_t r, uint8_t g, uint8_t b, uint16_t periodMs = 500);

#else

inline void rgbLedInit() {}
inline void rgbLedLoop() {}
inline void rgbLedOff() {}
inline void rgbLedSetColor(uint8_t, uint8_t, uint8_t) {}
inline void rgbLedSetMode(RgbLedMode) {}
inline void rgbLedBlink(uint8_t, uint8_t, uint8_t, uint16_t = 0) {}

#endif
