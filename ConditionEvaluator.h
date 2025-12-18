#pragma once
#include <Arduino.h>
#include "InputController.h"

// V1 podporuje jen INPUT. TIME/MQTT jsou připravené placeholderem.
// POZOR: Arduino core definuje makro INPUT, proto nepoužívat položku enumu "INPUT".
enum class CondType : uint8_t {
  INPUT_PIN = 0,
  TIME,
  MQTT
};

enum class LogicOp : uint8_t {
  AND = 0,
  OR
};

enum class InputWanted : uint8_t {
  ACTIVE = 0,
  INACTIVE
};

struct Condition {
  CondType type = CondType::INPUT_PIN;

  // INPUT
  uint8_t input = 1; // 1..8
  InputWanted state = InputWanted::ACTIVE;

  // TIME (placeholder)
  char timeFrom[6] = "00:00";
  char timeTo[6]   = "00:00";

  // MQTT (placeholder)
  char mqttTopic[64] = {0};
  char mqttValue[32] = {0};
};

struct WhenGroup {
  LogicOp op = LogicOp::AND;
  uint8_t count = 0;
  Condition items[8]; // max 8 podmínek v jedné skupině (V1)
};

// vyhodnotí 1 podmínku (V1: INPUT funguje, ostatní false)
bool condEvalOne(const Condition& c);

// vyhodnotí WHEN skupinu (AND/OR)
bool condEvalGroup(const WhenGroup& g);
