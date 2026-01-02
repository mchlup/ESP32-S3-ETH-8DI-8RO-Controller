#include "ConditionEvaluator.h"
#include "MqttController.h"
#include <time.h>

static bool evalInput(const Condition& c) {
  uint8_t in = c.input;
  if (in < 1 || in > INPUT_COUNT) return false;

  const bool active = inputGetState(static_cast<InputId>(in - 1));
  if (c.state == InputWanted::ACTIVE) {
    return active;
  }
  return !active;
}

static bool parseHmToMinutes(const char* hm, int* outMin) {
  if (!hm || !outMin) return false;
  int h = -1, m = -1;
  if (sscanf(hm, "%d:%d", &h, &m) != 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  *outMin = h * 60 + m;
  return true;
}

static bool timeIsValid() {
  time_t now = time(nullptr);
  return now > 1700000000; // jednoduchý sanity check, viz NetworkController.cpp
}

static int localMinutesNow() {
  time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  return tmNow.tm_hour * 60 + tmNow.tm_min;
}

static bool evalTimeWindow(const Condition& c) {
  if (!timeIsValid()) return false;

  int fromMin = 0, toMin = 0;
  if (!parseHmToMinutes(c.timeFrom, &fromMin)) return false;
  if (!parseHmToMinutes(c.timeTo, &toMin)) return false;

  // from == to => celý den
  if (fromMin == toMin) return true;

  const int nowMin = localMinutesNow();

  if (fromMin < toMin) {
    // běžný interval v rámci dne (from <= now < to)
    return (nowMin >= fromMin) && (nowMin < toMin);
  }

  // interval přes půlnoc (např. 22:00–06:00)
  return (nowMin >= fromMin) || (nowMin < toMin);
}

static bool evalMqttEquals(const Condition& c) {
  String last;
  if (!mqttGetLastValue(String(c.mqttTopic), &last)) return false;

  const String expected = String(c.mqttValue);
  if (!expected.length()) {
    // když není nastavená hodnota, stačí že topic existuje
    return true;
  }

  return last == expected;
}

bool condEvalOne(const Condition& c) {
  switch (c.type) {
    case CondType::INPUT_PIN: return evalInput(c);
    case CondType::TIME:
      return evalTimeWindow(c);
    case CondType::MQTT:
      return evalMqttEquals(c);
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
