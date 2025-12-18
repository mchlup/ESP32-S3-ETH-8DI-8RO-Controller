#include "ActionExecutor.h"

static void applyRelaySet(const Action& a,
                          RelayDecision decisions[RELAY_COUNT],
                          uint32_t ownerRuleId,
                          uint32_t minOnMs,
                          uint32_t minOffMs) {
  if (a.relay < 1 || a.relay > RELAY_COUNT) return;
  const uint8_t idx = a.relay - 1;

  // Pokud už relé rozhodl vyšší prioritou jiný rule, necháme první rozhodnutí (first-wins).
  if (decisions[idx].has) return;

  decisions[idx].has = true;
  decisions[idx].value = a.value;
  decisions[idx].ownerRuleId = ownerRuleId;
  decisions[idx].minOnMs = minOnMs;
  decisions[idx].minOffMs = minOffMs;
}

void actionsApply(const Action* actions,
                  uint8_t actionCount,
                  RelayDecision decisions[RELAY_COUNT],
                  uint32_t ownerRuleId,
                  uint32_t minOnMs,
                  uint32_t minOffMs) {
  if (!actions || actionCount == 0) return;

  for (uint8_t i = 0; i < actionCount; i++) {
    const Action& a = actions[i];

    switch (a.type) {
      case ActionType::RELAY_SET:
        applyRelaySet(a, decisions, ownerRuleId, minOnMs, minOffMs);
        break;

      case ActionType::RELAY_PULSE:
        // TODO: neblokující pulse plánovač (per relay)
        // V1 ignorujeme (aby se nic “nečekaného” nedělo)
        break;

      case ActionType::MQTT_PUBLISH:
        // TODO: publish přes MqttController (pokud connected)
        break;
    }
  }
}
