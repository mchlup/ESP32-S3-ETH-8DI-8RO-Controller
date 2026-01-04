#include "RuleEngine.h"
#include "config_pins.h"

#if FEATURE_RULE_ENGINE
#include <ArduinoJson.h>
#include <string.h>

#include "LogicController.h"
#include "InputController.h"
#include "RelayController.h"

#include "ConditionEvaluator.h"
#include "ActionExecutor.h"

// limity V1 (můžeš kdykoliv navýšit)
static const uint8_t  MAX_RULES = 24;
static const uint8_t  MAX_ACTIONS = 8;
// Pozn.: UI umí mít delší popisy + více pravidel, proto necháváme větší rezervu.
// ESP32-S3 má dost RAM, 16 kB je stále bezpečné.
static const uint16_t JSON_DOC_CAP = 24576;   // pro rules.json (UI může mít delší texty)

struct RuleItem {
  uint32_t id = 0;
  bool enabled = true;

  char name[32] = {0};
  char desc[96] = {0};

  int16_t priority = 50;
  bool stopOnMatch = false;

  WhenGroup when;
  uint8_t actionCount = 0;
  Action actions[MAX_ACTIONS];

  // ochrany
  uint16_t debounceMs = 0;
  uint16_t minOnMs = 0;
  uint16_t minOffMs = 0;

  // runtime debounce
  bool rawMatch = false;
  bool stableMatch = false;
  uint32_t lastRawChangeMs = 0;
};

static bool     s_enabled = false;
static uint16_t s_tickMs = 100;
static uint32_t s_lastTickMs = 0;

static RuleItem s_rules[MAX_RULES];
static uint8_t  s_ruleCount = 0;

static String   s_rulesJson = "{\"enabled\":false,\"rules\":[]}";

// Když žádné pravidlo neplatí:
// - false: drž poslední stav (původní chování)
// - true: vypni relé, která jsou „řízená pravidly“ (tj. vyskytují se v akcích relay_set)
static bool s_defaultOffControlled = true;

static bool     s_relayControlled[RELAY_COUNT] = {0};
static uint16_t s_relayMaxMinOn[RELAY_COUNT] = {0};
static uint16_t s_relayMaxMinOff[RELAY_COUNT] = {0};

static uint32_t s_orderIds[MAX_RULES] = {0};
static uint8_t  s_orderCount = 0;

static RuleEngineStatus s_status;

// per-relay change tracking for minOn/minOff
static uint32_t s_relayLastChangeMs[RELAY_COUNT] = {0};

static void resetStatus() {
  s_status = RuleEngineStatus{};
  s_status.enabled = s_enabled;
  s_status.ruleCount = s_ruleCount;
}

static void updateRuleDebounce(RuleItem& r, bool newRaw, uint32_t now) {
  if (newRaw != r.rawMatch) {
    r.rawMatch = newRaw;
    r.lastRawChangeMs = now;
  }

  const uint16_t db = r.debounceMs;
  if (db == 0) {
    r.stableMatch = r.rawMatch;
    return;
  }

  if ((uint32_t)(now - r.lastRawChangeMs) >= (uint32_t)db) {
    r.stableMatch = r.rawMatch;
  }
}

static void clearRules() {
  s_ruleCount = 0;
  s_orderCount = 0;
  for (uint8_t r = 0; r < RELAY_COUNT; r++) {
    s_relayControlled[r] = false;
    s_relayMaxMinOn[r] = 0;
    s_relayMaxMinOff[r] = 0;
  }
  for (uint8_t i = 0; i < MAX_RULES; i++) {
    s_orderIds[i] = 0;
  }
  for (uint8_t i = 0; i < MAX_RULES; i++) s_rules[i] = RuleItem{};
}

static LogicOp parseOp(const char* op) {
  if (!op) return LogicOp::AND;
  String s(op);
  s.toUpperCase();
  return (s == "OR") ? LogicOp::OR : LogicOp::AND;
}

static CondType parseCondType(const char* t) {
  if (!t) return CondType::INPUT_PIN;
  String s(t);
  s.toLowerCase();
  if (s == "time") return CondType::TIME;
  if (s == "mqtt") return CondType::MQTT;
  return CondType::INPUT_PIN;
}

static InputWanted parseInputWanted(const char* st) {
  if (!st) return InputWanted::ACTIVE;
  String s(st);
  s.toUpperCase();
  return (s == "INACTIVE") ? InputWanted::INACTIVE : InputWanted::ACTIVE;
}

static ActionType parseActionType(const char* t) {
  if (!t) return ActionType::RELAY_SET;
  String s(t);
  s.toLowerCase();
  if (s == "relay_pulse") return ActionType::RELAY_PULSE;
  if (s == "mqtt_publish") return ActionType::MQTT_PUBLISH;
  return ActionType::RELAY_SET;
}

static void safeCopy(char* dst, size_t dstSize, const char* src) {
  if (!dst || dstSize == 0) return;
  dst[0] = 0;
  if (!src) return;
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = 0;
}

static RuleItem* findRuleById(uint32_t id) {
  for (uint8_t i = 0; i < s_ruleCount; i++) {
    if (s_rules[i].id == id) return &s_rules[i];
  }
  return nullptr;
}

static const char* opToStr(LogicOp op) {
  return (op == LogicOp::OR) ? "OR" : "AND";
}

static const char* condTypeToStr(CondType t) {
  switch (t) {
    case CondType::TIME: return "time";
    case CondType::MQTT: return "mqtt";
    default: return "input";
  }
}

static const char* inputWantedToStr(InputWanted w) {
  return (w == InputWanted::INACTIVE) ? "INACTIVE" : "ACTIVE";
}

static const char* actionTypeToStr(ActionType t) {
  switch (t) {
    case ActionType::RELAY_PULSE: return "relay_pulse";
    case ActionType::MQTT_PUBLISH: return "mqtt_publish";
    default: return "relay_set";
  }
}

static void rebuildExportJson() {
  DynamicJsonDocument out(JSON_DOC_CAP);

  out["enabled"] = s_enabled;
  out["defaultOffControlled"] = s_defaultOffControlled;

  JsonArray arr = out.createNestedArray("rules");

  // UI pořadí držíme přes s_orderIds, aby se pravidla po uložení „nepřeházela“
  for (uint8_t oi = 0; oi < s_orderCount; oi++) {
    RuleItem* r = findRuleById(s_orderIds[oi]);
    if (!r) continue;

    JsonObject ro = arr.createNestedObject();
    ro["id"] = r->id;
    ro["enabled"] = r->enabled;
    ro["name"] = r->name;
    ro["desc"] = r->desc;
    ro["priority"] = r->priority;
    ro["stopOnMatch"] = r->stopOnMatch;
    ro["debounceMs"] = r->debounceMs;
    ro["minOnMs"] = r->minOnMs;
    ro["minOffMs"] = r->minOffMs;

    JsonObject wo = ro.createNestedObject("when");
    wo["op"] = opToStr(r->when.op);
    JsonArray items = wo.createNestedArray("items");
    for (uint8_t i = 0; i < r->when.count; i++) {
      const Condition& c = r->when.items[i];
      JsonObject co = items.createNestedObject();
      co["type"] = condTypeToStr(c.type);
      if (c.type == CondType::INPUT_PIN) {
        co["input"] = c.input;
        co["state"] = inputWantedToStr(c.state);
      } else if (c.type == CondType::TIME) {
        co["from"] = c.timeFrom;
        co["to"] = c.timeTo;
      } else if (c.type == CondType::MQTT) {
        co["topic"] = c.mqttTopic;
        co["value"] = c.mqttValue;
      }
    }

    JsonArray th = ro.createNestedArray("then");
    for (uint8_t ai = 0; ai < r->actionCount; ai++) {
      const Action& a = r->actions[ai];
      JsonObject ao = th.createNestedObject();
      ao["type"] = actionTypeToStr(a.type);
      if (a.type == ActionType::RELAY_SET) {
        ao["relay"] = a.relay;
        ao["value"] = a.value;
      } else if (a.type == ActionType::RELAY_PULSE) {
        ao["relay"] = a.relay;
        ao["ms"] = a.pulseMs;
      } else if (a.type == ActionType::MQTT_PUBLISH) {
        ao["topic"] = a.mqttTopic;
        ao["payload"] = a.mqttPayload;
      }
    }
  }

  String outStr;
  serializeJson(out, outStr);
  s_rulesJson = outStr;
}

static void sortRulesByPriorityDesc() {
  // jednoduché, MAX_RULES malé
  for (uint8_t i = 0; i < s_ruleCount; i++) {
    for (uint8_t j = i + 1; j < s_ruleCount; j++) {
      if (s_rules[j].priority > s_rules[i].priority) {
        RuleItem tmp = s_rules[i];
        s_rules[i] = s_rules[j];
        s_rules[j] = tmp;
      }
    }
  }
}

// Aplikace decisions do skutečných relé s ochranami minOn/minOff
static void applyDecisions(const RelayDecision decisions[RELAY_COUNT], uint32_t now) {
  for (uint8_t r = 0; r < RELAY_COUNT; r++) {
    if (!decisions[r].has) continue;

    const bool cur = relayGetState(static_cast<RelayId>(r));
    const bool want = decisions[r].value;

    s_status.hasDecision[r] = true;
    s_status.desiredRelay[r] = want;
    s_status.relayOwner[r] = decisions[r].ownerRuleId;

    if (cur == want) continue;

    // min ON/OFF enforcement
    const uint32_t since = (uint32_t)(now - s_relayLastChangeMs[r]);

    if (cur == false && want == true) {
      // OFF->ON, respektuj minOff
      if (since < decisions[r].minOffMs) continue;
    } else if (cur == true && want == false) {
      // ON->OFF, respektuj minOn
      if (since < decisions[r].minOnMs) continue;
    }

    relaySet(static_cast<RelayId>(r), want);
    s_relayLastChangeMs[r] = now;
  }
}

void ruleEngineInit() {
  clearRules();
  resetStatus();

  const uint32_t now = millis();
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    s_relayLastChangeMs[i] = now;
  }

  s_lastTickMs = 0;
}

void ruleEngineSetTickMs(uint16_t tickMs) {
  if (tickMs < 20) tickMs = 20;
  s_tickMs = tickMs;
}
uint16_t ruleEngineGetTickMs() { return s_tickMs; }

void ruleEngineSetEnabled(bool en) {
  s_enabled = en;
  s_status.enabled = s_enabled;
}
bool ruleEngineIsEnabled() { return s_enabled; }

RuleEngineStatus ruleEngineGetStatus() { return s_status; }

String ruleEngineGetStatusJson() {
  String json = "{";
  json += "\"enabled\":";
  json += s_enabled ? "true" : "false";
  json += ",\"defaultOffControlled\":";
  json += s_defaultOffControlled ? "true" : "false";

  json += ",\"ruleCount\":";
  json += String(s_ruleCount);
  json += ",\"activeRuleId\":";
  json += String(s_status.activeRuleId);
  json += ",\"lastEvalMs\":";
  json += String(s_status.lastEvalMs);
  json += ",\"lastEvalDurationUs\":";
  json += String(s_status.lastEvalDurationUs);

  json += ",\"relayOwners\":[";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (i) json += ",";
    json += String(s_status.relayOwner[i]);
  }
  json += "]";

  json += ",\"desiredRelays\":[";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (i) json += ",";
    json += (s_status.hasDecision[i] ? (s_status.desiredRelay[i] ? "1" : "0") : "-1");
  }
  json += "]";

  json += "}";
  return json;
}

String ruleEngineExportJson() {
  return s_rulesJson;
}

bool ruleEngineLoadFromJson(const String& json, String* errOut) {
  DynamicJsonDocument doc(JSON_DOC_CAP);

  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    if (errOut) *errOut = String("JSON parse error: ") + err.c_str();
    return false;
  }

  JsonArray rulesArr;
  bool enabled = false;
  bool defaultOff = true; // výchozí: vypínat řízená relé, když nic neplatí

  if (doc.is<JsonArray>()) {
    rulesArr = doc.as<JsonArray>();
    enabled = true;
    // legacy formát nemá volbu => necháme default
  } else {
    enabled = doc["enabled"] | false;
    defaultOff =
      doc["defaultOffControlled"] |
      doc["default_off_controlled"] |
      true;
    rulesArr = doc["rules"].as<JsonArray>();
  }

  if (rulesArr.isNull()) {
    if (errOut) *errOut = "Missing rules array.";
    return false;
  }

  clearRules();
  s_enabled = enabled;
  s_defaultOffControlled = defaultOff;

  uint8_t idx = 0;
  for (JsonVariant v : rulesArr) {
    if (idx >= MAX_RULES) break;
    if (!v.is<JsonObject>()) continue;

    JsonObject o = v.as<JsonObject>();
    RuleItem& r = s_rules[idx];

    r.id = o["id"] | (uint32_t)(idx + 1);
    s_orderIds[idx] = r.id;
    r.enabled = o["enabled"] | true;
    r.priority = (int16_t)(o["priority"] | 50);
    r.stopOnMatch = o["stopOnMatch"] | false;

    r.debounceMs = (uint16_t)(o["debounceMs"] | 0);
    r.minOnMs    = (uint16_t)(o["minOnMs"] | 0);
    r.minOffMs   = (uint16_t)(o["minOffMs"] | 0);

    safeCopy(r.name, sizeof(r.name), o["name"] | "");
    safeCopy(r.desc, sizeof(r.desc), o["desc"] | "");

    // WHEN
    JsonObject when = o["when"].as<JsonObject>();
    r.when.op = parseOp(when["op"] | "AND");
    r.when.count = 0;

    JsonArray items = when["items"].as<JsonArray>();
    if (!items.isNull()) {
      for (JsonVariant ci : items) {
        if (r.when.count >= 8) break;
        if (!ci.is<JsonObject>()) continue;

        JsonObject co = ci.as<JsonObject>();
        Condition& c = r.when.items[r.when.count];

        const char* type = co["type"] | "input";
        c.type = parseCondType(type);

        if (c.type == CondType::INPUT_PIN) {
          c.input = (uint8_t)(co["input"] | 1);
          c.state = parseInputWanted(co["state"] | "ACTIVE");
        } else if (c.type == CondType::TIME) {
          safeCopy(c.timeFrom, sizeof(c.timeFrom), co["from"] | "00:00");
          safeCopy(c.timeTo, sizeof(c.timeTo), co["to"] | "00:00");
        } else if (c.type == CondType::MQTT) {
          safeCopy(c.mqttTopic, sizeof(c.mqttTopic), co["topic"] | "");
          safeCopy(c.mqttValue, sizeof(c.mqttValue), co["value"] | "");
        }

        r.when.count++;
      }
    }

    // THEN
    r.actionCount = 0;
    JsonArray th = o["then"].as<JsonArray>();
    if (!th.isNull()) {
      for (JsonVariant ai : th) {
        if (r.actionCount >= MAX_ACTIONS) break;
        if (!ai.is<JsonObject>()) continue;

        JsonObject ao = ai.as<JsonObject>();
        Action& a = r.actions[r.actionCount];

        a.type = parseActionType(ao["type"] | "relay_set");

        if (a.type == ActionType::RELAY_SET) {
          a.relay = (uint8_t)(ao["relay"] | 1);
          a.value = ao["value"] | false;
        } else if (a.type == ActionType::RELAY_PULSE) {
          a.relay = (uint8_t)(ao["relay"] | 1);
          a.pulseMs = (uint16_t)(ao["ms"] | 500);
        } else if (a.type == ActionType::MQTT_PUBLISH) {
          safeCopy(a.mqttTopic, sizeof(a.mqttTopic), ao["topic"] | "");
          safeCopy(a.mqttPayload, sizeof(a.mqttPayload), ao["payload"] | "");
        }

        r.actionCount++;
      }
    }

    // „Která relé řídí pravidla“ + max minOn/minOff (pro fallback OFF)
    for (uint8_t ai = 0; ai < r.actionCount; ai++) {
      const Action& a = r.actions[ai];
      if (a.type == ActionType::RELAY_SET && a.relay >= 1 && a.relay <= RELAY_COUNT) {
        const uint8_t ridx = (uint8_t)(a.relay - 1);
        s_relayControlled[ridx] = true;
        if (r.minOnMs > s_relayMaxMinOn[ridx]) s_relayMaxMinOn[ridx] = r.minOnMs;
        if (r.minOffMs > s_relayMaxMinOff[ridx]) s_relayMaxMinOff[ridx] = r.minOffMs;
      }
    }

    r.rawMatch = false;
    r.stableMatch = false;
    r.lastRawChangeMs = millis();

    idx++;
  }

  s_ruleCount = idx;
  s_orderCount = s_ruleCount;
  sortRulesByPriorityDesc();

  rebuildExportJson();

  resetStatus();
  return true;
}

void ruleEngineUpdate() {
  const uint32_t now = millis();
  if ((uint32_t)(now - s_lastTickMs) < s_tickMs) return;
  s_lastTickMs = now;

  if (!s_enabled) {
    resetStatus();
    return;
  }

  // V1: běžet jen v AUTO
  if (logicGetControlMode() != ControlMode::AUTO) {
    resetStatus();
    s_status.enabled = s_enabled;
    s_status.ruleCount = s_ruleCount;
    s_status.lastEvalMs = now;
    return;
  }

  const uint32_t t0 = micros();

  resetStatus();
  s_status.enabled = s_enabled;
  s_status.ruleCount = s_ruleCount;
  s_status.lastEvalMs = now;
  s_status.activeRuleId = 0;

  RelayDecision decisions[RELAY_COUNT];
  for (uint8_t i = 0; i < RELAY_COUNT; i++) decisions[i] = RelayDecision{};

  for (uint8_t i = 0; i < s_ruleCount; i++) {
    RuleItem& r = s_rules[i];
    if (!r.enabled) continue;

    const bool raw = condEvalGroup(r.when);
    updateRuleDebounce(r, raw, now);

    if (!r.stableMatch) continue;

    if (s_status.activeRuleId == 0) s_status.activeRuleId = r.id;

    actionsApply(r.actions, r.actionCount, decisions, r.id, r.minOnMs, r.minOffMs);

    if (r.stopOnMatch) break;
  }

// DŮLEŽITÉ: když nic neplatí, relé by neměla zůstat „viset“ v ON.
  // Fallback OFF aplikujeme jen na relé, která jsou řízená pravidly (relay_set).
  if (s_defaultOffControlled) {
    for (uint8_t rr = 0; rr < RELAY_COUNT; rr++) {
      if (!s_relayControlled[rr]) continue;
      if (decisions[rr].has) continue;
      decisions[rr].has = true;
      decisions[rr].value = false;
      decisions[rr].ownerRuleId = 0;
      decisions[rr].minOnMs = s_relayMaxMinOn[rr];
      decisions[rr].minOffMs = s_relayMaxMinOff[rr];
    }
  }

  applyDecisions(decisions, now);

  s_status.lastEvalDurationUs = (uint32_t)(micros() - t0);
}

#else

// RuleEngine disabled at build time (FEATURE_RULE_ENGINE=0)
#include <Arduino.h>

void ruleEngineInit() {}
void ruleEngineSetTickMs(uint16_t) {}
uint16_t ruleEngineGetTickMs() { return 100; }
void ruleEngineSetEnabled(bool) {}
bool ruleEngineIsEnabled() { return false; }
RuleEngineStatus ruleEngineGetStatus() { return RuleEngineStatus{}; }
String ruleEngineGetStatusJson() { return String("{\"enabled\":false,\"disabledByBuild\":true}"); }
String ruleEngineExportJson() { return String("{\"enabled\":false,\"rules\":[]}"); }
bool ruleEngineLoadFromJson(const String&, String* errOut) { if (errOut) *errOut = "RuleEngine disabled by build"; return false; }
void ruleEngineUpdate() {}

#endif