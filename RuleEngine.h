#pragma once
#include <Arduino.h>
#include "RelayController.h"

// Stav pro diagnostiku /api/rules/status
struct RuleEngineStatus {
  bool     enabled = false;
  uint32_t lastEvalMs = 0;           // millis() kdy proběhlo poslední vyhodnocení
  uint32_t lastEvalDurationUs = 0;   // jak dlouho trvalo vyhodnocení (mikrosekundy)
  uint16_t ruleCount = 0;

  // pro každý relay: zda engine rozhodl hodnotu, a jakou
  bool     hasDecision[RELAY_COUNT] = {false};
  bool     desiredRelay[RELAY_COUNT] = {false};

  // kdo nastavil (id pravidla), 0 = nikdo
  uint32_t relayOwner[RELAY_COUNT] = {0};

  // poslední “aktivní” pravidlo (nejvyšší priority, které matchlo), 0 = žádné
  uint32_t activeRuleId = 0;
};

void ruleEngineInit();

// volat pravidelně v loop() – engine se spouští v interním intervalu (default 100ms)
void ruleEngineUpdate();

// načtení a parsování pravidel z JSON (typicky obsah /rules.json)
// očekává objekt: {"enabled":true/false, "rules":[...]}
bool ruleEngineLoadFromJson(const String& json, String* errOut = nullptr);

// vrátí poslední načtený JSON (canonical: to co engine drží v RAM)
String ruleEngineExportJson();

// enable/disable
void ruleEngineSetEnabled(bool en);
bool ruleEngineIsEnabled();

// interval vyhodnocování (ms), default 100
void ruleEngineSetTickMs(uint16_t tickMs);
uint16_t ruleEngineGetTickMs();

// status pro API
RuleEngineStatus ruleEngineGetStatus();

// helper pro API – připraví JSON status string
String ruleEngineGetStatusJson();
