#include "ConditionEvaluator.h"

static bool evalInput(const Condition& c) {
  uint8_t in = c.input;
  if (in < 1 || in > INPUT_COUNT) return false;

  const bool active = inputGetState(static_cast<InputId>(in - 1));
  if (c.state == InputWanted::ACTIVE) {
    return active;
  }
  return !active;
}

bool condEvalOne(const Condition& c) {
  switch (c.type) {
    case CondType::INPUT_PIN: return evalInput(c);
    case CondType::TIME:
      // TODO: RTC/NTP + day-of-week + interval přes půlnoc
      return false;
    case CondType::MQTT:
      // TODO: MQTT cache (last value per topic) + compare operator
      return false;
    default:
      return false;
  }
}

bool condEvalGroup(const WhenGroup& g) {
  if (g.count == 0) return false;

  if (g.op == LogicOp::AND) {
    for (uint8_t i = 0; i < g.count; i++) {
      if (!condEvalOne(g.items[i])) return false;
    }
    return true;
  }

  // OR
  for (uint8_t i = 0; i < g.count; i++) {
    if (condEvalOne(g.items[i])) return true;
  }
  return false;
}
