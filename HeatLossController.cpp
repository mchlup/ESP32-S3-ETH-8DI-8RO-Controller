#include "HeatLossController.h"
#include "FsController.h"
#include "Log.h"
#include "LogicController.h"
#include "OpenThermController.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>

namespace {
  HeatLossConfig g_cfg;
  HeatLossStatus g_st;
  bool g_inited = false;

  struct Sample {
    uint32_t ts;
    float ti;
    float te;
    float pkw;
  };

  // Keep a bounded ring buffer so we can compute rolling UA without reading files.
  Sample g_buf[720]; // up to 12h @ 60s
  uint16_t g_head = 0;
  uint16_t g_count = 0;

  static float parseTempSource(const String& src, bool* ok, String* why) {
    if (ok) *ok = false;
    String s = src; s.trim(); s.toLowerCase();

    if (s.startsWith("temp")) {
      int idx = s.substring(4).toInt(); // 1..8
      if (idx >= 1 && idx <= 8) {
        const uint8_t i = (uint8_t)(idx - 1);
        if (logicIsTempValid(i)) {
          if (ok) *ok = true;
          return logicGetTempC(i);
        }
        if (why) *why = "temp invalid";
        return NAN;
      }
      if (why) *why = "temp index";
      return NAN;
    }

    if (s == "opentherm.room") {
      OpenThermStatus ot = openthermGetStatus();
      if (isfinite(ot.roomTempC)) { if (ok) *ok = true; return ot.roomTempC; }
      if (why) *why = "ot room NAN";
      return NAN;
    }
    if (s == "opentherm.outdoor") {
      OpenThermStatus ot = openthermGetStatus();
      if (isfinite(ot.outdoorTempC)) { if (ok) *ok = true; return ot.outdoorTempC; }
      if (why) *why = "ot outdoor NAN";
      return NAN;
    }
    if (s == "equitherm.outdoor") {
      EquithermStatus es = logicGetEquithermStatus();
      if (es.outdoorValid && isfinite(es.outdoorC)) { if (ok) *ok = true; return es.outdoorC; }
      if (why) *why = "equitherm outdoor invalid";
      return NAN;
    }

    if (why) *why = "unknown source";
    return NAN;
  }

  static float getPowerKw(bool* ok, String* why) {
    if (ok) *ok = false;
    OpenThermStatus ot = openthermGetStatus();
    if (isfinite(ot.powerKw)) { if (ok) *ok = true; return ot.powerKw; }

    // Fallback: modulation% * assumedMaxBoilerKw
    if (isfinite(ot.modulationPct)) {
      if (ok) *ok = true;
      return (ot.modulationPct / 100.0f) * g_cfg.assumedMaxBoilerKw;
    }

    if (why) *why = "no power";
    return NAN;
  }

  static void pushSample(const Sample& s) {
    g_buf[g_head] = s;
    g_head = (uint16_t)((g_head + 1) % (sizeof(g_buf)/sizeof(g_buf[0])));
    if (g_count < (uint16_t)(sizeof(g_buf)/sizeof(g_buf[0]))) g_count++;
  }

  static void computeUA() {
    if (g_count < 3) { g_st.ua_W_per_K = NAN; return; }

    const uint32_t now = millis();
    const uint32_t maxAgeMs = g_cfg.windowSec * 1000UL;

    double sumP = 0.0;
    double sumDT = 0.0;
    uint32_t used = 0;

    // Iterate over ring buffer from newest backwards
    for (uint16_t k = 0; k < g_count; k++) {
      int idx = (int)g_head - 1 - k;
      if (idx < 0) idx += (int)(sizeof(g_buf)/sizeof(g_buf[0]));
      const Sample& sm = g_buf[idx];
      if ((uint32_t)(now - sm.ts) > maxAgeMs) break;

      if (!isfinite(sm.ti) || !isfinite(sm.te) || !isfinite(sm.pkw)) continue;
      const double dT = (double)sm.ti - (double)sm.te;
      if (fabs(dT) < 0.5) continue;

      sumP += (double)sm.pkw * 1000.0; // W
      sumDT += dT;
      used++;
    }

    if (used < 3) { g_st.ua_W_per_K = NAN; return; }

    const double pAvgW = sumP / (double)used;
    const double dTAvg = sumDT / (double)used;

    g_st.ua_W_per_K = (float)(pAvgW / dTAvg);

    const double designDT = (double)g_cfg.indoorTargetC - (double)g_cfg.designOutdoorC;
    if (designDT > 0.5 && isfinite(g_st.ua_W_per_K)) {
      g_st.projectedLossKw = (float)((g_st.ua_W_per_K * designDT) / 1000.0);
    } else {
      g_st.projectedLossKw = NAN;
    }
  }

  static void ensureLogHeader(File& f) {
    if (f.size() > 0) return;
    f.print("ts_ms,indoor_c,outdoor_c,power_kw,ua_w_per_k,projected_kw\n");
  }

  static bool appendLog(float ti, float te, float pkw) {
    if (!LittleFS.begin(true)) return false;

    const char* path = "/heatloss.csv";
    File f = LittleFS.open(path, FILE_APPEND);
    if (!f) return false;

    ensureLogHeader(f);

    const uint32_t ts = millis();
    f.printf("%lu,%.2f,%.2f,%.3f,%.2f,%.3f\n",
             (unsigned long)ts,
             (double)ti, (double)te, (double)pkw,
             (double)g_st.ua_W_per_K, (double)g_st.projectedLossKw);
    f.close();
    return true;
  }
}

void heatlossInit() {
  if (g_inited) return;
  g_inited = true;
  g_st = HeatLossStatus();
  g_st.enabled = g_cfg.enabled;
}

void heatlossApplyConfig(const String& json) {
  DynamicJsonDocument doc(2048);
  DeserializationError e = deserializeJson(doc, json);
  if (e) return;

  JsonObject root = doc.as<JsonObject>();
  JsonObject hl = root["heatloss"];
  if (hl.isNull()) return;

  g_cfg.enabled = (bool)(hl["enabled"] | g_cfg.enabled);
  g_cfg.logIntervalMs = (uint32_t)(hl["logIntervalMs"] | hl["log_interval_ms"] | g_cfg.logIntervalMs);
  if (g_cfg.logIntervalMs < 5000) g_cfg.logIntervalMs = 5000;
  if (g_cfg.logIntervalMs > 10UL * 60UL * 1000UL) g_cfg.logIntervalMs = 10UL * 60UL * 1000UL;

  g_cfg.windowSec = (uint32_t)(hl["windowSec"] | hl["window_sec"] | g_cfg.windowSec);
  if (g_cfg.windowSec < 300) g_cfg.windowSec = 300;
  if (g_cfg.windowSec > 48UL * 3600UL) g_cfg.windowSec = 48UL * 3600UL;

  g_cfg.designOutdoorC = (float)(hl["designOutdoorC"] | hl["design_outdoor_c"] | g_cfg.designOutdoorC);
  g_cfg.indoorTargetC  = (float)(hl["indoorTargetC"] | hl["indoor_target_c"] | g_cfg.indoorTargetC);

  const char* inSrc = hl["indoorSource"] | hl["indoor_source"];
  if (inSrc && String(inSrc).length()) g_cfg.indoorSource = String(inSrc);

  const char* outSrc = hl["outdoorSource"] | hl["outdoor_source"];
  if (outSrc && String(outSrc).length()) g_cfg.outdoorSource = String(outSrc);

  g_cfg.assumedMaxBoilerKw = (float)(hl["assumedMaxBoilerKw"] | hl["assumed_max_boiler_kw"] | g_cfg.assumedMaxBoilerKw);
  if (g_cfg.assumedMaxBoilerKw < 1.0f) g_cfg.assumedMaxBoilerKw = 1.0f;
  if (g_cfg.assumedMaxBoilerKw > 100.0f) g_cfg.assumedMaxBoilerKw = 100.0f;

  g_st.enabled = g_cfg.enabled;
}

HeatLossConfig heatlossGetConfig() { return g_cfg; }
HeatLossStatus heatlossGetStatus() { return g_st; }

String heatlossGetLogPath() { return "/heatloss.csv"; }

bool heatlossClearLog() {
  if (!LittleFS.begin(true)) return false;
  if (LittleFS.exists("/heatloss.csv")) return LittleFS.remove("/heatloss.csv");
  return true;
}

void heatlossLoop() {
  if (!g_cfg.enabled) return;

  const uint32_t now = millis();
  if (g_st.lastLogMs && (uint32_t)(now - g_st.lastLogMs) < g_cfg.logIntervalMs) return;

  bool okIn=false, okOut=false, okP=false;
  String whyIn, whyOut, whyP;

  const float ti = parseTempSource(g_cfg.indoorSource, &okIn, &whyIn);
  const float te = parseTempSource(g_cfg.outdoorSource, &okOut, &whyOut);
  const float pkw = getPowerKw(&okP, &whyP);

  g_st.indoorC = ti;
  g_st.outdoorC = te;
  g_st.powerKw = pkw;

  if (!(okIn && okOut && okP)) {
    g_st.haveSample = false;
    g_st.reason = String("missing: ") + (okIn ? "" : "indoor ") + (okOut ? "" : "outdoor ") + (okP ? "" : "power ");
    return;
  }

  const float dT = ti - te;
  if (!isfinite(dT) || fabs(dT) < 0.5f) {
    g_st.haveSample = false;
    g_st.reason = "deltaT too small";
    return;
  }

  g_st.haveSample = true;
  g_st.reason = "";

  Sample sm; sm.ts = now; sm.ti = ti; sm.te = te; sm.pkw = pkw;
  pushSample(sm);
  g_st.samples++;

  computeUA();
  appendLog(ti, te, pkw);

  g_st.lastLogMs = now;
}
