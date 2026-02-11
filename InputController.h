#pragma once

#include <Arduino.h>

static constexpr uint8_t INPUT_COUNT = 8;

enum class InputId : uint8_t {
  IN1 = 0,
  IN2,
  IN3,
  IN4,
  IN5,
  IN6,
  IN7,
  IN8
};

using InputChangeCallback = void(*)(InputId id, bool state);

void inputInit();
void inputUpdate();
void inputSetCallback(InputChangeCallback cb);

// Debounced raw level (HIGH/LOW as bool: true=HIGH)
bool inputGetRaw(InputId id);
// Logical state (active/inactive) respecting configured polarity.
bool inputGetState(InputId id);
