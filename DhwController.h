#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

struct DhwInterval {
  uint16_t startMin = 0;
  uint16_t endMin = 0;
  bool valid = false;
};

static constexpr uint8_t DHW_MAX_INTERVALS_PER_DAY = 8;

struct DhwDaySchedule {
  DhwInterval items[DHW_MAX_INTERVALS_PER_DAY];
  uint8_t count = 0;
};

struct DhwConfig {
  bool enabled = true;
  bool disableEquithermDuringHeat = true;
  uint32_t tempMaxAgeMs = 900000;

  struct {
    bool useInput = true;
    bool useSchedule = true;
    bool scheduleEnabled = true;
    float targetTempC = 50.0f;
    float hysteresisC = 2.0f;
    String requestMode = "relay"; // relay | opentherm
    bool otEnableDhw = true;
    float otDhwSetpointC = 50.0f;
    bool relayRequest = true;
    bool driveValveRelay = true;
    uint8_t valveRelayIndex = 2;   // R3, zero-based
    uint8_t boilerRelayIndex = 4;  // R5, zero-based
    uint32_t valveLeadMs = 3000;
    uint32_t valveSwitchBackMs = 1500;
    uint32_t boilerOffHoldMs = 2000;
    DhwDaySchedule week[7];
  } heat;

  struct {
    bool useInput = true;
    bool useSchedule = true;
    bool scheduleEnabled = true;
    bool pulseEnabled = true;
    uint16_t pulseOnMin = 5;
    uint16_t pulseOffMin = 15;
    uint8_t relayIndex = 3; // R4, zero-based
    DhwDaySchedule week[7];
  } circ;

  struct {
    bool enabled = false;
    uint8_t weekday = 0; // 0=Mon..6=Sun
    uint16_t startMin = 120;
    float targetTempC = 60.0f;
    uint16_t holdMin = 30;
  } antiLegionella;
};

struct DhwStatus {
  bool enabled = false;
  bool timeValid = false;
  String timeIso;

  bool heatInputActive = false;
  bool heatScheduleActive = false;
  bool boilerDhwMode = false; // true when OT Status(ID0) reports DHW active
  bool heatRequested = false;
  bool heatActive = false;
  bool heatSatisfied = false;
  String heatReason;
  String heatPhase;
  bool heatSequenceActive = false;
  String requestMode;

  bool circInputActive = false;
  bool circScheduleActive = false;
  bool circRequested = false;
  bool circPulseOn = false;
  bool circActive = false;
  String circReason;

  float tankTempC = NAN;
  uint32_t tankTempAgeMs = 0;
  float targetTempC = NAN;
  float otTargetTempC = NAN;

  bool valveRelayOn = false;
  bool boilerRelayOn = false;
  bool circRelayOn = false;
  bool otDhwEnable = false;
  bool antiLegionellaActive = false;
  bool antiLegionellaDone = false;

  uint32_t lastEvalMs = 0;
};

void dhwInit();
void dhwLoop();
void dhwReloadFromStore();

DhwConfig dhwGetConfig();
DhwStatus dhwGetStatus();
String dhwGetStatusJson();
void dhwFillFastJson(JsonObject& out);

void dhwApplyConfig(const String& json);
bool dhwHandleCmdJson(const String& json, String& outErr);

bool dhwIsHeatActive();
bool dhwIsPriorityActive();
