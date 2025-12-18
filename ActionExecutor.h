#pragma once
#include <Arduino.h>
#include "RelayController.h"

// V1 podporuje jen RELAY_SET.
// RELAY_PULSE / MQTT_PUBLISH jsou připravené jako placeholder.
enum class ActionType : uint8_t {
  RELAY_SET = 0,
  RELAY_PULSE,
  MQTT_PUBLISH
};

struct Action {
  ActionType type = ActionType::RELAY_SET;

  // RELAY_SET / RELAY_PULSE
  uint8_t relay = 1;     // 1..8
  bool value = false;    // RELAY_SET: ON/OFF
  uint16_t pulseMs = 500;// RELAY_PULSE placeholder

  // MQTT_PUBLISH placeholder
  char mqttTopic[64] = {0};
  char mqttPayload[64] = {0};
};

struct RelayDecision {
  bool has = false;
  bool value = false;
  uint32_t ownerRuleId = 0;

  // ochrany (ms) – použije se pro aplikaci změn (min ON/OFF)
  uint32_t minOnMs = 0;
  uint32_t minOffMs = 0;
};

// aplikuje akce do “decision” pole (nepřepíná relé hned – jen nastaví požadavek)
void actionsApply(
  const Action* actions,
  uint8_t actionCount,
  RelayDecision decisions[RELAY_COUNT],
  uint32_t ownerRuleId,
  uint32_t minOnMs,
  uint32_t minOffMs
);
