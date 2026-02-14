#pragma once
#include <Arduino.h>

struct HeatLossConfig {
  bool enabled = true;
  uint32_t logIntervalMs = 60000;   // 60s
  uint32_t windowSec = 3600;        // 1h rolling window for UA
  float designOutdoorC = -15.0f;    // for projected heat loss
  float indoorTargetC  = 22.0f;
  // Source selection:
  // - indoor: "opentherm.room" or "temp1".."temp8"
  // - outdoor: "equitherm.outdoor" or "opentherm.outdoor" or "temp1".."temp8"
  String indoorSource = "opentherm.room";
  String outdoorSource = "equitherm.outdoor";

  // Boiler power estimation:
  // - prefer OpenTherm powerKw if available (currently estimated from modulation)
  // - otherwise assume modulationPct * assumedMaxBoilerKw
  float assumedMaxBoilerKw = 9.0f;
};

struct HeatLossStatus {
  bool enabled = false;
  bool haveSample = false;

  float indoorC = NAN;
  float outdoorC = NAN;
  float powerKw = NAN;

  // UA (W/K) ~= P(W) / (Ti-Te)
  float ua_W_per_K = NAN;

  // Projected heat loss at designOutdoorC (kW)
  float projectedLossKw = NAN;

  uint32_t samples = 0;
  uint32_t lastLogMs = 0;
  String reason = "";
};

void heatlossInit();
void heatlossLoop();
HeatLossConfig heatlossGetConfig();
HeatLossStatus heatlossGetStatus();
void heatlossApplyConfig(const String& json);
bool heatlossClearLog();
String heatlossGetLogPath();
