#include "Features.h"
#include "DhwController.h"

#include <math.h>

#include "ConfigStore.h"
#include "InputController.h"
#include "NetworkController.h"
#include "OpenThermController.h"
#include "EventLog.h"
#include "RelayController.h"
#include "TemperatureManager.h"
#include "EquithermController.h"

namespace {
  DhwConfig s_cfg;
  DhwStatus s_st;
  bool s_inited = false;
  bool s_forceHeat = false;
  uint32_t s_legionellaHoldUntilMs = 0;
  int s_legionellaLastDayKey = -1;
  uint32_t s_forceHeatUntilMs = 0;
  bool s_forceCirc = false;
  bool s_circPulseAnchorValid = false;
  uint32_t s_circPulseAnchorMs = 0;
  int s_circPulseAnchorWeekday = -1;
  uint16_t s_circPulseAnchorStartMin = 0;

  static inline uint32_t clampSeqMs(uint32_t v, uint32_t def) {
    if (v > 60000) return 60000;
    return v;
  }

  enum class DhwHeatPhase : uint8_t {
    Idle = 0,
    SwitchingToDhw,
    Heating,
    SwitchingBack
  };

  DhwHeatPhase s_heatPhase = DhwHeatPhase::Idle;
  uint32_t s_heatPhaseUntilMs = 0;

  static inline RelayId rid(uint8_t idx) { return (RelayId)idx; }

  static const char* heatPhaseName(DhwHeatPhase p) {
    switch (p) {
      case DhwHeatPhase::SwitchingToDhw: return "switching_to_dhw";
      case DhwHeatPhase::Heating: return "heating";
      case DhwHeatPhase::SwitchingBack: return "switching_back_to_ch";
      default: return "idle";
    }
  }

  static uint8_t clampRelayIndex(int v, uint8_t def) {
    if (v < 0 || v > 7) return def;
    return (uint8_t)v;
  }

  static uint16_t clampMin(int v) {
    if (v < 0) return 0;
    if (v > 1439) return 1439;
    return (uint16_t)v;
  }

  static void clampConfig() {
    if (s_cfg.heat.requestMode != "relay" && s_cfg.heat.requestMode != "opentherm") s_cfg.heat.requestMode = "relay";
    if (s_cfg.tempMaxAgeMs < 10000) s_cfg.tempMaxAgeMs = 10000;
    if (s_cfg.tempMaxAgeMs > 3600000) s_cfg.tempMaxAgeMs = 3600000;
    if (!isfinite(s_cfg.heat.targetTempC)) s_cfg.heat.targetTempC = 50.0f;
    if (s_cfg.heat.targetTempC < 20.0f) s_cfg.heat.targetTempC = 20.0f;
    if (s_cfg.heat.targetTempC > 80.0f) s_cfg.heat.targetTempC = 80.0f;
    if (!isfinite(s_cfg.heat.hysteresisC)) s_cfg.heat.hysteresisC = 2.0f;
    if (s_cfg.heat.hysteresisC < 0.5f) s_cfg.heat.hysteresisC = 0.5f;
    if (s_cfg.heat.hysteresisC > 15.0f) s_cfg.heat.hysteresisC = 15.0f;
    if (!isfinite(s_cfg.heat.otDhwSetpointC)) s_cfg.heat.otDhwSetpointC = s_cfg.heat.targetTempC;
    if (s_cfg.heat.otDhwSetpointC < 20.0f) s_cfg.heat.otDhwSetpointC = 20.0f;
    if (s_cfg.heat.otDhwSetpointC > 80.0f) s_cfg.heat.otDhwSetpointC = 80.0f;
    if (s_cfg.circ.pulseOnMin > 1440) s_cfg.circ.pulseOnMin = 1440;
    if (s_cfg.circ.pulseOffMin > 1440) s_cfg.circ.pulseOffMin = 1440;
    s_cfg.heat.valveRelayIndex = clampRelayIndex((int)s_cfg.heat.valveRelayIndex, 2);
    s_cfg.heat.boilerRelayIndex = clampRelayIndex((int)s_cfg.heat.boilerRelayIndex, 4);
    s_cfg.heat.valveLeadMs = clampSeqMs(s_cfg.heat.valveLeadMs, 3000);
    s_cfg.heat.valveSwitchBackMs = clampSeqMs(s_cfg.heat.valveSwitchBackMs, 1500);
    s_cfg.heat.boilerOffHoldMs = clampSeqMs(s_cfg.heat.boilerOffHoldMs, 2000);
    s_cfg.circ.relayIndex = clampRelayIndex((int)s_cfg.circ.relayIndex, 3);
    for (int d = 0; d < 7; d++) {
      for (uint8_t i = 0; i < s_cfg.heat.week[d].count && i < DHW_MAX_INTERVALS_PER_DAY; i++) {
        auto &it = s_cfg.heat.week[d].items[i];
        it.startMin = clampMin(it.startMin);
        it.endMin = clampMin(it.endMin);
        it.valid = (it.startMin != it.endMin);
      }
      if (s_cfg.heat.week[d].count > DHW_MAX_INTERVALS_PER_DAY) s_cfg.heat.week[d].count = DHW_MAX_INTERVALS_PER_DAY;
      for (uint8_t i = 0; i < s_cfg.circ.week[d].count && i < DHW_MAX_INTERVALS_PER_DAY; i++) {
        auto &it = s_cfg.circ.week[d].items[i];
        it.startMin = clampMin(it.startMin);
        it.endMin = clampMin(it.endMin);
        it.valid = (it.startMin != it.endMin);
      }
      if (s_cfg.circ.week[d].count > DHW_MAX_INTERVALS_PER_DAY) s_cfg.circ.week[d].count = DHW_MAX_INTERVALS_PER_DAY;
    }
  }

  static String scheduleToJson(const DhwDaySchedule week[7]) {
    DynamicJsonDocument doc(3072);
    JsonArray root = doc.to<JsonArray>();
    for (int d = 0; d < 7; d++) {
      JsonArray day = root.createNestedArray();
      const uint8_t count = week[d].count > DHW_MAX_INTERVALS_PER_DAY ? DHW_MAX_INTERVALS_PER_DAY : week[d].count;
      for (uint8_t i = 0; i < count; i++) {
        const DhwInterval &it = week[d].items[i];
        if (!it.valid || it.startMin == it.endMin) continue;
        JsonObject o = day.createNestedObject();
        o["startMin"] = it.startMin;
        o["endMin"] = it.endMin;
      }
    }
    String out;
    serializeJson(doc, out);
    return out;
  }

  static void scheduleFromJson(const String& json, DhwDaySchedule week[7]) {
    for (int d = 0; d < 7; d++) week[d] = DhwDaySchedule{};
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, json)) return;
    JsonArray root = doc.as<JsonArray>();
    if (root.isNull()) return;
    for (int d = 0; d < 7 && d < (int)root.size(); d++) {
      JsonArray day = root[d].as<JsonArray>();
      if (day.isNull()) continue;
      uint8_t count = 0;
      for (JsonVariant v : day) {
        if (count >= DHW_MAX_INTERVALS_PER_DAY) break;
        JsonObject o = v.as<JsonObject>();
        if (o.isNull()) continue;
        uint16_t s = clampMin((int)(o["startMin"] | 0));
        uint16_t e = clampMin((int)(o["endMin"] | 0));
        if (s == e) continue;
        week[d].items[count].startMin = s;
        week[d].items[count].endMin = e;
        week[d].items[count].valid = true;
        count++;
      }
      week[d].count = count;
    }
  }

  static void loadFromPrefs() {
    s_cfg = DhwConfig{};
    s_cfg.enabled = ConfigStore::getDhwEnabled();
    s_cfg.disableEquithermDuringHeat = ConfigStore::getDhwDisableEquithermDuringHeat();
    s_cfg.tempMaxAgeMs = ConfigStore::getDhwTempMaxAgeMs();

    s_cfg.heat.useInput = ConfigStore::getDhwHeatUseInput();
    s_cfg.heat.useSchedule = ConfigStore::getDhwHeatUseSchedule();
    s_cfg.heat.scheduleEnabled = ConfigStore::getDhwHeatScheduleEnabled();
    s_cfg.heat.targetTempC = ConfigStore::getDhwHeatTargetTempC();
    s_cfg.heat.hysteresisC = ConfigStore::getDhwHeatHysteresisC();
    s_cfg.heat.requestMode = ConfigStore::getDhwHeatRequestMode();
    s_cfg.heat.otEnableDhw = ConfigStore::getDhwHeatOtEnableDhw();
    s_cfg.heat.otDhwSetpointC = ConfigStore::getDhwHeatOtDhwSetpointC();
    s_cfg.heat.relayRequest = ConfigStore::getDhwHeatRelayRequest();
    s_cfg.heat.driveValveRelay = ConfigStore::getDhwHeatDriveValveRelay();
    s_cfg.heat.valveRelayIndex = ConfigStore::getDhwHeatValveRelayIndex();
    s_cfg.heat.boilerRelayIndex = ConfigStore::getDhwHeatBoilerRelayIndex();
    s_cfg.heat.valveLeadMs = ConfigStore::getDhwHeatValveLeadMs();
    s_cfg.heat.valveSwitchBackMs = ConfigStore::getDhwHeatValveSwitchBackMs();
    s_cfg.heat.boilerOffHoldMs = ConfigStore::getDhwHeatBoilerOffHoldMs();
    scheduleFromJson(ConfigStore::getDhwHeatScheduleJson(), s_cfg.heat.week);

    s_cfg.circ.useInput = ConfigStore::getDhwCircUseInput();
    s_cfg.circ.useSchedule = ConfigStore::getDhwCircUseSchedule();
    s_cfg.circ.scheduleEnabled = ConfigStore::getDhwCircScheduleEnabled();
    s_cfg.circ.pulseEnabled = ConfigStore::getDhwCircPulseEnabled();
    s_cfg.circ.pulseOnMin = ConfigStore::getDhwCircPulseOnMin();
    s_cfg.circ.pulseOffMin = ConfigStore::getDhwCircPulseOffMin();
    s_cfg.circ.relayIndex = ConfigStore::getDhwCircRelayIndex();
    s_cfg.antiLegionella.enabled = ConfigStore::getDhwAntiLegionellaEnabled();
    s_cfg.antiLegionella.weekday = ConfigStore::getDhwAntiLegionellaWeekday();
    s_cfg.antiLegionella.startMin = ConfigStore::getDhwAntiLegionellaStartMin();
    s_cfg.antiLegionella.targetTempC = ConfigStore::getDhwAntiLegionellaTargetTempC();
    s_cfg.antiLegionella.holdMin = ConfigStore::getDhwAntiLegionellaHoldMin();
    scheduleFromJson(ConfigStore::getDhwCircScheduleJson(), s_cfg.circ.week);

    const uint32_t persistedDayKey = ConfigStore::getDhwAntiLegionellaLastDayKey();
    s_legionellaLastDayKey = persistedDayKey ? (int)persistedDayKey : -1;

    clampConfig();
  }

  static bool hmNow(uint16_t &minsOut, int &weekdayOut, String* isoOut = nullptr) {
    weekdayOut = -1;
    minsOut = 0;
    if (!networkIsTimeValid()) return false;
    const String iso = networkGetTimeIso();
    if (isoOut) *isoOut = iso;
    if (iso.length() < 16) return false;
    int hh = iso.substring(11, 13).toInt();
    int mm = iso.substring(14, 16).toInt();
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    minsOut = (uint16_t)(hh * 60 + mm);

    int y = iso.substring(0, 4).toInt();
    int m = iso.substring(5, 7).toInt();
    int d = iso.substring(8, 10).toInt();
    if (y <= 0 || m <= 0 || d <= 0) return false;
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100;
    int J = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    int dow = ((h + 5) % 7); // Mon=0..Sun=6
    if (dow < 0 || dow > 6) return false;
    weekdayOut = dow;
    return true;
  }

  static bool intervalActive(const DhwInterval &it, uint16_t mins) {
    if (!it.valid || it.startMin == it.endMin) return false;
    if (it.startMin < it.endMin) return mins >= it.startMin && mins < it.endMin;
    return mins >= it.startMin || mins < it.endMin;
  }

  static bool findActiveInterval(const DhwDaySchedule week[7], bool enabled, bool &timeValidOut, int &weekdayOut, uint16_t &minsOut, uint16_t *startMinOut = nullptr, uint16_t *endMinOut = nullptr) {
    timeValidOut = false;
    weekdayOut = -1;
    minsOut = 0;
    if (startMinOut) *startMinOut = 0;
    if (endMinOut) *endMinOut = 0;
    if (!enabled) return false;
    if (!hmNow(minsOut, weekdayOut, nullptr)) return false;
    timeValidOut = true;
    if (weekdayOut < 0 || weekdayOut > 6) return false;
    const DhwDaySchedule &day = week[weekdayOut];
    const uint8_t count = day.count > DHW_MAX_INTERVALS_PER_DAY ? DHW_MAX_INTERVALS_PER_DAY : day.count;
    for (uint8_t i = 0; i < count; i++) {
      if (intervalActive(day.items[i], minsOut)) {
        if (startMinOut) *startMinOut = day.items[i].startMin;
        if (endMinOut) *endMinOut = day.items[i].endMin;
        return true;
      }
    }
    return false;
  }

  static bool scheduleActive(const DhwDaySchedule week[7], bool enabled, bool &timeValidOut) {
    int weekday = -1;
    uint16_t mins = 0;
    return findActiveInterval(week, enabled, timeValidOut, weekday, mins, nullptr, nullptr);
  }

  static void updateCircPulseAnchor(bool circRequested, bool circScheduleActive, int circWeekday, uint16_t circStartMin, uint32_t nowMs) {
    if (!circRequested) {
      s_circPulseAnchorValid = false;
      s_circPulseAnchorWeekday = -1;
      s_circPulseAnchorStartMin = 0;
      s_circPulseAnchorMs = nowMs;
      return;
    }

    if (circScheduleActive) {
      if (!s_circPulseAnchorValid || s_circPulseAnchorWeekday != circWeekday || s_circPulseAnchorStartMin != circStartMin) {
        s_circPulseAnchorValid = true;
        s_circPulseAnchorWeekday = circWeekday;
        s_circPulseAnchorStartMin = circStartMin;
        s_circPulseAnchorMs = nowMs;
      }
      return;
    }

    if (!s_circPulseAnchorValid || s_circPulseAnchorWeekday != -2) {
      s_circPulseAnchorValid = true;
      s_circPulseAnchorWeekday = -2; // input/manual anchor
      s_circPulseAnchorStartMin = 0;
      s_circPulseAnchorMs = nowMs;
    }
  }

  static bool circPulseOn(uint32_t nowMs, bool circScheduleActive, bool timeValid, int weekday, uint16_t nowMin, uint16_t intervalStartMin) {
    if (!s_cfg.circ.pulseEnabled) return true;
    const uint32_t onMs = (uint32_t)s_cfg.circ.pulseOnMin * 60000UL;
    const uint32_t offMs = (uint32_t)s_cfg.circ.pulseOffMin * 60000UL;
    const uint32_t period = onMs + offMs;
    if (period == 0) return true;
    if (onMs == 0) return false;

    if (circScheduleActive && timeValid) {
      int32_t elapsedMin = (int32_t)nowMin - (int32_t)intervalStartMin;
      if (elapsedMin < 0) elapsedMin += 1440;
      const uint32_t pos = ((uint32_t)elapsedMin * 60000UL) % period;
      return pos < onMs;
    }

    if (!s_circPulseAnchorValid) return true;
    const uint32_t pos = (uint32_t)(nowMs - s_circPulseAnchorMs) % period;
    return pos < onMs;
  }

  static void applyOutputs(bool valveOn, bool boilerRelayOn, bool circActive) {
    if (s_cfg.heat.driveValveRelay) relaySet(rid(s_cfg.heat.valveRelayIndex), valveOn);
    if (s_cfg.heat.relayRequest) relaySet(rid(s_cfg.heat.boilerRelayIndex), boilerRelayOn && s_cfg.heat.requestMode == "relay");
    relaySet(rid(s_cfg.circ.relayIndex), circActive);
  }

  static void syncOpenTherm(bool boilerDemandActive) {
    if (s_cfg.heat.requestMode != "opentherm") {
      openthermClearDhwRequest();
      s_st.otDhwEnable = false;
      return;
    }

    if (!boilerDemandActive) {
      openthermClearDhwRequest();
      s_st.otDhwEnable = false;
      return;
    }

    OpenThermSourceRequest req;
    req.active = true;
    req.chEnableSet = true;
    req.chEnable = false;
    req.dhwEnableSet = true;
    req.dhwEnable = s_cfg.heat.otEnableDhw;
    if (s_cfg.heat.otEnableDhw) {
      req.dhwSetpointSet = true;
      req.dhwSetpointC = s_cfg.heat.otDhwSetpointC;
    }
    String err;
    const bool ok = openthermSetDhwRequest(req, err);
    s_st.otDhwEnable = ok ? req.dhwEnable : false;
  }

  static void updateHeatSequence(bool request, bool boilerDhwMode, bool tempAllowsHeating, uint32_t nowMs, bool& heatActiveOut, bool& valveOnOut, bool& boilerDemandOut) {
    if (boilerDhwMode) {
      s_heatPhase = DhwHeatPhase::Heating;
      s_heatPhaseUntilMs = 0;
    }

    if (!request) {
      if (s_heatPhase == DhwHeatPhase::Heating && s_cfg.heat.driveValveRelay) {
        s_heatPhase = DhwHeatPhase::SwitchingBack;
        s_heatPhaseUntilMs = nowMs + s_cfg.heat.valveSwitchBackMs;
      } else if (s_heatPhase == DhwHeatPhase::SwitchingToDhw || s_heatPhase == DhwHeatPhase::SwitchingBack) {
        if (s_heatPhaseUntilMs != 0 && (int32_t)(nowMs - s_heatPhaseUntilMs) >= 0) {
          s_heatPhase = DhwHeatPhase::Idle;
          s_heatPhaseUntilMs = 0;
        }
      } else {
        s_heatPhase = DhwHeatPhase::Idle;
        s_heatPhaseUntilMs = 0;
      }
    } else {
      switch (s_heatPhase) {
        case DhwHeatPhase::Idle:
          if (tempAllowsHeating) {
            if (s_cfg.heat.driveValveRelay) {
              s_heatPhase = DhwHeatPhase::SwitchingToDhw;
              s_heatPhaseUntilMs = nowMs + s_cfg.heat.valveLeadMs;
            } else {
              s_heatPhase = DhwHeatPhase::Heating;
              s_heatPhaseUntilMs = 0;
            }
          }
          break;
        case DhwHeatPhase::SwitchingToDhw:
          if (!tempAllowsHeating) {
            s_heatPhase = DhwHeatPhase::Idle;
            s_heatPhaseUntilMs = 0;
          } else if (s_heatPhaseUntilMs != 0 && (int32_t)(nowMs - s_heatPhaseUntilMs) >= 0) {
            s_heatPhase = DhwHeatPhase::Heating;
            s_heatPhaseUntilMs = 0;
          }
          break;
        case DhwHeatPhase::Heating:
          if (!tempAllowsHeating && !boilerDhwMode) {
            if (s_cfg.heat.driveValveRelay) {
              s_heatPhase = DhwHeatPhase::SwitchingBack;
              s_heatPhaseUntilMs = nowMs + s_cfg.heat.valveSwitchBackMs;
            } else {
              s_heatPhase = DhwHeatPhase::Idle;
              s_heatPhaseUntilMs = 0;
            }
          }
          break;
        case DhwHeatPhase::SwitchingBack:
          if (tempAllowsHeating) {
            if (s_cfg.heat.driveValveRelay) {
              s_heatPhase = DhwHeatPhase::SwitchingToDhw;
              s_heatPhaseUntilMs = nowMs + s_cfg.heat.valveLeadMs;
            } else {
              s_heatPhase = DhwHeatPhase::Heating;
              s_heatPhaseUntilMs = 0;
            }
          } else if (s_heatPhaseUntilMs != 0 && (int32_t)(nowMs - s_heatPhaseUntilMs) >= 0) {
            s_heatPhase = DhwHeatPhase::Idle;
            s_heatPhaseUntilMs = 0;
          }
          break;
      }
    }

    valveOnOut = (s_heatPhase == DhwHeatPhase::SwitchingToDhw || s_heatPhase == DhwHeatPhase::Heating || s_heatPhase == DhwHeatPhase::SwitchingBack);
    boilerDemandOut = boilerDhwMode || (s_heatPhase == DhwHeatPhase::Heating);
    if (s_heatPhase == DhwHeatPhase::SwitchingBack && s_heatPhaseUntilMs != 0) {
      const int32_t remaining = (int32_t)(s_heatPhaseUntilMs - nowMs);
      if (remaining > (int32_t)s_cfg.heat.boilerOffHoldMs) boilerDemandOut = false;
    }
    heatActiveOut = boilerDemandOut || valveOnOut;
  }

  static void fillScheduleJson(JsonObject parent, const DhwDaySchedule week[7]) {
    JsonArray days = parent.createNestedArray("week");
    static const char* names[7] = {"mon","tue","wed","thu","fri","sat","sun"};
    for (int d = 0; d < 7; d++) {
      JsonObject day = days.createNestedObject();
      day["day"] = names[d];
      JsonArray arr = day.createNestedArray("intervals");
      const uint8_t count = week[d].count > DHW_MAX_INTERVALS_PER_DAY ? DHW_MAX_INTERVALS_PER_DAY : week[d].count;
      for (uint8_t i = 0; i < count; i++) {
        const DhwInterval &it = week[d].items[i];
        if (!it.valid) continue;
        JsonObject o = arr.createNestedObject();
        o["startMin"] = it.startMin;
        o["endMin"] = it.endMin;
      }
    }
  }

  static void fillConfigJson(JsonObject out) {
    out["enabled"] = s_cfg.enabled;
    out["disableEquithermDuringHeat"] = s_cfg.disableEquithermDuringHeat;
    out["tempMaxAgeMs"] = s_cfg.tempMaxAgeMs;

    JsonObject heat = out.createNestedObject("heat");
    heat["useInput"] = s_cfg.heat.useInput;
    heat["useSchedule"] = s_cfg.heat.useSchedule;
    heat["scheduleEnabled"] = s_cfg.heat.scheduleEnabled;
    heat["targetTempC"] = s_cfg.heat.targetTempC;
    heat["hysteresisC"] = s_cfg.heat.hysteresisC;
    heat["requestMode"] = s_cfg.heat.requestMode;
    heat["otEnableDhw"] = s_cfg.heat.otEnableDhw;
    heat["otDhwSetpointC"] = s_cfg.heat.otDhwSetpointC;
    heat["relayRequest"] = s_cfg.heat.relayRequest;
    heat["driveValveRelay"] = s_cfg.heat.driveValveRelay;
    heat["valveRelay"] = (uint32_t)(s_cfg.heat.valveRelayIndex + 1);
    heat["boilerRelay"] = (uint32_t)(s_cfg.heat.boilerRelayIndex + 1);
    heat["valveLeadMs"] = s_cfg.heat.valveLeadMs;
    heat["valveSwitchBackMs"] = s_cfg.heat.valveSwitchBackMs;
    heat["boilerOffHoldMs"] = s_cfg.heat.boilerOffHoldMs;
    fillScheduleJson(heat.createNestedObject("schedule"), s_cfg.heat.week);

    JsonObject circ = out.createNestedObject("circ");
    circ["useInput"] = s_cfg.circ.useInput;
    circ["useSchedule"] = s_cfg.circ.useSchedule;
    circ["scheduleEnabled"] = s_cfg.circ.scheduleEnabled;
    circ["pulseEnabled"] = s_cfg.circ.pulseEnabled;
    circ["pulseOnMin"] = s_cfg.circ.pulseOnMin;
    circ["pulseOffMin"] = s_cfg.circ.pulseOffMin;
    circ["relay"] = (uint32_t)(s_cfg.circ.relayIndex + 1);
    fillScheduleJson(circ.createNestedObject("schedule"), s_cfg.circ.week);

    JsonObject al = out.createNestedObject("antiLegionella");
    al["enabled"] = s_cfg.antiLegionella.enabled;
    al["weekday"] = s_cfg.antiLegionella.weekday;
    al["startMin"] = s_cfg.antiLegionella.startMin;
    al["targetTempC"] = s_cfg.antiLegionella.targetTempC;
    al["holdMin"] = s_cfg.antiLegionella.holdMin;
  }

  static void fillStatusJson(JsonObject out) {
    out["enabled"] = s_st.enabled;
    out["timeValid"] = s_st.timeValid;
    out["timeIso"] = s_st.timeIso;
    out["heatInputActive"] = s_st.heatInputActive;
    out["heatScheduleActive"] = s_st.heatScheduleActive;
    out["boilerDhwMode"] = s_st.boilerDhwMode;
    out["heatRequested"] = s_st.heatRequested;
    out["heatActive"] = s_st.heatActive;
    out["heatSatisfied"] = s_st.heatSatisfied;
    out["heatReason"] = s_st.heatReason;
    out["heatPhase"] = s_st.heatPhase;
    out["heatSequenceActive"] = s_st.heatSequenceActive;
    out["requestMode"] = s_st.requestMode;
    out["circInputActive"] = s_st.circInputActive;
    out["circScheduleActive"] = s_st.circScheduleActive;
    out["circRequested"] = s_st.circRequested;
    out["circPulseOn"] = s_st.circPulseOn;
    out["circActive"] = s_st.circActive;
    out["circReason"] = s_st.circReason;
    if (isfinite(s_st.tankTempC)) out["tankTempC"] = s_st.tankTempC; else out["tankTempC"] = nullptr;
    out["tankTempAgeMs"] = s_st.tankTempAgeMs;
    if (isfinite(s_st.targetTempC)) out["targetTempC"] = s_st.targetTempC; else out["targetTempC"] = nullptr;
    if (isfinite(s_st.otTargetTempC)) out["otTargetTempC"] = s_st.otTargetTempC; else out["otTargetTempC"] = nullptr;
    out["valveRelayOn"] = s_st.valveRelayOn;
    out["boilerRelayOn"] = s_st.boilerRelayOn;
    out["circRelayOn"] = s_st.circRelayOn;
    out["otDhwEnable"] = s_st.otDhwEnable;
    out["antiLegionellaActive"] = s_st.antiLegionellaActive;
    out["antiLegionellaDone"] = s_st.antiLegionellaDone;
    out["lastEvalMs"] = s_st.lastEvalMs;
  }

  static void applyScheduleFromJson(JsonVariantConst weekV, DhwDaySchedule week[7]) {
    for (int d = 0; d < 7; d++) week[d] = DhwDaySchedule{};
    if (!weekV.is<JsonArrayConst>()) return;
    JsonArrayConst arr = weekV.as<JsonArrayConst>();
    for (JsonVariantConst vv : arr) {
      JsonObjectConst day = vv.as<JsonObjectConst>();
      if (day.isNull()) continue;
      const char* dn = day["day"] | "";
      int idx = -1;
      if (!strcmp(dn, "mon")) idx = 0; else if (!strcmp(dn, "tue")) idx = 1; else if (!strcmp(dn, "wed")) idx = 2;
      else if (!strcmp(dn, "thu")) idx = 3; else if (!strcmp(dn, "fri")) idx = 4; else if (!strcmp(dn, "sat")) idx = 5; else if (!strcmp(dn, "sun")) idx = 6;
      if (idx < 0 || idx > 6) continue;
      JsonArrayConst ints = day["intervals"].as<JsonArrayConst>();
      if (ints.isNull()) continue;
      uint8_t count = 0;
      for (JsonVariantConst iv : ints) {
        if (count >= DHW_MAX_INTERVALS_PER_DAY) break;
        JsonObjectConst o = iv.as<JsonObjectConst>();
        if (o.isNull()) continue;
        uint16_t s = clampMin((int)(o["startMin"] | 0));
        uint16_t e = clampMin((int)(o["endMin"] | 0));
        if (s == e) continue;
        week[idx].items[count].startMin = s;
        week[idx].items[count].endMin = e;
        week[idx].items[count].valid = true;
        count++;
      }
      week[idx].count = count;
    }
  }
}

void dhwInit() {
  if (s_inited) return;
  s_inited = true;
  dhwReloadFromStore();
}

void dhwReloadFromStore() {
  loadFromPrefs();
}

void dhwLoop() {
  if (!s_inited) dhwInit();
  const bool prevHeatActive = s_st.heatActive;
  const bool prevOtDhwEnable = s_st.otDhwEnable;
  s_st = DhwStatus{};
  s_st.enabled = s_cfg.enabled;
  s_st.requestMode = s_cfg.heat.requestMode;
  s_st.heatPhase = heatPhaseName(s_heatPhase);
  s_st.heatSequenceActive = (s_heatPhase != DhwHeatPhase::Idle);
  s_st.targetTempC = s_cfg.heat.targetTempC;
  s_st.otTargetTempC = s_cfg.heat.otDhwSetpointC;
  s_st.timeValid = networkIsTimeValid();
  s_st.timeIso = networkGetTimeIso();
  s_st.lastEvalMs = millis();

  if (!s_cfg.enabled) {
    s_heatPhase = DhwHeatPhase::Idle;
    s_heatPhaseUntilMs = 0;
    applyOutputs(false, false, false);
    syncOpenTherm(false);
    equithermSetExternalBlock(false);
    s_st.heatReason = "disabled";
    s_st.circReason = "disabled";
    return;
  }

  s_st.heatInputActive = s_cfg.heat.useInput && inputGetState(InputId::IN2);
  bool heatSchedTimeValid = false;
  s_st.heatScheduleActive = s_cfg.heat.useSchedule && scheduleActive(s_cfg.heat.week, s_cfg.heat.scheduleEnabled, heatSchedTimeValid);
  s_st.circInputActive = s_cfg.circ.useInput && inputGetState(InputId::IN3);
  bool circSchedTimeValid = false;
  int circScheduleWeekday = -1;
  uint16_t circNowMin = 0;
  uint16_t circStartMin = 0;
  uint16_t circEndMin = 0;
  s_st.circScheduleActive = s_cfg.circ.useSchedule && findActiveInterval(s_cfg.circ.week, s_cfg.circ.scheduleEnabled, circSchedTimeValid, circScheduleWeekday, circNowMin, &circStartMin, &circEndMin);

  TempValue tv = TemperatureManager::get(TempRole::DhwTank, s_cfg.tempMaxAgeMs);
  if (tv.valid && isfinite(tv.c)) {
    s_st.tankTempC = tv.c;
    s_st.tankTempAgeMs = tv.ageMs;
  }

  const OpenThermStatusSnapshot ot = openthermGetStatus();
  bool boilerDhwMode = ot.present && ot.ready && ot.dhwActive;
  if (!boilerDhwMode && ot.present && ot.ready && isfinite(ot.dhwSetpointC) && ot.dhwEnable && ot.flameOn && !ot.chActive) {
    boilerDhwMode = true;
  }
  s_st.boilerDhwMode = boilerDhwMode;

  bool antiActive = false;
  bool antiDone = false;
  if (s_cfg.antiLegionella.enabled && s_st.timeValid) {
    time_t nowEpoch = (time_t)networkGetTimeEpoch();
    struct tm tmv{};
    localtime_r(&nowEpoch, &tmv);
    const int weekday = (tmv.tm_wday == 0) ? 6 : (tmv.tm_wday - 1);
    const int nowMin = tmv.tm_hour * 60 + tmv.tm_min;
    const int dayKey = (tmv.tm_year * 1000) + tmv.tm_yday;
    if (s_legionellaLastDayKey > dayKey) {
      s_legionellaLastDayKey = -1;
      ConfigStore::setDhwAntiLegionellaLastDayKey(0);
    }
    antiDone = (s_legionellaLastDayKey == dayKey);
    if (weekday == (int)s_cfg.antiLegionella.weekday && nowMin >= (int)s_cfg.antiLegionella.startMin && !antiDone) {
      antiActive = true;
      s_st.targetTempC = s_cfg.antiLegionella.targetTempC;
      if (isfinite(s_st.tankTempC) && s_st.tankTempC >= s_cfg.antiLegionella.targetTempC) {
        if (!s_legionellaHoldUntilMs) s_legionellaHoldUntilMs = s_st.lastEvalMs + (uint32_t)s_cfg.antiLegionella.holdMin * 60000UL;
        antiDone = ((int32_t)(s_st.lastEvalMs - s_legionellaHoldUntilMs) >= 0);
      } else {
        s_legionellaHoldUntilMs = 0;
      }
      if (antiDone) {
        s_legionellaLastDayKey = dayKey;
        ConfigStore::setDhwAntiLegionellaLastDayKey((uint32_t)dayKey);
        s_legionellaHoldUntilMs = 0;
        antiActive = false;
      }
    } else if (weekday != (int)s_cfg.antiLegionella.weekday || nowMin < (int)s_cfg.antiLegionella.startMin) {
      s_legionellaHoldUntilMs = 0;
    }
  } else {
    s_legionellaHoldUntilMs = 0;
  }
  s_st.antiLegionellaActive = antiActive;
  s_st.antiLegionellaDone = antiDone;

  bool request = s_st.heatInputActive || s_st.heatScheduleActive || s_st.boilerDhwMode || antiActive;
  if (s_forceHeat && (int32_t)(millis() - s_forceHeatUntilMs) < 0) request = true;
  else if (s_forceHeat) s_forceHeat = false;
  s_st.heatRequested = request;

  bool satisfied = false;
  if (isfinite(s_st.tankTempC)) satisfied = s_st.tankTempC >= s_st.targetTempC;
  s_st.heatSatisfied = satisfied;

  bool tempAllowsHeating = false;
  if (request && isfinite(s_st.tankTempC)) {
    const float resumeAt = s_st.targetTempC - s_cfg.heat.hysteresisC;
    const bool wasHeating = prevHeatActive;
    const bool wasOtHeating = prevOtDhwEnable;
    const bool continueLatchedHeat = (s_cfg.heat.requestMode == "opentherm") ? wasOtHeating : wasHeating;
    if (continueLatchedHeat) tempAllowsHeating = (s_st.tankTempC < s_st.targetTempC);
    else tempAllowsHeating = (s_st.tankTempC < resumeAt);
  }

  bool heatActive = false;
  bool valveOn = false;
  bool boilerDemandActive = false;
  updateHeatSequence(request, s_st.boilerDhwMode, tempAllowsHeating, s_st.lastEvalMs, heatActive, valveOn, boilerDemandActive);
  s_st.heatActive = heatActive;
  s_st.heatPhase = heatPhaseName(s_heatPhase);
  s_st.heatSequenceActive = (s_heatPhase != DhwHeatPhase::Idle);
  if (s_st.boilerDhwMode) s_st.heatReason = "boiler_dhw_active_ot";
  else if (!request) s_st.heatReason = (s_heatPhase == DhwHeatPhase::SwitchingBack) ? "switching_back" : "idle";
  else if (!isfinite(s_st.tankTempC)) s_st.heatReason = "request_blocked_no_valid_temp";
  else if (s_heatPhase == DhwHeatPhase::SwitchingToDhw) s_st.heatReason = "switching_to_dhw";
  else if (s_heatPhase == DhwHeatPhase::SwitchingBack) s_st.heatReason = "switching_back";
  else if (boilerDemandActive) s_st.heatReason = s_st.antiLegionellaActive ? "anti_legionella" : "heating";
  else s_st.heatReason = "target_reached";

  s_st.circRequested = s_st.circInputActive || s_st.circScheduleActive || s_forceCirc;
  updateCircPulseAnchor(s_st.circRequested, s_st.circScheduleActive, circScheduleWeekday, circStartMin, s_st.lastEvalMs);
  s_st.circPulseOn = s_st.circRequested ? circPulseOn(s_st.lastEvalMs, s_st.circScheduleActive, circSchedTimeValid, circScheduleWeekday, circNowMin, circStartMin) : false;
  s_st.circActive = s_st.circRequested && (s_cfg.circ.pulseEnabled ? s_st.circPulseOn : true);
  if (!s_st.circRequested) s_st.circReason = "idle";
  else if (s_forceCirc) s_st.circReason = s_cfg.circ.pulseEnabled ? (s_st.circPulseOn ? "manual_pulse_on" : "manual_pulse_off") : "manual";
  else if (!s_cfg.circ.pulseEnabled) s_st.circReason = "continuous";
  else s_st.circReason = s_st.circPulseOn ? "pulse_on" : "pulse_off";

  const bool shouldBlockEquitherm = s_cfg.disableEquithermDuringHeat && (heatActive || s_heatPhase == DhwHeatPhase::SwitchingToDhw || s_heatPhase == DhwHeatPhase::SwitchingBack);
  equithermSetExternalBlock(shouldBlockEquitherm);

  applyOutputs(valveOn, boilerDemandActive, s_st.circActive);
  s_st.valveRelayOn = relayGetState(rid(s_cfg.heat.valveRelayIndex));
  s_st.boilerRelayOn = relayGetState(rid(s_cfg.heat.boilerRelayIndex));
  s_st.circRelayOn = relayGetState(rid(s_cfg.circ.relayIndex));

  syncOpenTherm(boilerDemandActive);
}

DhwConfig dhwGetConfig() {
  if (!s_inited) dhwInit();
  return s_cfg;
}

DhwStatus dhwGetStatus() {
  return s_st;
}

String dhwGetStatusJson() {
  DynamicJsonDocument doc(4096);
  doc["ok"] = true;
  JsonObject cfg = doc.createNestedObject("config");
  JsonObject st = doc.createNestedObject("status");
  fillConfigJson(cfg);
  fillStatusJson(st);
  String out;
  serializeJson(doc, out);
  return out;
}

void dhwFillFastJson(JsonObject& out) {
  out["en"] = s_st.enabled;
  out["hr"] = s_st.heatRequested;
  out["ha"] = s_st.heatActive;
  out["hs"] = s_st.heatScheduleActive;
  out["bm"] = s_st.boilerDhwMode;
  out["hi"] = s_st.heatInputActive;
  out["cr"] = s_st.circRequested;
  out["ca"] = s_st.circActive;
  out["cs"] = s_st.circScheduleActive;
  out["ci"] = s_st.circInputActive;
  out["cp"] = s_st.circPulseOn;
  out["rm"] = s_st.requestMode;
  out["hp"] = s_st.heatPhase;
  out["hsq"] = s_st.heatSequenceActive;
  if (isfinite(s_st.tankTempC)) out["tt"] = s_st.tankTempC; else out["tt"] = nullptr;
  if (isfinite(s_st.targetTempC)) out["tg"] = s_st.targetTempC; else out["tg"] = nullptr;
  out["vr"] = s_st.valveRelayOn;
  out["br"] = s_st.boilerRelayOn;
  out["rr"] = s_st.circRelayOn;
  out["ode"] = s_st.otDhwEnable;
  out["al"] = s_st.antiLegionellaActive;
}

void dhwApplyConfig(const String& json) {
  if (!s_inited) dhwInit();
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, json)) return;
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) return;
  JsonObjectConst d = root.containsKey("dhw") && root["dhw"].is<JsonObjectConst>() ? root["dhw"].as<JsonObjectConst>() : root;
  if (d.isNull()) return;

  ConfigStore::BatchGuard storeBatch;

  if (d.containsKey("enabled")) s_cfg.enabled = (bool)(d["enabled"] | true);
  if (d.containsKey("disableEquithermDuringHeat")) s_cfg.disableEquithermDuringHeat = (bool)(d["disableEquithermDuringHeat"] | true);
  if (d.containsKey("tempMaxAgeMs")) s_cfg.tempMaxAgeMs = (uint32_t)(d["tempMaxAgeMs"] | s_cfg.tempMaxAgeMs);

  if (d.containsKey("heat") && d["heat"].is<JsonObjectConst>()) {
    JsonObjectConst h = d["heat"].as<JsonObjectConst>();
    if (h.containsKey("useInput")) s_cfg.heat.useInput = (bool)(h["useInput"] | true);
    if (h.containsKey("useSchedule")) s_cfg.heat.useSchedule = (bool)(h["useSchedule"] | true);
    if (h.containsKey("scheduleEnabled")) s_cfg.heat.scheduleEnabled = (bool)(h["scheduleEnabled"] | true);
    if (h.containsKey("targetTempC")) s_cfg.heat.targetTempC = h["targetTempC"] | s_cfg.heat.targetTempC;
    if (h.containsKey("hysteresisC")) s_cfg.heat.hysteresisC = h["hysteresisC"] | s_cfg.heat.hysteresisC;
    if (h.containsKey("requestMode")) s_cfg.heat.requestMode = String((const char*)(h["requestMode"] | s_cfg.heat.requestMode.c_str()));
    if (h.containsKey("otEnableDhw")) s_cfg.heat.otEnableDhw = (bool)(h["otEnableDhw"] | true);
    if (h.containsKey("otDhwSetpointC")) s_cfg.heat.otDhwSetpointC = h["otDhwSetpointC"] | s_cfg.heat.otDhwSetpointC;
    if (h.containsKey("relayRequest")) s_cfg.heat.relayRequest = (bool)(h["relayRequest"] | true);
    if (h.containsKey("driveValveRelay")) s_cfg.heat.driveValveRelay = (bool)(h["driveValveRelay"] | true);
    if (h.containsKey("valveRelay")) s_cfg.heat.valveRelayIndex = clampRelayIndex((int)(h["valveRelay"] | 3) - 1, 2);
    if (h.containsKey("boilerRelay")) s_cfg.heat.boilerRelayIndex = clampRelayIndex((int)(h["boilerRelay"] | 5) - 1, 4);
    if (h.containsKey("valveLeadMs")) s_cfg.heat.valveLeadMs = (uint32_t)(h["valveLeadMs"] | s_cfg.heat.valveLeadMs);
    if (h.containsKey("valveSwitchBackMs")) s_cfg.heat.valveSwitchBackMs = (uint32_t)(h["valveSwitchBackMs"] | s_cfg.heat.valveSwitchBackMs);
    if (h.containsKey("boilerOffHoldMs")) s_cfg.heat.boilerOffHoldMs = (uint32_t)(h["boilerOffHoldMs"] | s_cfg.heat.boilerOffHoldMs);
    if (h.containsKey("schedule") && h["schedule"].is<JsonObjectConst>()) {
      JsonObjectConst s = h["schedule"].as<JsonObjectConst>();
      if (s.containsKey("week")) applyScheduleFromJson(s["week"], s_cfg.heat.week);
    }
  }

  if (d.containsKey("antiLegionella") && d["antiLegionella"].is<JsonObjectConst>()) {
    JsonObjectConst a = d["antiLegionella"].as<JsonObjectConst>();
    if (a.containsKey("enabled")) s_cfg.antiLegionella.enabled = (bool)(a["enabled"] | false);
    if (a.containsKey("weekday")) s_cfg.antiLegionella.weekday = (uint8_t)(a["weekday"] | s_cfg.antiLegionella.weekday);
    if (a.containsKey("startMin")) s_cfg.antiLegionella.startMin = (uint16_t)(a["startMin"] | s_cfg.antiLegionella.startMin);
    if (a.containsKey("targetTempC")) s_cfg.antiLegionella.targetTempC = a["targetTempC"] | s_cfg.antiLegionella.targetTempC;
    if (a.containsKey("holdMin")) s_cfg.antiLegionella.holdMin = (uint16_t)(a["holdMin"] | s_cfg.antiLegionella.holdMin);
  }

  if (d.containsKey("circ") && d["circ"].is<JsonObjectConst>()) {
    JsonObjectConst c = d["circ"].as<JsonObjectConst>();
    if (c.containsKey("useInput")) s_cfg.circ.useInput = (bool)(c["useInput"] | true);
    if (c.containsKey("useSchedule")) s_cfg.circ.useSchedule = (bool)(c["useSchedule"] | true);
    if (c.containsKey("scheduleEnabled")) s_cfg.circ.scheduleEnabled = (bool)(c["scheduleEnabled"] | true);
    if (c.containsKey("pulseEnabled")) s_cfg.circ.pulseEnabled = (bool)(c["pulseEnabled"] | true);
    if (c.containsKey("pulseOnMin")) s_cfg.circ.pulseOnMin = (uint16_t)(c["pulseOnMin"] | s_cfg.circ.pulseOnMin);
    if (c.containsKey("pulseOffMin")) s_cfg.circ.pulseOffMin = (uint16_t)(c["pulseOffMin"] | s_cfg.circ.pulseOffMin);
    if (c.containsKey("relay")) s_cfg.circ.relayIndex = clampRelayIndex((int)(c["relay"] | 4) - 1, 3);
    if (c.containsKey("schedule") && c["schedule"].is<JsonObjectConst>()) {
      JsonObjectConst s = c["schedule"].as<JsonObjectConst>();
      if (s.containsKey("week")) applyScheduleFromJson(s["week"], s_cfg.circ.week);
    }
  }

  clampConfig();

  ConfigStore::setDhwEnabled(s_cfg.enabled);
  ConfigStore::setDhwDisableEquithermDuringHeat(s_cfg.disableEquithermDuringHeat);
  ConfigStore::setDhwTempMaxAgeMs(s_cfg.tempMaxAgeMs);
  ConfigStore::setDhwHeatUseInput(s_cfg.heat.useInput);
  ConfigStore::setDhwHeatUseSchedule(s_cfg.heat.useSchedule);
  ConfigStore::setDhwHeatScheduleEnabled(s_cfg.heat.scheduleEnabled);
  ConfigStore::setDhwHeatTargetTempC(s_cfg.heat.targetTempC);
  ConfigStore::setDhwHeatHysteresisC(s_cfg.heat.hysteresisC);
  ConfigStore::setDhwHeatRequestMode(s_cfg.heat.requestMode);
  ConfigStore::setDhwHeatOtEnableDhw(s_cfg.heat.otEnableDhw);
  ConfigStore::setDhwHeatOtDhwSetpointC(s_cfg.heat.otDhwSetpointC);
  ConfigStore::setDhwHeatRelayRequest(s_cfg.heat.relayRequest);
  ConfigStore::setDhwHeatDriveValveRelay(s_cfg.heat.driveValveRelay);
  ConfigStore::setDhwHeatValveRelayIndex(s_cfg.heat.valveRelayIndex);
  ConfigStore::setDhwHeatBoilerRelayIndex(s_cfg.heat.boilerRelayIndex);
  ConfigStore::setDhwHeatValveLeadMs(s_cfg.heat.valveLeadMs);
  ConfigStore::setDhwHeatValveSwitchBackMs(s_cfg.heat.valveSwitchBackMs);
  ConfigStore::setDhwHeatBoilerOffHoldMs(s_cfg.heat.boilerOffHoldMs);
  ConfigStore::setDhwHeatScheduleJson(scheduleToJson(s_cfg.heat.week));
  ConfigStore::setDhwCircUseInput(s_cfg.circ.useInput);
  ConfigStore::setDhwCircUseSchedule(s_cfg.circ.useSchedule);
  ConfigStore::setDhwCircScheduleEnabled(s_cfg.circ.scheduleEnabled);
  ConfigStore::setDhwCircPulseEnabled(s_cfg.circ.pulseEnabled);
  ConfigStore::setDhwCircPulseOnMin(s_cfg.circ.pulseOnMin);
  ConfigStore::setDhwCircPulseOffMin(s_cfg.circ.pulseOffMin);
  ConfigStore::setDhwCircRelayIndex(s_cfg.circ.relayIndex);
  ConfigStore::setDhwCircScheduleJson(scheduleToJson(s_cfg.circ.week));
  ConfigStore::setDhwAntiLegionellaEnabled(s_cfg.antiLegionella.enabled);
  ConfigStore::setDhwAntiLegionellaWeekday(s_cfg.antiLegionella.weekday);
  ConfigStore::setDhwAntiLegionellaStartMin(s_cfg.antiLegionella.startMin);
  ConfigStore::setDhwAntiLegionellaTargetTempC(s_cfg.antiLegionella.targetTempC);
  ConfigStore::setDhwAntiLegionellaHoldMin(s_cfg.antiLegionella.holdMin);

  // If DHW should control the boiler over OpenTherm, make sure the OT module is
  // enabled and in control mode. Otherwise /api/dhw/cmd would be accepted but the
  // later runtime OT command would fail with disabled/readOnly.
  if (s_cfg.enabled && s_cfg.heat.requestMode == "opentherm") {
    if (!ConfigStore::getOtEnabled()) ConfigStore::setOtEnabled(true);
    if (!ConfigStore::getOtAutoStart()) ConfigStore::setOtAutoStart(true);
    if (ConfigStore::getOtMode() == "readOnly") ConfigStore::setOtMode("control");

    DynamicJsonDocument wrap(256);
    JsonObject ot = wrap.createNestedObject("opentherm");
    ot["enabled"] = true;
    ot["autoStart"] = true;
    ot["mode"] = "control";
    ot["boilerControl"] = "opentherm";
    String js;
    serializeJson(wrap, js);
    openthermApplyConfig(js);
  }
}

bool dhwHandleCmdJson(const String& json, String& outErr) {
  outErr = "";
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json) || !doc.is<JsonObject>()) {
    outErr = "bad json";
    return false;
  }
  JsonObject o = doc.as<JsonObject>();
  if (o.containsKey("heatActive")) {
    const bool on = (bool)(o["heatActive"] | false);
    s_forceHeat = on;
    s_forceHeatUntilMs = on ? (millis() + 15UL * 60000UL) : 0;
    if (!on) {
      s_heatPhase = DhwHeatPhase::Idle;
      s_heatPhaseUntilMs = 0;
      applyOutputs(false, false, s_st.circActive);
      syncOpenTherm(false);
    }
  }
  if (o.containsKey("boostMin")) {
    const uint32_t min = (uint32_t)(o["boostMin"] | 15);
    s_forceHeat = true;
    s_forceHeatUntilMs = millis() + min * 60000UL;
  }
  if (o.containsKey("circActive")) {
    const bool on = (bool)(o["circActive"] | false);
    s_forceCirc = on;
    if (!on) {
      relaySet(rid(s_cfg.circ.relayIndex), false);
    }
  }
  return true;
}

bool dhwIsHeatActive() { return s_st.heatActive; }
bool dhwIsPriorityActive() { return s_cfg.disableEquithermDuringHeat && (s_st.heatActive || s_st.heatSequenceActive); }
