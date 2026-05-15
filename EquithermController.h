#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Ekviterm (equitherm) controller.
//
// - Uses OpenTherm Outside temperature (ID27) and Boiler flow temperature (ID25).
// - Computes desired heating water temperature from outside temperature
//   with separate day/night curves and optional weekly schedule.
// - Can drive a mixing valve (fixed R1=heat/open, R2=cool/close) to reach the desired flow temperature.
// - Can optionally "assist" the boiler by requesting higher CH setpoint than the target
//   (useful when mixing valve needs hot water headroom).

static constexpr uint8_t HEATING_MAX_INTERVALS_PER_DAY = 6;

struct EquithermInterval {
  uint16_t startMin = 360;
  uint16_t endMin = 1320;
};

struct EquithermCurve {
  float outColdC  = -12.0f;
  float flowColdC = 55.0f;
  float outWarmC  = 20.0f;
  float flowWarmC = 25.0f;
};

struct EquithermConfig {
  bool enabled = false;

  // Requested mode: "auto" / "day" / "night"
  String mode = "auto";

  // If true, IN1 ACTIVE forces night mode.
  bool useIn1NightOverride = true;

  // Summer mode: disable heating when outside is warm enough.
  bool summerModeEnabled = false;
  float summerOffAboveC = 18.0f;
  float summerOnBelowC = 16.0f;

  // Weekly schedule (used only when mode=="auto" and scheduleEnabled==true)
  bool scheduleEnabled = false;
  uint8_t intervalCount[7] = {1,1,1,1,1,1,1};
  EquithermInterval intervals[7][HEATING_MAX_INTERVALS_PER_DAY] = {};

  EquithermCurve day;
  EquithermCurve night;

  // Limits (global)
  float minFlowC = 22.0f;
  float maxFlowC = 60.0f;

  // Clamp sent CH setpoint again (safety)
  float minChSetpointC = 22.0f;
  float maxChSetpointC = 60.0f;

  // Temperature age requirement for OT temps
  uint32_t tempMaxAgeMs = 600000;

  // Send behavior (OpenTherm ID1 write)
  uint32_t minSendIntervalMs = 60000;
  float minSendDeltaC = 0.5f;

  // Output mapping
  bool useOpenTherm = true;

  // Optional: apply boiler Max CH setpoint (ID57) so boiler clamp matches our max.
  bool applyBoilerMaxCh = false;
  float boilerMaxChC = 60.0f;

  // Optional: drive a relay for boiler day/night input (default R6).
  bool driveNightRelay = true;
  uint8_t nightRelayIndex = 5; // 0..7 = R1..R8
  bool nightRelayOnWhenNight = true;

  // Mixing valve relay mapping is fixed by project wiring: R1 = heat/open, R2 = cool/close.
  bool mixingEnabled = false;
  uint8_t mixOpenRelayIndex = 0;  // 0..7 = R1..R8
  uint8_t mixCloseRelayIndex = 1; // 0..7 = R1..R8
  float mixDeadbandC = 0.5f;
  float mixTargetOffsetC = 0.0f;
  uint32_t mixPulseMs = 300;
  uint32_t mixMinIntervalMs = 30000;
  uint32_t mixTravelMs = 6000;
  uint32_t mixCalibrationSeatMs = 1500;
  uint32_t mixAutoRecalibrationMs = 21600000UL;

  // Boiler assist (setpoint headroom above targetFlowC).
  bool boilerAssistEnabled = false;
  float boilerAssistDeltaC = 5.0f;
  bool boilerAssistForceChEnable = true;
};

struct EquithermStatus {
  bool enabled = false;
  bool active = false;
  String reason;

  // Effective mode
  String modeReq;
  String modeEff;
  bool scheduleUsed = false;
  bool in1Active = false;
  bool in1ForcingNight = false;
  bool summerActive = false;

  // Temps (OpenTherm)
  float outsideC = NAN;
  uint32_t outsideAgeMs = 0;
  String outsideSrc;

  float flowC = NAN; // Boiler/flow temperature (OpenTherm priority)
  uint32_t flowAgeMs = 0;
  String flowSrc;

  // Mixing feedback temperature: preferred source for valve regulation.
  // Uses dedicated mixing feedback from TemperatureManager::getMixFeedback(),
  // which must prefer the DS18B20 sensor behind the mixer and must not be
  // overridden by OpenTherm boiler return temperature.
  float mixFeedbackC = NAN;
  uint32_t mixFeedbackAgeMs = 0;
  String mixFeedbackSrc;

  // Targets
  float targetFlowC = NAN;      // curve result (after limits + optional mix offset)
  float targetBaseFlowC = NAN;  // curve result before optional mix offset
  float boilerSetpointC = NAN;  // what we request from boiler (may include assist delta)

  // Output
  float lastSentChC = NAN;
  bool lastSendOk = false;
  String lastSendErr;
  uint32_t lastSendMs = 0;

  // Mixing
  String mixState;        // "idle" / "open" / "close" / "manual_open" / "manual_close"
  bool mixPulsing = false;
  bool mixManual = false;
  String mixManualDir;
  uint32_t mixLastActMs = 0;
  uint32_t mixPulseReqMs = 0;
  uint32_t mixPulseElapsedMs = 0;
  uint32_t mixPulseRemainingMs = 0;
  uint32_t mixLastPulseMs = 0;
  float mixPositionPct = NAN;
  bool mixPositionTrusted = false;
  uint32_t mixLastCalibrationMs = 0;
  String mixCalibrationState;

  // Boiler max CH
  float boilerMaxChC = NAN;
  float boilerMaxBoundMinC = NAN;
  float boilerMaxBoundMaxC = NAN;
  float boilerClampMinC = NAN;
  float boilerClampMaxC = NAN;

  // Time
  bool timeValid = false;
  String timeIso;
};

void equithermInit();
void equithermLoop();
void equithermReloadFromStore();

EquithermConfig equithermGetConfig();
EquithermStatus equithermGetStatus();
String equithermGetStatusJson();

// Apply runtime + persist (expects JSON object for "equitherm" settings).
void equithermApplyConfig(const String& json);

// Commands: {"mode":"day|night|auto"} or {"enabled":true/false}
bool equithermHandleCmdJson(const String& json, String& outErr);

// For /api/fast
void equithermFillFastJson(JsonObject& out);
void equithermSetExternalBlock(bool blocked);

// Lightweight service hook used from blocking OpenTherm wait loops.
void equithermBackgroundService();

// Force immediate recompute on external state changes such as IN1 day/night input.
void equithermRequestRecompute();
