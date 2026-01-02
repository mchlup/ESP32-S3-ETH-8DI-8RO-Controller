#include "NtcController.h"
#include <math.h>

// This module provides both the new class API (NtcController::...) and
// the legacy functions used across the current project (ntcIsValid, ntcGetTempC, ntcApplyConfig, ...).

namespace {

constexpr uint8_t INPUT_COUNT = 8;

struct NtcInputCfg {
  bool     enabled = false;
  uint8_t  gpio    = 255;      // ADC GPIO
  float    beta    = 3950.0f;
  float    rSeries = 10000.0f; // series resistor
  float    r0      = 10000.0f; // nominal NTC resistance at T0
  float    t0C     = 25.0f;    // nominal temperature (°C)
  float    offsetC = 0.0f;     // calibration offset (°C)
};

struct NtcRuntime {
  uint32_t lastSampleMs = 0;
  int      raw          = 0;
  float    tempC        = NAN;
  bool     valid        = false;
};

NtcInputCfg s_cfg[INPUT_COUNT];
NtcRuntime  s_rt[INPUT_COUNT];

inline bool gpioSupportsNtc(uint8_t gpio) {
  // User requirement: NTC only on GPIO1..GPIO3
  return gpio >= 1 && gpio <= 3;
}

float computeTempC(int raw, const NtcInputCfg& c) {
  if (raw <= 0 || raw >= 4095) return NAN;

  const float v = (float)raw / 4095.0f;
  if (v <= 0.0f || v >= 1.0f) return NAN;

  // Voltage divider: NTC to GND, Rseries to Vcc (typical). Resistance:
  // Rntc = Rseries * (Vout / (Vcc - Vout)) = Rseries * v / (1 - v)
  const float rNtc = c.rSeries * (v / (1.0f - v));
  if (!(rNtc > 0.0f) || !isfinite(rNtc)) return NAN;

  const float T0 = (c.t0C + 273.15f);
  const float invT = (1.0f / T0) + (1.0f / c.beta) * logf(rNtc / c.r0);
  if (!isfinite(invT) || invT <= 0.0f) return NAN;

  const float tempK = 1.0f / invT;
  return (tempK - 273.15f) + c.offsetC;
}

void sampleOne(uint8_t i, uint32_t nowMs) {
  if (i >= INPUT_COUNT) return;

  if (!s_cfg[i].enabled || !gpioSupportsNtc(s_cfg[i].gpio)) {
    s_rt[i].valid = false;
    s_rt[i].tempC = NAN;
    s_rt[i].raw = 0;
    return;
  }

  if (nowMs - s_rt[i].lastSampleMs < 1000) return;
  s_rt[i].lastSampleMs = nowMs;

  const int raw = analogRead(s_cfg[i].gpio);
  s_rt[i].raw = raw;

  const float t = computeTempC(raw, s_cfg[i]);
  s_rt[i].tempC = t;
  s_rt[i].valid = isfinite(t);
}

} // namespace

// ---------- Class API ----------

void NtcController::begin() {
  for (uint8_t i = 0; i < INPUT_COUNT; i++) {
    s_cfg[i] = NtcInputCfg{};
    s_rt[i]  = NtcRuntime{};
  }
}

void NtcController::configureGpio(uint8_t gpio, bool enable) {
  // This method is GPIO-based; in the current project we primarily configure per "input index".
  // Keep it as a convenience: map gpio -> same index (only if 0..7).
  if (gpio >= INPUT_COUNT) return;
  s_cfg[gpio].enabled = enable;
  s_cfg[gpio].gpio = gpio;
}

void NtcController::loop() {
  const uint32_t nowMs = millis();
  for (uint8_t i = 0; i < INPUT_COUNT; i++) sampleOne(i, nowMs);
}

NtcReading NtcController::get(uint8_t gpio) {
  NtcReading r;
  r.gpio = gpio;
  r.temperature = NAN;
  r.status = NTC_DISABLED;

  if (gpio >= INPUT_COUNT) return r;
  if (!s_cfg[gpio].enabled || !gpioSupportsNtc(s_cfg[gpio].gpio)) return r;

  // Ensure fresh-ish sample
  sampleOne(gpio, millis());

  if (s_rt[gpio].valid) {
    r.temperature = s_rt[gpio].tempC;
    r.status = NTC_OK;
  } else {
    r.status = NTC_ERROR;
  }
  return r;
}

// ---------- Legacy API used in LogicController/WebServerController ----------

void ntcInit() {
  NtcController::begin();
}

void ntcLoop() {
  NtcController::loop();
}

bool ntcIsEnabled(uint8_t inputIndex) {
  if (inputIndex >= INPUT_COUNT) return false;
  return s_cfg[inputIndex].enabled;
}

uint8_t ntcGetGpio(uint8_t inputIndex) {
  if (inputIndex >= INPUT_COUNT) return 255;
  return s_cfg[inputIndex].gpio;
}

int ntcGetRaw(uint8_t inputIndex) {
  if (inputIndex >= INPUT_COUNT) return 0;
  return s_rt[inputIndex].raw;
}

bool ntcIsValid(uint8_t inputIndex) {
  if (inputIndex >= INPUT_COUNT) return false;
  // Ensure sampling isn't stale
  sampleOne(inputIndex, millis());
  return s_rt[inputIndex].valid;
}

float ntcGetTempC(uint8_t inputIndex) {
  if (inputIndex >= INPUT_COUNT) return NAN;
  sampleOne(inputIndex, millis());
  return s_rt[inputIndex].tempC;
}

static void applyCfgObj(const JsonObjectConst& root) {
  // Expected config path: cfg.iofunc.inputs[i] with role "temp_ntc10k" and params {gpio,beta,rSeries,r0,t0,offset}
  JsonObjectConst cfg = root;
  if (root.containsKey("cfg") && root["cfg"].is<JsonObjectConst>()) cfg = root["cfg"].as<JsonObjectConst>();

  if (!cfg.containsKey("iofunc") || !cfg["iofunc"].is<JsonObjectConst>()) return;
  JsonObjectConst iof = cfg["iofunc"].as<JsonObjectConst>();
  if (!iof.containsKey("inputs") || !iof["inputs"].is<JsonArrayConst>()) return;
  JsonArrayConst inputs = iof["inputs"].as<JsonArrayConst>();

  uint8_t idx = 0;
  for (JsonVariantConst v : inputs) {
    if (idx >= INPUT_COUNT) break;
    if (!v.is<JsonObjectConst>()) { idx++; continue; }
    JsonObjectConst o = v.as<JsonObjectConst>();
    const char* role = o["role"] | "none";

    if (strcmp(role, "temp_ntc10k") == 0) {
      JsonObjectConst p = o["params"].is<JsonObjectConst>() ? o["params"].as<JsonObjectConst>() : JsonObjectConst();
      s_cfg[idx].enabled = true;
      s_cfg[idx].gpio    = (uint8_t)(p["gpio"] | 0);
      s_cfg[idx].beta    = (float)(p["beta"] | 3950);
      s_cfg[idx].rSeries = (float)(p["rSeries"] | 10000);
      s_cfg[idx].r0      = (float)(p["r0"] | 10000);
      s_cfg[idx].t0C     = (float)(p["t0"] | 25);
      s_cfg[idx].offsetC = (float)(p["offset"] | 0);
      if (!gpioSupportsNtc(s_cfg[idx].gpio)) {
        // Enforce user constraint: disable invalid GPIO (notably GPIO0)
        s_cfg[idx].enabled = false;
      }
    } else {
      // Not NTC -> disable NTC for this logical temp slot
      s_cfg[idx].enabled = false;
    }

    idx++;
  }
}

void ntcApplyConfig(const JsonObjectConst& json) {
  applyCfgObj(json);
}

void ntcApplyConfig(const String& jsonStr) {
  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) return;
  JsonObjectConst root = doc.as<JsonObjectConst>();
  applyCfgObj(root);
}
