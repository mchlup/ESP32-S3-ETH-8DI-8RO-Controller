#include "HistoryBuffer.h"
#include <math.h>
#include "TemperatureManager.h"
#include "OpenThermController.h"
#include "EquithermController.h"
#include "DhwController.h"

namespace {
  struct Sample {
    uint32_t ms = 0;
    float outsideC = NAN;
    float flowC = NAN;
    float dhwC = NAN;
    float returnC = NAN;
    float tankTopC = NAN;
    float tankMidC = NAN;
    float tankBottomC = NAN;
    float pressureBar = NAN;
    float mixPct = NAN;
    bool dhwHeat = false;
  };
  constexpr size_t kCap = 240;
  constexpr uint32_t kPeriodMs = 60000UL;
  Sample g_samples[kCap];
  size_t g_head = 0;
  size_t g_count = 0;
  uint32_t g_lastSampleMs = 0;
  inline float tv(TempRole r){ TempValue v = TemperatureManager::get(r, 1800000UL); return (v.valid && isfinite(v.c)) ? v.c : NAN; }
  void pushSample() {
    Sample s{};
    s.ms = millis();
    s.outsideC = tv(TempRole::Outside);
    s.flowC = tv(TempRole::Flow);
    s.dhwC = tv(TempRole::DhwTank);
    s.returnC = tv(TempRole::Return);
    s.tankTopC = tv(TempRole::TankTop);
    s.tankMidC = tv(TempRole::TankMid);
    s.tankBottomC = tv(TempRole::TankBottom);
    OpenThermStatusSnapshot ot = openthermGetStatus();
    s.pressureBar = (ot.present && ot.ready && isfinite(ot.pressureBar)) ? ot.pressureBar : NAN;
    EquithermStatus eq = equithermGetStatus();
    s.mixPct = eq.mixPositionPct;
    s.dhwHeat = dhwIsHeatActive();
    g_samples[g_head] = s;
    g_head = (g_head + 1) % kCap;
    if (g_count < kCap) ++g_count;
  }
}
namespace HistoryBuffer {
  void begin() { clear(); }
  void clear() { for(size_t i=0;i<kCap;i++) g_samples[i]=Sample{}; g_head=0; g_count=0; g_lastSampleMs=0; }
  void loop() {
    uint32_t now = millis();
    if (g_lastSampleMs && (uint32_t)(now - g_lastSampleMs) < kPeriodMs) return;
    g_lastSampleMs = now;
    pushSample();
  }
  void fillJson(JsonArray out, size_t maxItems) {
    const size_t count = (g_count < maxItems) ? g_count : maxItems;
    const size_t start = (g_head + kCap - count) % kCap;
    for(size_t i=0;i<count;i++){
      const Sample &s = g_samples[(start+i)%kCap];
      if(!s.ms) continue;
      JsonObject o = out.createNestedObject();
      o["ms"] = s.ms;
      if (isfinite(s.outsideC)) o["outsideC"] = s.outsideC; else o["outsideC"] = nullptr;
      if (isfinite(s.flowC)) o["flowC"] = s.flowC; else o["flowC"] = nullptr;
      if (isfinite(s.dhwC)) o["dhwC"] = s.dhwC; else o["dhwC"] = nullptr;
      if (isfinite(s.returnC)) o["returnC"] = s.returnC; else o["returnC"] = nullptr;
      if (isfinite(s.tankTopC)) o["tankTopC"] = s.tankTopC; else o["tankTopC"] = nullptr;
      if (isfinite(s.tankMidC)) o["tankMidC"] = s.tankMidC; else o["tankMidC"] = nullptr;
      if (isfinite(s.tankBottomC)) o["tankBottomC"] = s.tankBottomC; else o["tankBottomC"] = nullptr;
      if (isfinite(s.pressureBar)) o["pressureBar"] = s.pressureBar; else o["pressureBar"] = nullptr;
      if (isfinite(s.mixPct)) o["mixPct"] = s.mixPct; else o["mixPct"] = nullptr;
      o["dhwHeat"] = s.dhwHeat;
    }
  }
  String toJson(size_t maxItems) {
    DynamicJsonDocument doc(24576);
    doc["ok"] = true;
    JsonArray arr = doc.createNestedArray("items");
    fillJson(arr, maxItems);
    String out; serializeJson(doc,out); return out;
  }
}
