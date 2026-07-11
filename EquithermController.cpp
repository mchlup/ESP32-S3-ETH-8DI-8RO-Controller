#include "Features.h"
#include "EquithermController.h"

// Feature flag (optional). In minimal build we include it explicitly.
#ifndef FEATURE_EQUITHERM
#define FEATURE_EQUITHERM 1
#endif

#if defined(FEATURE_EQUITHERM)

#include <math.h>

#include "ConfigStore.h"
#include "TemperatureManager.h"
#include "DhwController.h"
#include "InputController.h"
#include "RelayController.h"
#include "NetworkController.h"
#include "OpenThermController.h"
#include "EventLog.h"
#include "WebPortalController.h"

namespace {
  bool s_inited = false;
  EquithermConfig s_cfg;
  EquithermStatus s_st;
  bool s_externalBlock = false;

  float s_outsideFiltered = NAN;
  uint32_t s_lastComputeMs = 0;
  uint32_t s_lastOutsideFilterMs = 0;

  static constexpr uint32_t kEqControlIntervalMs = 200;
  static constexpr float kOutsideFilterTauMs = 7000.0f;
  static constexpr float kMixTrendStrongCps = 0.020f;
  static constexpr float kMixTrendModerateCps = 0.010f;
  static constexpr uint32_t kMixFeedbackMissingWarnMs = 5000;
  static constexpr uint32_t kMixFeedbackMissingFaultMs = 20000;
  static constexpr uint32_t kMixActuatorRetryHoldMs = 120000;
  // A temperature change smaller than this is treated as sensor noise. There is
  // only one automatic post-pulse evaluation window and its duration is always
  // the configured mixMinIntervalMs; no hidden fixed 2.5/7.5 s timer exists.
  static constexpr float kMixExpectedTempDeltaC = 0.12f;
  static constexpr uint8_t kMixIneffectivePulseThreshold = 3;
  static constexpr int8_t kMixDirectionA = +1; // R1: toward A = hot accumulator branch, AB temperature rises, 100 %
  static constexpr int8_t kMixDirectionB = -1; // R2: toward B = return/cool branch, AB temperature falls, 0 %
  static constexpr uint8_t kMixHeatRelayIndex = 0;
  static constexpr uint8_t kMixCoolRelayIndex = 1;
  static constexpr uint8_t kDefaultNightRelayIndex = 5; // R6, mimo pevně vyhrazená R1/R2

  static inline bool isMixRelayIndex(uint8_t idx) {
    return idx == kMixHeatRelayIndex || idx == kMixCoolRelayIndex;
  }

  float s_lastMixFeedbackC = NAN;
  uint32_t s_lastMixFeedbackMs = 0;
  float s_mixFeedbackTrendCps = 0.0f;
  uint32_t s_mixFeedbackMissingSinceMs = 0;
  uint32_t s_mixLastFeedbackOkMs = 0;
  bool s_mixFeedbackFaultLatched = false;
  uint8_t s_mixIneffectivePulseCount = 0;
  uint32_t s_mixActuatorRetryAfterMs = 0;
  uint32_t s_mixManualHoldUntilMs = 0;

  // Automatic recalibration is considered only once at the beginning of an
  // actual heating cycle and before the first normal regulation pulse. It is
  // never started in summer, while heating is blocked, or during regulation.
  bool s_heatingCycleActive = false;
  bool s_autoCalibrationAttemptedThisCycle = false;
  bool s_regularRegulationStartedThisCycle = false;

  // Runtime latch for accumulator support. The latch adds hysteresis to the
  // decision whether the tank is hot enough and prevents the post-target action
  // from being repeatedly restarted on every 200 ms control cycle.
  bool s_accumulatorSupportActive = false;
  bool s_accumulatorSupportTargetReached = false;
  uint32_t s_accumulatorSupportTargetCandidateSinceMs = 0;
  float s_accumulatorSupportLatchedTargetC = NAN;
  bool s_accumulatorSupportAwaitingSettle = false;
  float s_accumulatorSupportStableRefC = NAN;
  uint32_t s_accumulatorSupportStableSinceMs = 0;

  // Target-band confirmation must be backed by a measurable hydraulic response
  // to an automatic support pulse. Without this guard, the OpenTherm CH value
  // can already lie inside the target band even when the valve is stuck, which
  // would falsely trigger the configured post-target action (for example return A).
  bool s_accumulatorSupportPulseResponseRequired = false;
  bool s_accumulatorSupportPulseResponseObserved = false;
  bool s_accumulatorSupportHadEffectivePulse = false;
  int8_t s_accumulatorSupportLastPulseDir = 0;
  float s_accumulatorSupportPulseStartC = NAN;

  static void resetAccumulatorSupportSettleTracking() {
    s_accumulatorSupportAwaitingSettle = false;
    s_accumulatorSupportStableRefC = NAN;
    s_accumulatorSupportStableSinceMs = 0;
  }

  static void resetAccumulatorSupportResponseTracking(bool resetEffectiveHistory = true) {
    s_accumulatorSupportPulseResponseRequired = false;
    s_accumulatorSupportPulseResponseObserved = false;
    s_accumulatorSupportLastPulseDir = 0;
    s_accumulatorSupportPulseStartC = NAN;
    if (resetEffectiveHistory) s_accumulatorSupportHadEffectivePulse = false;
  }

  static void resetAccumulatorSupportTracking() {
    s_accumulatorSupportActive = false;
    s_accumulatorSupportTargetReached = false;
    s_accumulatorSupportTargetCandidateSinceMs = 0;
    s_accumulatorSupportLatchedTargetC = NAN;
    resetAccumulatorSupportSettleTracking();
    resetAccumulatorSupportResponseTracking(true);
  }

  static void resetHeatingCycleState() {
    s_heatingCycleActive = false;
    s_autoCalibrationAttemptedThisCycle = false;
    s_regularRegulationStartedThisCycle = false;
  }

  static void resetMixFeedbackTracking() {
    s_lastMixFeedbackC = NAN;
    s_lastMixFeedbackMs = 0;
    s_mixFeedbackTrendCps = 0.0f;
    s_mixFeedbackMissingSinceMs = 0;
    s_mixLastFeedbackOkMs = 0;
    s_mixFeedbackFaultLatched = false;
    s_mixIneffectivePulseCount = 0;
    s_mixActuatorRetryAfterMs = 0;
    resetHeatingCycleState();
  }

  static inline void clampFloat(float& v, float lo, float hi) {
    if (!isfinite(v)) return;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
  }

  static inline float lerpCurve(const EquithermCurve& c, float outsideC) {
    // Linear interpolation/extrapolation between (outCold, flowCold) and (outWarm, flowWarm)
    const float dx = (c.outColdC - c.outWarmC);
    if (fabsf(dx) < 0.001f) return c.flowColdC;
    const float k = (c.flowColdC - c.flowWarmC) / dx;
    return c.flowWarmC + k * (outsideC - c.outWarmC);
  }

  static inline String srcName(TempSource s) {
    switch (s) {
      case TempSource::OpenTherm: return "opentherm";
      case TempSource::Dallas: return "dallas";
      case TempSource::Ble: return "ble";
      default: return "none";
    }
  }

  static inline uint16_t parseHmToMin(const String& s, bool& ok) {
    ok = false;
    int colon = s.indexOf(':');
    if (colon < 0) return 0;
    int hh = s.substring(0, colon).toInt();
    int mm = s.substring(colon + 1).toInt();
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return 0;
    ok = true;
    return (uint16_t)(hh * 60 + mm);
  }

  static inline String minToHm(uint16_t m) {
    if (m > 1439) m = 1439;
    char buf[6];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(m / 60), (unsigned)(m % 60));
    return String(buf);
  }

  static void loadFromPrefs() {
    s_cfg.enabled = ConfigStore::getEqEnabled();
    s_cfg.mode = ConfigStore::getEqMode();
    s_cfg.useIn1NightOverride = ConfigStore::getEqUseIn1NightOverride();
    s_cfg.summerModeEnabled = ConfigStore::getEqSummerModeEnabled();
    s_cfg.summerOffAboveC = ConfigStore::getEqSummerOffAboveC();
    s_cfg.summerOnBelowC = ConfigStore::getEqSummerOnBelowC();
    s_cfg.scheduleEnabled = ConfigStore::getEqScheduleEnabled();
    uint16_t starts[7][HEATING_MAX_INTERVALS_PER_DAY] = {};
    uint16_t ends[7][HEATING_MAX_INTERVALS_PER_DAY] = {};
    ConfigStore::getEqScheduleIntervals(s_cfg.intervalCount, starts, ends);
    for (int d = 0; d < 7; d++) for (int i = 0; i < HEATING_MAX_INTERVALS_PER_DAY; i++) { s_cfg.intervals[d][i].startMin = starts[d][i]; s_cfg.intervals[d][i].endMin = ends[d][i]; }

    s_cfg.day.outColdC = ConfigStore::getEqDayOutColdC();
    s_cfg.day.flowColdC = ConfigStore::getEqDayFlowColdC();
    s_cfg.day.outWarmC = ConfigStore::getEqDayOutWarmC();
    s_cfg.day.flowWarmC = ConfigStore::getEqDayFlowWarmC();

    s_cfg.night.outColdC = ConfigStore::getEqNightOutColdC();
    s_cfg.night.flowColdC = ConfigStore::getEqNightFlowColdC();
    s_cfg.night.outWarmC = ConfigStore::getEqNightOutWarmC();
    s_cfg.night.flowWarmC = ConfigStore::getEqNightFlowWarmC();

    s_cfg.minFlowC = ConfigStore::getEqMinFlowC();
    s_cfg.maxFlowC = ConfigStore::getEqMaxFlowC();
    s_cfg.minChSetpointC = ConfigStore::getEqMinChSetpointC();
    s_cfg.maxChSetpointC = ConfigStore::getEqMaxChSetpointC();

    s_cfg.tempMaxAgeMs = ConfigStore::getEqTempMaxAgeMs();
    s_cfg.minSendIntervalMs = ConfigStore::getEqMinSendIntervalMs();
    s_cfg.minSendDeltaC = ConfigStore::getEqMinSendDeltaC();

    s_cfg.useOpenTherm = ConfigStore::getEqUseOpenTherm();
    s_cfg.applyBoilerMaxCh = ConfigStore::getEqApplyBoilerMaxCh();
    s_cfg.boilerMaxChC = ConfigStore::getEqBoilerMaxChC();

    s_cfg.driveNightRelay = ConfigStore::getEqDriveNightRelay();
    s_cfg.nightRelayIndex = ConfigStore::getEqNightRelayIndex();
    s_cfg.nightRelayOnWhenNight = ConfigStore::getEqNightRelayOnWhenNight();

    // Mixing valve
    s_cfg.mixingEnabled = ConfigStore::getEqMixingEnabled();
    s_cfg.mixOpenRelayIndex = ConfigStore::getEqMixOpenRelayIndex();
    s_cfg.mixCloseRelayIndex = ConfigStore::getEqMixCloseRelayIndex();
    s_cfg.mixDeadbandC = ConfigStore::getEqMixDeadbandC();
    s_cfg.mixTargetOffsetC = ConfigStore::getEqMixTargetOffsetC();
    s_cfg.mixTargetReachedAction = ConfigStore::getEqMixTargetReachedAction();
    s_cfg.mixPulseMs = ConfigStore::getEqMixPulseMs();
    s_cfg.mixMinIntervalMs = ConfigStore::getEqMixMinIntervalMs();
    s_cfg.mixTravelMs = ConfigStore::getEqMixTravelMs();
    s_cfg.mixCalibrationSeatMs = ConfigStore::getEqMixCalibrationSeatMs();
    s_cfg.mixAutoRecalibrationMs = ConfigStore::getEqMixAutoRecalibrationMs();
    s_cfg.mixTempSourceA = TemperatureManager::normalizeSourceKey(ConfigStore::getEqMixTempSourceA(), "tank_mid");
    s_cfg.mixTempSourceB = TemperatureManager::normalizeSourceKey(ConfigStore::getEqMixTempSourceB(), "return_dallas");
    s_cfg.mixTempSourceAB = TemperatureManager::normalizeSourceKey(ConfigStore::getEqMixTempSourceAB(), "opentherm_ch");

    // Boiler assist
    s_cfg.boilerAssistEnabled = ConfigStore::getEqBoilerAssistEnabled();
    s_cfg.boilerAssistDeltaC = ConfigStore::getEqBoilerAssistDeltaC();
    s_cfg.boilerAssistForceChEnable = ConfigStore::getEqBoilerAssistForceChEnable();

    // Clamp basics
    if (s_cfg.mode != "auto" && s_cfg.mode != "day" && s_cfg.mode != "night") s_cfg.mode = "auto";
    clampFloat(s_cfg.minFlowC, 10.0f, 90.0f);
    clampFloat(s_cfg.maxFlowC, 10.0f, 90.0f);
    if (s_cfg.minFlowC > s_cfg.maxFlowC) { float t=s_cfg.minFlowC; s_cfg.minFlowC=s_cfg.maxFlowC; s_cfg.maxFlowC=t; }
    clampFloat(s_cfg.minChSetpointC, 10.0f, 90.0f);
    clampFloat(s_cfg.maxChSetpointC, 10.0f, 90.0f);
    if (s_cfg.minChSetpointC > s_cfg.maxChSetpointC) { float t=s_cfg.minChSetpointC; s_cfg.minChSetpointC=s_cfg.maxChSetpointC; s_cfg.maxChSetpointC=t; }
    if (s_cfg.nightRelayIndex > 7) s_cfg.nightRelayIndex = kDefaultNightRelayIndex;
    // Mixing valve relay mapping is fixed by hardware:
    // R1 heats the after-mix circuit, R2 cools/closes the hot branch.
    s_cfg.mixOpenRelayIndex = kMixHeatRelayIndex;
    s_cfg.mixCloseRelayIndex = kMixCoolRelayIndex;
    if (s_cfg.driveNightRelay && isMixRelayIndex(s_cfg.nightRelayIndex)) {
      s_cfg.nightRelayIndex = kDefaultNightRelayIndex;
    }
    clampFloat(s_cfg.mixDeadbandC, 0.1f, 10.0f);
    clampFloat(s_cfg.mixTargetOffsetC, 0.0f, 20.0f);
    s_cfg.mixTargetReachedAction.trim();
    s_cfg.mixTargetReachedAction.toLowerCase();
    if (s_cfg.mixTargetReachedAction != "return_a" && s_cfg.mixTargetReachedAction != "hold") {
      s_cfg.mixTargetReachedAction = "return_a";
    }
    if (s_cfg.mixPulseMs < 100) s_cfg.mixPulseMs = 100;
    if (s_cfg.mixPulseMs > 10000) s_cfg.mixPulseMs = 10000;
    if (s_cfg.mixMinIntervalMs < 500) s_cfg.mixMinIntervalMs = 500;
    if (s_cfg.mixMinIntervalMs > 60000) s_cfg.mixMinIntervalMs = 60000;
    if (s_cfg.mixTravelMs < 1000) s_cfg.mixTravelMs = 1000;
    if (s_cfg.mixTravelMs > 900000) s_cfg.mixTravelMs = 900000;
    if (s_cfg.mixCalibrationSeatMs < 250) s_cfg.mixCalibrationSeatMs = 250;
    if (s_cfg.mixCalibrationSeatMs > 10000) s_cfg.mixCalibrationSeatMs = 10000;
    relaySetMixingInterlockRelays(s_cfg.mixOpenRelayIndex, s_cfg.mixCloseRelayIndex);
    clampFloat(s_cfg.boilerAssistDeltaC, 0.0f, 30.0f);
  }

  static bool getLocalTimeParts(struct tm& outTm) {
    outTm = {};
    if (!networkIsTimeValid()) return false;
    time_t now = (time_t)networkGetTimeEpoch();
    if (now <= (time_t)1672531200) return false;
    localtime_r(&now, &outTm);
    return true;
  }

  static void computeAndSend();

  static bool isNightBySchedule(bool& usedSchedule, String& outIso) {
    usedSchedule = false;
    outIso = networkGetTimeIso();

    if (!s_cfg.scheduleEnabled) return false;
    struct tm tmv;
    if (!getLocalTimeParts(tmv)) return false;

    // tm_wday: 0=Sun..6=Sat -> convert to Mon..Sun index
    int idx = (tmv.tm_wday == 0) ? 6 : (tmv.tm_wday - 1);
    if (idx < 0) idx = 0;
    if (idx > 6) idx = 6;

    const uint16_t nowMin = (uint16_t)(tmv.tm_hour * 60 + tmv.tm_min);
    bool isDay = false;
    const uint8_t cnt = (s_cfg.intervalCount[idx] > HEATING_MAX_INTERVALS_PER_DAY) ? HEATING_MAX_INTERVALS_PER_DAY : s_cfg.intervalCount[idx];
    for (uint8_t i = 0; i < cnt; i++) {
      const uint16_t dayStart = s_cfg.intervals[idx][i].startMin;
      const uint16_t nightStart = s_cfg.intervals[idx][i].endMin;
      if (dayStart == nightStart) continue;
      bool active = false;
      if (dayStart < nightStart) active = (nowMin >= dayStart && nowMin < nightStart);
      else active = (nowMin >= dayStart || nowMin < nightStart);
      if (active) { isDay = true; break; }
    }

    usedSchedule = true;
    return !isDay;
  }

  static void recomputeNow() {
    equithermInit();
    s_lastComputeMs = millis();
    computeAndSend();
  }

  static String effectiveMode(bool& outScheduleUsed, bool& outIn1Forcing, bool& outTimeValid, String& outTimeIso) {
    outScheduleUsed = false;
    outIn1Forcing = false;
    outTimeValid = networkIsTimeValid();
    outTimeIso = networkGetTimeIso();

    if (s_cfg.mode == "day") return "day";
    if (s_cfg.mode == "night") return "night";

    // auto
    if (s_cfg.useIn1NightOverride && inputGetState(InputId::IN1)) {
      outIn1Forcing = true;
      return "night";
    }

    bool schedUsed = false;
    String iso;
    bool night = isNightBySchedule(schedUsed, iso);
    if (schedUsed) {
      outScheduleUsed = true;
      if (iso.length()) outTimeIso = iso;
      return night ? "night" : "day";
    }

    // fallback
    return "day";
  }

  static bool isSummerActive(float outsideC) {
    static bool s_summerLatched = false;
    if (!s_cfg.summerModeEnabled || !isfinite(outsideC)) { s_summerLatched = false; return false; }
    if (!s_summerLatched) { if (outsideC >= s_cfg.summerOffAboveC) s_summerLatched = true; }
    else if (outsideC <= s_cfg.summerOnBelowC) s_summerLatched = false;
    return s_summerLatched;
  }

  static void driveNightRelay(const String& effMode) {
    if (!s_cfg.driveNightRelay) return;
    if (s_cfg.nightRelayIndex > 7) return;
    const bool night = (effMode == "night");
    const bool on = s_cfg.nightRelayOnWhenNight ? night : !night;
    relaySet((RelayId)s_cfg.nightRelayIndex, on);
  }

  static void resetNightRelayToSafeDay() {
    driveNightRelay("day");
  }

  static bool applyBoilerMaxChIfNeeded(float desiredMax, String& outErr) {
    outErr = "";
    if (!s_cfg.applyBoilerMaxCh) return true;
    if (!isfinite(desiredMax)) return true;

    // Only attempt occasionally.
    static uint32_t lastTryMs = 0;
    const uint32_t now = millis();
    if (now - lastTryMs < 60000) return true;
    lastTryMs = now;

    OpenThermStatusSnapshot ot = openthermGetStatus();
    s_st.boilerMaxChC = ot.maxChSetpointC;
    s_st.boilerMaxBoundMinC = ot.maxChBoundMinC;
    s_st.boilerMaxBoundMaxC = ot.maxChBoundMaxC;

    // Respect boiler-reported writable bounds before comparing or writing.
    // Without this, a UI-configured value above the boiler maximum (for example
    // 60 °C while the boiler exposes 22..40 °C) would trigger a needless ID57
    // write attempt on every retry interval even though the effective value can
    // never exceed the reported upper bound.
    float effectiveDesiredMax = desiredMax;
    const float boundLo = isfinite(ot.maxChBoundMinC) ? ot.maxChBoundMinC : 10.0f;
    const float boundHi = isfinite(ot.maxChBoundMaxC) ? ot.maxChBoundMaxC : 90.0f;
    clampFloat(effectiveDesiredMax, boundLo, boundHi);

    // If current max is already at or above the effective desired value
    // (with a small tolerance), skip the write entirely.
    if (isfinite(ot.maxChSetpointC) && ot.maxChSetpointC + 0.25f >= effectiveDesiredMax) return true;

    String err;
    if (!openthermSetMaxChSetpointC(effectiveDesiredMax, err)) {
      outErr = err;
      return false;
    }
    return true;
  }

  static bool sendChSetpoint(float boilerSetpointC, bool forceChEnable, String& outErr) {
    outErr = "";
    if (!s_cfg.useOpenTherm) { outErr = "openTherm disabled"; return false; }

    OpenThermSourceRequest req;
    req.active = true;
    req.chSetpointSet = true;
    req.chSetpointC = boilerSetpointC;
    if (forceChEnable) {
      req.chEnableSet = true;
      req.chEnable = true;
    }

    if (!openthermSetEquithermRequest(req, outErr)) return false;
    return true;
  }

  struct MixPulse {
    bool active = false;
    bool manual = false;
    int8_t dir = 0; // +1=open, -1=close
    uint32_t startedMs = 0;
    uint32_t untilMs = 0;
    uint32_t requestedMs = 0;
    uint32_t lastActMs = 0;
    uint32_t lastPulseMs = 0;
    float positionPct = 50.0f;
    bool forceEndPosition = false;
    bool autoCalibration = false;
    bool postTargetMove = false;
    float endPositionPct = NAN;
    bool positionTrusted = false;
    bool lastRelayApplyOk = true;
    uint8_t lastRelayMask = 0;
    uint32_t lastCalibrationMs = 0;
    int8_t lastCalibrationDir = 0;
    bool feedbackAtStartValid = false;
    float feedbackAtStartC = NAN;
  };

  static MixPulse s_mix;

  static bool mixFeedbackRecent(uint32_t now) {
    return isfinite(s_lastMixFeedbackC)
        && s_lastMixFeedbackMs != 0
        && now >= s_lastMixFeedbackMs
        && (now - s_lastMixFeedbackMs) <= s_cfg.tempMaxAgeMs;
  }

  static bool mixFeedbackMissingFaultActive(uint32_t now) {
    return s_mixFeedbackFaultLatched
        || (s_mixFeedbackMissingSinceMs != 0 && now >= s_mixFeedbackMissingSinceMs
            && (now - s_mixFeedbackMissingSinceMs) >= kMixFeedbackMissingFaultMs);
  }

  static bool mixActuatorFaultActive(uint32_t now) {
    return s_mixActuatorRetryAfterMs != 0 && now < s_mixActuatorRetryAfterMs;
  }

  static String mixCalibrationStateText(uint32_t now) {
    if (mixFeedbackMissingFaultActive(now)) return "feedback_missing";
    if (mixActuatorFaultActive(now)) return "actuator_suspect";
    if (!s_mix.positionTrusted) return "untrusted";
    if (mixFeedbackRecent(now) && s_lastMixFeedbackMs != 0 && s_mix.lastCalibrationMs != 0 && s_lastMixFeedbackMs < s_mix.lastCalibrationMs) return "trusted_sensor_stale";
    if (s_cfg.mixAutoRecalibrationMs != 0 && s_mix.lastCalibrationMs != 0
        && (uint32_t)(now - s_mix.lastCalibrationMs) >= s_cfg.mixAutoRecalibrationMs) return "stale";
    if (s_mix.lastCalibrationDir > 0) return "trusted_a";
    if (s_mix.lastCalibrationDir < 0) return "trusted_b";
    return "trusted";
  }

  static bool mixAllOff() {
    uint8_t appliedMask = relayGetMask();
    const bool ok = relaySetMixingDirection(0, &appliedMask);
    s_mix.lastRelayApplyOk = ok;
    s_mix.lastRelayMask = appliedMask;
    return ok;
  }

  static uint32_t mixElapsedMs(uint32_t now) {
    if (!s_mix.active) return 0;
    if (now < s_mix.startedMs) return 0;
    uint32_t elapsed = now - s_mix.startedMs;
    if (s_mix.requestedMs && elapsed > s_mix.requestedMs) elapsed = s_mix.requestedMs;
    return elapsed;
  }

  static void mixApplyPositionDelta(float deltaPct) {
    s_mix.positionPct += deltaPct;
    if (s_mix.positionPct < 0.0f) s_mix.positionPct = 0.0f;
    if (s_mix.positionPct > 100.0f) s_mix.positionPct = 100.0f;
  }

  static bool mixAtMechanicalLimit(int8_t dir) {
    constexpr float kMixLimitEpsPct = 0.5f;
    if (dir > 0) return s_mix.positionPct >= (100.0f - kMixLimitEpsPct);
    if (dir < 0) return s_mix.positionPct <= kMixLimitEpsPct;
    return false;
  }

  static bool mixHasTrustedLimit(int8_t dir) {
    return s_mix.positionTrusted && mixAtMechanicalLimit(dir);
  }

  static uint32_t mixMinIntervalRemainingMs(uint32_t now) {
    if (s_mix.lastActMs == 0) return 0;
    const uint32_t elapsed = now - s_mix.lastActMs;
    if (elapsed >= s_cfg.mixMinIntervalMs) return 0;
    return s_cfg.mixMinIntervalMs - elapsed;
  }

  static float updateMixFeedbackTrend(float mixFeedbackC, bool valid, uint32_t now) {
    if (!valid || !isfinite(mixFeedbackC)) {
      if (s_mixFeedbackMissingSinceMs == 0) s_mixFeedbackMissingSinceMs = now;
      s_lastMixFeedbackC = NAN;
      s_lastMixFeedbackMs = 0;
      s_mixFeedbackTrendCps = 0.0f;
      return 0.0f;
    }
    s_mixLastFeedbackOkMs = now;
    s_mixFeedbackMissingSinceMs = 0;
    s_mixFeedbackFaultLatched = false;
    if (!isfinite(s_lastMixFeedbackC) || s_lastMixFeedbackMs == 0 || now <= s_lastMixFeedbackMs) {
      s_lastMixFeedbackC = mixFeedbackC;
      s_lastMixFeedbackMs = now;
      s_mixFeedbackTrendCps = 0.0f;
      return 0.0f;
    }

    const uint32_t dtMs = now - s_lastMixFeedbackMs;
    if (dtMs < 800) return s_mixFeedbackTrendCps;

    const float instTrendCps = (mixFeedbackC - s_lastMixFeedbackC) / ((float)dtMs / 1000.0f);
    s_lastMixFeedbackC = mixFeedbackC;
    s_lastMixFeedbackMs = now;

    const float alpha = 0.35f;
    s_mixFeedbackTrendCps = (1.0f - alpha) * s_mixFeedbackTrendCps + alpha * instTrendCps;
    return s_mixFeedbackTrendCps;
  }

  static uint32_t mixAdaptivePulseMs(float targetFlowC, float mixFeedbackC, float trendCps) {
    float absErrC = fabsf(targetFlowC - mixFeedbackC);
    uint32_t pulseMs = s_cfg.mixPulseMs;

    if (absErrC <= s_cfg.mixDeadbandC * 1.5f) pulseMs = (uint32_t)lroundf((float)s_cfg.mixPulseMs * 0.35f);
    else if (absErrC <= s_cfg.mixDeadbandC * 3.0f) pulseMs = (uint32_t)lroundf((float)s_cfg.mixPulseMs * 0.60f);
    else if (absErrC >= 8.0f) pulseMs = (uint32_t)lroundf((float)s_cfg.mixPulseMs * 1.75f);
    else if (absErrC >= 4.0f) pulseMs = (uint32_t)lroundf((float)s_cfg.mixPulseMs * 1.35f);

    const bool warmingTowardTarget = (targetFlowC > mixFeedbackC) && (trendCps > kMixTrendModerateCps);
    const bool coolingTowardTarget = (targetFlowC < mixFeedbackC) && (trendCps < -kMixTrendModerateCps);
    if (warmingTowardTarget || coolingTowardTarget) {
      pulseMs = (uint32_t)lroundf((float)pulseMs * 0.70f);
    }

    const uint32_t minPulseMs = 120;
    uint32_t maxPulseMs = 60000;
    if (s_cfg.mixTravelMs > 0) {
      maxPulseMs = s_cfg.mixTravelMs / 4;
      if (maxPulseMs < minPulseMs) maxPulseMs = minPulseMs;
      if (maxPulseMs > 60000) maxPulseMs = 60000;
    }
    if (pulseMs < minPulseMs) pulseMs = minPulseMs;
    if (pulseMs > maxPulseMs) pulseMs = maxPulseMs;
    return pulseMs;
  }

  static bool mixShouldHoldForTrend(float errorC, float trendCps) {
    const float absErrC = fabsf(errorC);
    float holdWindowC = s_cfg.mixDeadbandC * 4.0f;
    if (holdWindowC < 1.5f) holdWindowC = 1.5f;
    if (absErrC > holdWindowC) return false;
    if (errorC > 0.0f && trendCps > kMixTrendStrongCps) return true;
    if (errorC < 0.0f && trendCps < -kMixTrendStrongCps) return true;
    return false;
  }

  static void mixInvalidateCalibration(bool recenter);

  static void mixMarkCalibrated(float posPct, int8_t dir) {
    s_mix.positionPct = posPct;
    if (s_mix.positionPct < 0.0f) s_mix.positionPct = 0.0f;
    if (s_mix.positionPct > 100.0f) s_mix.positionPct = 100.0f;
    s_mix.positionTrusted = true;
    s_mix.lastCalibrationMs = millis();
    s_mix.lastCalibrationDir = dir;
    s_mixActuatorRetryAfterMs = 0;
    s_mixIneffectivePulseCount = 0;
  }

  static void mixInvalidateCalibration(bool recenter = false) {
    s_mix.positionTrusted = false;
    s_mix.lastCalibrationDir = 0;
    if (recenter) s_mix.positionPct = 50.0f;
  }

  static void mixFinalizePulse(uint32_t now) {
    if (!s_mix.active) return;
    const int8_t pulseDir = s_mix.dir;
    const bool pulseManual = s_mix.manual;
    const bool pulseForceEndPosition = s_mix.forceEndPosition;
    const bool pulseAutoCalibration = s_mix.autoCalibration;
    const bool pulsePostTargetMove = s_mix.postTargetMove;
    const bool pulseFeedbackValid = s_mix.feedbackAtStartValid;
    const float pulseStartFeedbackC = s_mix.feedbackAtStartC;
    const uint32_t elapsedMs = mixElapsedMs(now);

    // The pulse is complete only after both direction relays have been verified
    // OFF. Even if that verification fails, clear the software pulse state so a
    // later retry can only target the fail-safe OFF mask.
    (void)mixAllOff();
    s_mix.lastPulseMs = elapsedMs;
    s_mix.lastActMs = now; // minimum interval is measured from relay OFF

    const float stepPct = (s_cfg.mixTravelMs > 0)
      ? (100.0f * ((float)elapsedMs / (float)s_cfg.mixTravelMs))
      : 0.0f;
    if (pulseForceEndPosition) {
      mixMarkCalibrated(s_mix.endPositionPct, pulseDir);
    } else {
      mixApplyPositionDelta(pulseDir > 0 ? stepPct : -stepPct);
    }

    s_mix.active = false;
    s_mix.manual = false;
    s_mix.dir = 0;
    s_mix.startedMs = 0;
    s_mix.untilMs = 0;
    s_mix.requestedMs = 0;
    s_mix.forceEndPosition = false;
    s_mix.autoCalibration = false;
    s_mix.postTargetMove = false;
    s_mix.feedbackAtStartValid = false;
    s_mix.feedbackAtStartC = NAN;

    // One and only one automatic post-pulse state machine is used for support.
    // It waits until AB has been stable for the full configured minimum interval,
    // detects response in the commanded direction and then either confirms the
    // target or permits the next pulse. End moves/calibration are terminal and
    // intentionally do not enter this evaluator.
    if (!pulseManual && !pulseForceEndPosition && !pulseAutoCalibration
        && !pulsePostTargetMove && s_accumulatorSupportActive) {
      s_accumulatorSupportAwaitingSettle = true;
      s_accumulatorSupportStableRefC = mixFeedbackRecent(now) ? s_lastMixFeedbackC : NAN;
      s_accumulatorSupportStableSinceMs = now;
      s_accumulatorSupportTargetReached = false;
      s_accumulatorSupportTargetCandidateSinceMs = 0;
      s_accumulatorSupportPulseResponseRequired = true;
      s_accumulatorSupportPulseResponseObserved = false;
      s_accumulatorSupportLastPulseDir = pulseDir;
      s_accumulatorSupportPulseStartC = pulseFeedbackValid
        ? pulseStartFeedbackC
        : s_accumulatorSupportStableRefC;
    }
  }

  static void stopMixingNow(uint32_t now = millis(), bool updatePosition = true) {
    if (s_mix.active) {
      if (updatePosition) {
        mixFinalizePulse(now);
        return;
      }
      (void)mixAllOff();
      s_mix.lastPulseMs = mixElapsedMs(now);
      s_mix.lastActMs = now;
    } else {
      (void)mixAllOff();
    }
    s_mix.active = false;
    s_mix.manual = false;
    s_mix.dir = 0;
    s_mix.startedMs = 0;
    s_mix.untilMs = 0;
    s_mix.requestedMs = 0;
    s_mix.forceEndPosition = false;
    s_mix.autoCalibration = false;
    s_mix.postTargetMove = false;
    s_mix.feedbackAtStartValid = false;
    s_mix.feedbackAtStartC = NAN;
  }

  static bool mixStartPulse(int8_t dir, uint32_t now, bool manual = false,
                            uint32_t pulseMsOverride = 0, bool ignoreMinInterval = false) {
    if (dir != kMixDirectionA && dir != kMixDirectionB) return false;
    if (!manual && !s_cfg.mixingEnabled) return false;
    if (!manual && mixHasTrustedLimit(dir)) return false;
    if (!manual && mixFeedbackMissingFaultActive(now)) return false;
    if (!manual && mixActuatorFaultActive(now)) return false;

    uint32_t pulseMs = pulseMsOverride ? pulseMsOverride : s_cfg.mixPulseMs;
    if (pulseMs < 50) pulseMs = 50;
    if (pulseMs > 60000) pulseMs = 60000;
    if (s_mix.active) stopMixingNow(now, true);

    s_mix.forceEndPosition = false;
    s_mix.autoCalibration = false;
    s_mix.postTargetMove = false;
    if (!ignoreMinInterval && s_mix.lastActMs != 0
        && (now - s_mix.lastActMs) < s_cfg.mixMinIntervalMs) return false;

    // Physical meaning is fixed and shared by UI/backend:
    // A/+1/R1 = hot accumulator branch, raises AB, estimated position 100 %.
    // B/-1/R2 = return/cool branch, lowers AB, estimated position 0 %.
    uint8_t appliedMask = relayGetMask();
    const bool relayApplied = relaySetMixingDirection(dir, &appliedMask);
    s_mix.lastRelayApplyOk = relayApplied;
    s_mix.lastRelayMask = appliedMask;
    if (!relayApplied) return false;

    s_mix.active = true;
    s_mix.manual = manual;
    s_mix.dir = dir;
    s_mix.startedMs = now;
    s_mix.requestedMs = pulseMs;
    s_mix.untilMs = now + pulseMs;
    s_mix.lastActMs = now;
    s_mix.feedbackAtStartValid = mixFeedbackRecent(now);
    s_mix.feedbackAtStartC = s_mix.feedbackAtStartValid ? s_lastMixFeedbackC : NAN;
    if (manual) {
      s_mixActuatorRetryAfterMs = 0;
      s_mixManualHoldUntilMs = now + pulseMs + 30000UL;
      // A service action is authoritative for the current heating cycle. Never
      // follow it with an unsolicited automatic homing move.
      s_autoCalibrationAttemptedThisCycle = true;
    } else {
      s_regularRegulationStartedThisCycle = true;
    }
    return true;
  }

  static void mixUpdate(uint32_t now) {
    if (!s_cfg.mixingEnabled && !s_mix.manual) {
      if (s_mix.active) stopMixingNow(now, true);
      return;
    }
    if (s_mix.active && (int32_t)(now - s_mix.untilMs) >= 0) mixFinalizePulse(now);
  }

  static bool mixAutoRecalibrationDue(uint32_t now) {
    if (s_cfg.mixAutoRecalibrationMs == 0) return false; // 0 h means really OFF
    if (!s_cfg.mixingEnabled) return false;
    if (!s_heatingCycleActive || s_autoCalibrationAttemptedThisCycle
        || s_regularRegulationStartedThisCycle) return false;
    if (s_mix.active || s_mix.manual) return false;
    if (mixFeedbackMissingFaultActive(now) || mixActuatorFaultActive(now)) return false;
    if (!s_mix.positionTrusted || s_mix.lastCalibrationMs == 0) return true;
    return (uint32_t)(now - s_mix.lastCalibrationMs) >= s_cfg.mixAutoRecalibrationMs;
  }

  static bool mixStartAutoCalibration(uint32_t now) {
    // One attempt per valid heating cycle. Mark the attempt before touching the
    // relay so an I2C fault cannot cause rapid retry hammering from compute().
    s_autoCalibrationAttemptedThisCycle = true;

    // If the controller already has a trusted B limit, there is no reason to
    // energize the actuator again. Refresh only the calibration timestamp.
    if (mixHasTrustedLimit(kMixDirectionB)) {
      mixMarkCalibrated(0.0f, kMixDirectionB);
      return true;
    }

    const uint32_t seatMs = s_cfg.mixTravelMs + s_cfg.mixCalibrationSeatMs;
    if (!mixStartPulse(kMixDirectionB, now, false, seatMs, true)) return false;
    s_regularRegulationStartedThisCycle = false; // this is pre-regulation homing
    s_mix.forceEndPosition = true;
    s_mix.autoCalibration = true;
    s_mix.endPositionPct = 0.0f;
    return true;
  }

  static bool mixStartAutomaticEndMove(int8_t dir, uint32_t now) {
    if (dir != kMixDirectionA && dir != kMixDirectionB) return false;
    const uint32_t seatMs = s_cfg.mixTravelMs + s_cfg.mixCalibrationSeatMs;
    if (!mixStartPulse(dir, now, false, seatMs, false)) return false;
    s_mix.forceEndPosition = true;
    s_mix.postTargetMove = true;
    s_mix.endPositionPct = (dir == kMixDirectionA) ? 100.0f : 0.0f;
    return true;
  }

  static void fillManualMixStatus(uint32_t now, const char* reason) {
    s_st.active = false;
    s_st.reason = reason;
    s_st.mixState = (s_mix.dir > 0) ? "manual_open" : "manual_close";
    s_st.mixPulsing = s_mix.active;
    s_st.mixManual = true;
    s_st.mixManualDir = (s_mix.dir > 0) ? "A" : "B";
    s_st.mixLastActMs = s_mix.lastActMs;
    s_st.mixPulseReqMs = s_mix.requestedMs;
    const uint32_t elapsedMs = mixElapsedMs(now);
    s_st.mixPulseElapsedMs = elapsedMs;
    s_st.mixPulseRemainingMs = (s_mix.requestedMs > elapsedMs) ? (s_mix.requestedMs - elapsedMs) : 0;
    s_st.mixLastPulseMs = s_mix.lastPulseMs;
    s_st.mixPositionPct = s_mix.positionPct;
    s_st.mixPositionTrusted = s_mix.positionTrusted;
    s_st.mixLastCalibrationMs = s_mix.lastCalibrationMs;
    s_st.mixCalibrationState = mixCalibrationStateText(now);
  }

  static void computeAndSend() {
    // Preserve last send state (static)
    static float lastSent = NAN;
    static uint32_t lastSentMs = 0;
    static String lastErr;
    static bool lastOk = false;
    static String lastEffMode = "";
    static bool lastSupportActive = false;
    static bool lastSupportStateKnown = false;

    // Update runtime snapshot
    s_st = EquithermStatus{};
    s_st.enabled = s_cfg.enabled;
    s_st.modeReq = s_cfg.mode;
    s_st.timeValid = networkIsTimeValid();
    s_st.timeIso = networkGetTimeIso();

    s_st.lastSentChC = lastSent;
    s_st.lastSendMs = lastSentMs;
    s_st.lastSendErr = lastErr;
    s_st.lastSendOk = lastOk;
    s_st.in1Active = inputGetState(InputId::IN1);
    s_st.mixPulsing = s_mix.active;
    s_st.mixManual = s_mix.manual;
    s_st.mixManualDir = s_mix.manual ? String((s_mix.dir > 0) ? "A" : "B") : String("");
    s_st.mixLastActMs = s_mix.lastActMs;
    s_st.mixPulseReqMs = s_mix.requestedMs;
    {
      const uint32_t mixNow = millis();
      const uint32_t elapsedMs = mixElapsedMs(mixNow);
      s_st.mixPulseElapsedMs = elapsedMs;
      s_st.mixPulseRemainingMs = (s_mix.active && s_mix.requestedMs > elapsedMs) ? (s_mix.requestedMs - elapsedMs) : 0;
    }
    s_st.mixLastPulseMs = s_mix.lastPulseMs;
    s_st.mixPositionPct = s_mix.positionPct;
    s_st.mixPositionTrusted = s_mix.positionTrusted;
    s_st.mixLastCalibrationMs = s_mix.lastCalibrationMs;
    s_st.mixCalibrationState = mixCalibrationStateText(millis());
    s_st.mixRelayApplyOk = s_mix.lastRelayApplyOk;
    s_st.mixRelayMask = s_mix.lastRelayMask;

    // Read all three hydraulic ports even when automatic equitherm control is
    // disabled, so the Thermometers and Mixing pages remain useful for service.
    const TempValue mixATv = TemperatureManager::getBySourceKey(s_cfg.mixTempSourceA, s_cfg.tempMaxAgeMs);
    const TempValue mixBTv = TemperatureManager::getBySourceKey(s_cfg.mixTempSourceB, s_cfg.tempMaxAgeMs);
    const TempValue mixABTv = TemperatureManager::getBySourceKey(s_cfg.mixTempSourceAB, s_cfg.tempMaxAgeMs);
    s_st.mixTempASelected = s_cfg.mixTempSourceA;
    s_st.mixTempBSelected = s_cfg.mixTempSourceB;
    s_st.mixTempABSelected = s_cfg.mixTempSourceAB;
    s_st.mixTempAC = (mixATv.valid && isfinite(mixATv.c)) ? mixATv.c : NAN;
    s_st.mixTempAAgeMs = mixATv.valid ? mixATv.ageMs : 0;
    s_st.mixTempASrc = mixATv.valid ? srcName(mixATv.src) : "none";
    s_st.mixTempBC = (mixBTv.valid && isfinite(mixBTv.c)) ? mixBTv.c : NAN;
    s_st.mixTempBAgeMs = mixBTv.valid ? mixBTv.ageMs : 0;
    s_st.mixTempBSrc = mixBTv.valid ? srcName(mixBTv.src) : "none";
    s_st.mixTempABC = (mixABTv.valid && isfinite(mixABTv.c)) ? mixABTv.c : NAN;
    s_st.mixTempABAgeMs = mixABTv.valid ? mixABTv.ageMs : 0;
    s_st.mixTempABSrc = mixABTv.valid ? srcName(mixABTv.src) : "none";
    // Backward-compatible feedback fields always mirror hydraulic port AB.
    s_st.mixFeedbackC = s_st.mixTempABC;
    s_st.mixFeedbackAgeMs = s_st.mixTempABAgeMs;
    s_st.mixFeedbackSrc = s_st.mixTempABSrc;

    if (!s_cfg.enabled) {
      if (s_mix.active && s_mix.manual) {
        fillManualMixStatus(millis(), "manual_pulse");
        return;
      }
      stopMixingNow(millis(), true);
      resetAccumulatorSupportTracking();
      resetHeatingCycleState();
      resetNightRelayToSafeDay();
      s_st.active = false;
      s_st.reason = "disabled";
      openthermClearEquithermRequest();
      return;
    }
    if (s_externalBlock || dhwIsPriorityActive()) {
      stopMixingNow(millis(), true);
      resetAccumulatorSupportTracking();
      resetHeatingCycleState();
      resetNightRelayToSafeDay();
      s_st.active = false;
      s_st.reason = "blocked_dhw";
      openthermClearEquithermRequest();
      return;
    }

    // Need OpenTherm present if we want to control via OT.
    OpenThermStatusSnapshot ot{};
    if (s_cfg.useOpenTherm) {
      ot = openthermGetStatus();
      if (!ot.present || !ot.ready) {
        if (s_mix.active && s_mix.manual) {
          fillManualMixStatus(millis(), "manual_pulse");
          return;
        }
        stopMixingNow(millis(), true);
        resetAccumulatorSupportTracking();
        resetHeatingCycleState();
        resetNightRelayToSafeDay();
        s_st.active = false;
        s_st.reason = "opentherm not ready";
        openthermClearEquithermRequest();
        return;
      }
    }
    bool schedUsed=false, in1=false, timeValid=false;
    String iso;
    const String eff = effectiveMode(schedUsed, in1, timeValid, iso);
    s_st.modeEff = eff;
    s_st.scheduleUsed = schedUsed;
    s_st.in1ForcingNight = in1;
    s_st.timeValid = timeValid;
    if (iso.length()) s_st.timeIso = iso;

    // Drive optional relay (day/night).
    driveNightRelay(eff);

    if (s_mix.active && s_mix.manual) {
      fillManualMixStatus(millis(), "manual_pulse");
      return;
    }

    const uint32_t nowMs = millis();

    // Temperatures: use central TemperatureManager so all fallbacks are respected.
    const TempValue outsideTv = TemperatureManager::get(TempRole::Outside, s_cfg.tempMaxAgeMs);
    if (!outsideTv.valid || !isfinite(outsideTv.c)) {
      stopMixingNow(millis(), true);
      resetAccumulatorSupportTracking();
      resetHeatingCycleState();
      resetNightRelayToSafeDay();
      s_st.active = false;
      s_st.reason = "outside temp missing";
      openthermClearEquithermRequest();
      return;
    }
    const float outsideC = outsideTv.c;
    s_st.outsideC = outsideC;
    s_st.outsideAgeMs = outsideTv.ageMs;
    s_st.outsideSrc = srcName(outsideTv.src);
    s_st.summerActive = isSummerActive(outsideC);
    if (s_st.summerActive) {
      stopMixingNow(nowMs, true);
      resetAccumulatorSupportTracking();
      resetHeatingCycleState();
      resetNightRelayToSafeDay();
      s_st.active = false;
      s_st.reason = "summer_mode";
      s_st.mixState = "blocked_summer";
      openthermClearEquithermRequest();
      return;
    }

    if (!s_heatingCycleActive) {
      s_heatingCycleActive = true;
      s_autoCalibrationAttemptedThisCycle = false;
      s_regularRegulationStartedThisCycle = false;
    }

    const TempValue flowTv = TemperatureManager::get(TempRole::Flow, s_cfg.tempMaxAgeMs);
    const float flowC = (flowTv.valid && isfinite(flowTv.c)) ? flowTv.c : NAN;
    s_st.flowC = flowC;
    s_st.flowAgeMs = flowTv.valid ? flowTv.ageMs : 0;
    s_st.flowSrc = (flowTv.valid && isfinite(flowTv.c)) ? srcName(flowTv.src) : "none";

    // Port AB is the regulation feedback. In this hydraulic layout it is the
    // boiler-measured CH temperature received over OpenTherm. Ports A and B are
    // diagnostics; B deliberately uses the Dallas-only Return role.
    const bool mixTempValid = mixABTv.valid && isfinite(mixABTv.c);
    const float mixFeedbackC = mixTempValid ? mixABTv.c : NAN;
    const float mixTrendCps = updateMixFeedbackTrend(mixFeedbackC, mixTempValid, nowMs);
    s_st.mixCalibrationState = mixCalibrationStateText(nowMs);

    // Smooth outside a bit to avoid oscillation. Keep the filter time constant
    // stable even when control loop cadence changes, otherwise valve timing would
    // silently depend on the 2 s compute loop.
    if (!isfinite(s_outsideFiltered)) {
      s_outsideFiltered = outsideC;
      s_lastOutsideFilterMs = millis();
    } else {
      const uint32_t nowMs = millis();
      const uint32_t dtMs = (s_lastOutsideFilterMs == 0 || nowMs < s_lastOutsideFilterMs)
        ? kEqControlIntervalMs
        : (nowMs - s_lastOutsideFilterMs);
      s_lastOutsideFilterMs = nowMs;
      const float alpha = 1.0f - expf(-(float)dtMs / kOutsideFilterTauMs);
      s_outsideFiltered = (1.0f - alpha) * s_outsideFiltered + alpha * outsideC;
    }

    const EquithermCurve& curve = (eff == "night") ? s_cfg.night : s_cfg.day;
    float baseTargetFlow = lerpCurve(curve, s_outsideFiltered);

    // The equitherm target is the value requested from the boiler over OT.
    // The mixing offset is deliberately NOT applied to this value.
    clampFloat(baseTargetFlow, s_cfg.minFlowC, s_cfg.maxFlowC);
    s_st.targetBaseFlowC = baseTargetFlow;

    float supportTargetFlow = baseTargetFlow + s_cfg.mixTargetOffsetC;
    clampFloat(supportTargetFlow, s_cfg.minFlowC, s_cfg.maxFlowC);
    s_st.supportTargetFlowC = supportTargetFlow;

    const bool supportConfigured = s_cfg.boilerAssistEnabled
        && s_cfg.mixingEnabled
        && s_cfg.mixTargetOffsetC > 0.01f;
    const bool tankHotValid = mixATv.valid && isfinite(mixATv.c);
    const bool supportAvailableNow = supportConfigured
        && tankHotValid
        && mixATv.c >= supportTargetFlow;
    const bool supportWasActive = s_accumulatorSupportActive;
    const float supportDropThresholdC = supportTargetFlow - s_cfg.mixDeadbandC;

    // Start support only when port A can actually supply the increased target.
    // Keep it active down to target-deadband to avoid rapid on/off toggling.
    if (s_accumulatorSupportActive) {
      if (!supportConfigured || !tankHotValid || mixATv.c < supportDropThresholdC) {
        s_accumulatorSupportActive = false;
      }
    } else if (supportAvailableNow) {
      s_accumulatorSupportActive = true;
    }

    if (supportWasActive != s_accumulatorSupportActive) {
      s_accumulatorSupportTargetReached = false;
      s_accumulatorSupportTargetCandidateSinceMs = 0;
      s_accumulatorSupportLatchedTargetC = supportTargetFlow;
      resetAccumulatorSupportSettleTracking();
      resetAccumulatorSupportResponseTracking(true);
      if (!s_accumulatorSupportActive && s_mix.active && !s_mix.manual) {
        stopMixingNow(nowMs, false);
      }
    }
    if (s_accumulatorSupportActive
        && (!isfinite(s_accumulatorSupportLatchedTargetC)
            || fabsf(s_accumulatorSupportLatchedTargetC - supportTargetFlow) > 0.1f)) {
      s_accumulatorSupportTargetReached = false;
      s_accumulatorSupportTargetCandidateSinceMs = 0;
      s_accumulatorSupportLatchedTargetC = supportTargetFlow;
      resetAccumulatorSupportResponseTracking(true);
      if (s_accumulatorSupportAwaitingSettle) {
        s_accumulatorSupportStableRefC = mixTempValid ? mixFeedbackC : NAN;
        s_accumulatorSupportStableSinceMs = nowMs;
      }
    }
    if (!s_accumulatorSupportActive) {
      s_accumulatorSupportTargetReached = false;
      s_accumulatorSupportTargetCandidateSinceMs = 0;
      s_accumulatorSupportLatchedTargetC = NAN;
      resetAccumulatorSupportSettleTracking();
      resetAccumulatorSupportResponseTracking(true);
    }

    const float targetFlow = s_accumulatorSupportActive ? supportTargetFlow : baseTargetFlow;
    s_st.targetFlowC = targetFlow;
    s_st.accumulatorSupportConfigured = supportConfigured;
    s_st.accumulatorSupportAvailable = supportAvailableNow;
    s_st.accumulatorSupportActive = s_accumulatorSupportActive;
    s_st.accumulatorSupportAction = s_cfg.mixTargetReachedAction;

    // Automatic recalibration is allowed only at the beginning of a real heating
    // cycle, after summer/DHW/OT guards passed and before any regular valve pulse.
    // A value of 0 h disables this path completely.
    if (mixAutoRecalibrationDue(nowMs)) {
      (void)mixStartAutoCalibration(nowMs);
    }

    const bool automaticMixPulseActive = s_mix.active && !s_mix.manual;
    const bool supportTargetBandReached = s_accumulatorSupportActive
        && mixTempValid
        && fabsf(mixFeedbackC - targetFlow) <= s_cfg.mixDeadbandC;
    const float targetExitBandC = fmaxf(s_cfg.mixDeadbandC + 0.2f,
                                        s_cfg.mixDeadbandC * 1.5f);

    // A completed target is not permanent. If AB later leaves the target band
    // with hysteresis, resume regulation. A post-target end move that has already
    // started is allowed to finish; otherwise its own temperature effect would
    // cancel the requested mechanical return before reaching the limit.
    if (s_accumulatorSupportTargetReached
        && !(s_mix.active && s_mix.postTargetMove)
        && (!mixTempValid || fabsf(mixFeedbackC - targetFlow) > targetExitBandC)) {
      s_accumulatorSupportTargetReached = false;
      s_accumulatorSupportTargetCandidateSinceMs = 0;
      resetAccumulatorSupportSettleTracking();
      // A future target confirmation must be backed by a new effective valve
      // pulse; an old response must not remain valid after leaving the band.
      resetAccumulatorSupportResponseTracking(true);
    }

    // Observe the response of the last automatic support pulse while using the
    // same configured interval that gates the next pulse. No second fixed timer
    // and no automatic recalibration is triggered by an ineffective pulse.
    if (s_accumulatorSupportActive
        && s_accumulatorSupportPulseResponseRequired
        && !s_accumulatorSupportPulseResponseObserved
        && !automaticMixPulseActive
        && mixTempValid
        && isfinite(s_accumulatorSupportPulseStartC)
        && s_accumulatorSupportLastPulseDir != 0) {
      const float responseDeltaC = mixFeedbackC - s_accumulatorSupportPulseStartC;
      const bool movedAsRequested = (s_accumulatorSupportLastPulseDir == kMixDirectionA)
        ? (responseDeltaC >= kMixExpectedTempDeltaC)
        : (responseDeltaC <= -kMixExpectedTempDeltaC);
      if (movedAsRequested) {
        s_accumulatorSupportPulseResponseObserved = true;
        // The directional response is proven immediately. The separate settle
        // gate still blocks another pulse until AB stops changing for the full
        // configured interval, while target confirmation can use the same
        // interval instead of waiting twice.
        s_accumulatorSupportHadEffectivePulse = true;
        s_mixIneffectivePulseCount = 0;
        s_mixActuatorRetryAfterMs = 0;
      }
    }

    bool supportOutputSettled = false;
    if (s_accumulatorSupportActive
        && s_accumulatorSupportAwaitingSettle
        && !automaticMixPulseActive
        && mixTempValid) {
      if (!isfinite(s_accumulatorSupportStableRefC)
          || s_accumulatorSupportStableSinceMs == 0) {
        s_accumulatorSupportStableRefC = mixFeedbackC;
        s_accumulatorSupportStableSinceMs = nowMs;
      } else if (fabsf(mixFeedbackC - s_accumulatorSupportStableRefC)
                 >= kMixExpectedTempDeltaC) {
        s_accumulatorSupportStableRefC = mixFeedbackC;
        s_accumulatorSupportStableSinceMs = nowMs;
      }
      supportOutputSettled = (nowMs - s_accumulatorSupportStableSinceMs)
          >= s_cfg.mixMinIntervalMs;

      if (supportOutputSettled) {
        if (s_accumulatorSupportPulseResponseRequired) {
          if (s_accumulatorSupportPulseResponseObserved) {
            s_accumulatorSupportHadEffectivePulse = true;
            s_mixIneffectivePulseCount = 0;
            s_mixActuatorRetryAfterMs = 0;
          } else {
            if (s_mixIneffectivePulseCount < 255) ++s_mixIneffectivePulseCount;
            if (s_mixIneffectivePulseCount >= kMixIneffectivePulseThreshold) {
              s_mixActuatorRetryAfterMs = nowMs + kMixActuatorRetryHoldMs;
            }
          }
        }
        s_accumulatorSupportAwaitingSettle = false;
        s_accumulatorSupportPulseResponseRequired = false;
        s_accumulatorSupportPulseResponseObserved = false;
        s_accumulatorSupportLastPulseDir = 0;
        s_accumulatorSupportPulseStartC = NAN;
      }
    }

    const bool supportPulseResponseVerified = s_accumulatorSupportPulseResponseRequired
        ? s_accumulatorSupportPulseResponseObserved
        : s_accumulatorSupportHadEffectivePulse;

    if (!s_accumulatorSupportTargetReached) {
      if (!supportTargetBandReached || automaticMixPulseActive
          || !supportPulseResponseVerified) {
        s_accumulatorSupportTargetCandidateSinceMs = 0;
      } else {
        if (s_accumulatorSupportTargetCandidateSinceMs == 0) {
          s_accumulatorSupportTargetCandidateSinceMs = nowMs;
        }
        if ((nowMs - s_accumulatorSupportTargetCandidateSinceMs)
            >= s_cfg.mixMinIntervalMs) {
          s_accumulatorSupportTargetReached = true;
          s_accumulatorSupportTargetCandidateSinceMs = 0;
          resetAccumulatorSupportSettleTracking();
        }
      }
    }

    const bool supportTargetConfirming = s_accumulatorSupportActive
        && !s_accumulatorSupportTargetReached
        && supportTargetBandReached
        && supportPulseResponseVerified
        && !automaticMixPulseActive;
    const bool supportTargetUnverifiedHold = s_accumulatorSupportActive
        && !s_accumulatorSupportTargetReached
        && supportTargetBandReached
        && !supportPulseResponseVerified
        && !automaticMixPulseActive;
    const bool supportWaitingForSettle = s_accumulatorSupportActive
        && s_accumulatorSupportAwaitingSettle
        && !s_accumulatorSupportTargetReached
        && !automaticMixPulseActive;
    s_st.accumulatorSupportTargetReached = s_accumulatorSupportTargetReached;

    // Mixing valve control uses hydraulic port AB. A and B semantics are fixed:
    // A/R1/hot accumulator branch = 100 %, B/R2/return branch = 0 %.
    s_st.mixState = "idle";
    if (s_mix.active && s_mix.autoCalibration) {
      s_st.mixState = "calibrating_b";
    } else if (s_accumulatorSupportActive && s_accumulatorSupportTargetReached) {
      if (s_cfg.mixTargetReachedAction == "hold") {
        if (s_mix.active && !s_mix.manual) stopMixingNow(nowMs, false);
        s_st.mixState = "support_target_hold";
      } else if (mixHasTrustedLimit(kMixDirectionA)) {
        s_st.mixState = "support_target_at_a";
      } else if (s_mix.active) {
        s_st.mixState = "support_target_return_a";
      } else if (mixStartAutomaticEndMove(kMixDirectionA, nowMs)) {
        s_st.mixState = "support_target_return_a";
      } else {
        s_st.mixState = !s_mix.lastRelayApplyOk
          ? "fault_relay_write"
          : (mixActuatorFaultActive(nowMs)
              ? "fault_actuator_suspect"
              : "support_target_return_a_pending");
      }
    } else if (s_mix.active) {
      s_st.mixState = s_mix.manual
        ? ((s_mix.dir == kMixDirectionA) ? "manual_open" : "manual_close")
        : ((s_mix.dir == kMixDirectionA) ? "open" : "close");
    } else if (s_cfg.mixingEnabled) {
      if (!mixTempValid) {
        const uint32_t missingForMs = (s_mixFeedbackMissingSinceMs != 0 && nowMs >= s_mixFeedbackMissingSinceMs)
          ? (nowMs - s_mixFeedbackMissingSinceMs) : 0;
        if (missingForMs >= kMixFeedbackMissingFaultMs) {
          stopMixingNow(nowMs, true);
          mixInvalidateCalibration(false);
          s_mixFeedbackFaultLatched = true;
          s_st.mixState = "fault_no_feedback";
        } else if (missingForMs >= kMixFeedbackMissingWarnMs) {
          s_st.mixState = "no_feedback_wait";
        } else {
          s_st.mixState = "no_feedback";
        }
      } else {
        const float errorC = targetFlow - mixFeedbackC;
        const bool wantsOpen = (mixFeedbackC + s_cfg.mixDeadbandC < targetFlow);
        const bool wantsClose = (mixFeedbackC - s_cfg.mixDeadbandC > targetFlow);
        const uint32_t minIntervalRemainingMs = mixMinIntervalRemainingMs(nowMs);
        const uint32_t adaptivePulseMs = mixAdaptivePulseMs(targetFlow, mixFeedbackC, mixTrendCps);
        const bool holdForTrend = !s_accumulatorSupportActive
            && mixShouldHoldForTrend(errorC, mixTrendCps);
        const bool actuatorFault = mixActuatorFaultActive(nowMs);
        const bool manualHold = s_mixManualHoldUntilMs != 0
            && (int32_t)(s_mixManualHoldUntilMs - nowMs) > 0;

        if (supportTargetConfirming) {
          s_st.mixState = "support_target_confirming";
        } else if (supportTargetUnverifiedHold) {
          // Already inside the requested band: do not create a diagnostic/probe
          // pulse. Hold safely and require a real future regulation response
          // before the post-target action can ever be confirmed.
          s_st.mixState = "support_target_unverified_hold";
        } else if (manualHold) {
          s_st.mixState = "manual_hold";
        } else if (wantsOpen) {
          if (mixFeedbackMissingFaultActive(nowMs)) {
            s_st.mixState = "fault_no_feedback";
          } else if (actuatorFault) {
            s_st.mixState = "fault_actuator_suspect";
          } else if (mixHasTrustedLimit(kMixDirectionA)) {
            s_st.mixState = "limit_a";
          } else if (supportWaitingForSettle) {
            s_st.mixState = "support_wait_settle_open";
          } else if (holdForTrend) {
            s_st.mixState = "settling_open";
          } else if (minIntervalRemainingMs > 0) {
            s_st.mixState = "hold_min_interval_open";
          } else if (mixStartPulse(kMixDirectionA, nowMs, false, adaptivePulseMs)) {
            if (s_accumulatorSupportActive) resetAccumulatorSupportSettleTracking();
            s_st.mixState = "open";
          } else {
            s_st.mixState = !s_mix.lastRelayApplyOk
              ? "fault_relay_write"
              : (actuatorFault ? "fault_actuator_suspect" : "open_pending");
          }
        } else if (wantsClose) {
          if (mixFeedbackMissingFaultActive(nowMs)) {
            s_st.mixState = "fault_no_feedback";
          } else if (actuatorFault) {
            s_st.mixState = "fault_actuator_suspect";
          } else if (mixHasTrustedLimit(kMixDirectionB)) {
            s_st.mixState = "limit_b";
          } else if (supportWaitingForSettle) {
            s_st.mixState = "support_wait_settle_close";
          } else if (holdForTrend) {
            s_st.mixState = "settling_close";
          } else if (minIntervalRemainingMs > 0) {
            s_st.mixState = "hold_min_interval_close";
          } else if (mixStartPulse(kMixDirectionB, nowMs, false, adaptivePulseMs)) {
            if (s_accumulatorSupportActive) resetAccumulatorSupportSettleTracking();
            s_st.mixState = "close";
          } else {
            s_st.mixState = !s_mix.lastRelayApplyOk
              ? "fault_relay_write"
              : (actuatorFault ? "fault_actuator_suspect" : "close_pending");
          }
        } else {
          s_st.mixState = actuatorFault ? "fault_actuator_suspect" : "in_deadband";
        }
      }
    }

    // Refresh runtime output telemetry after this control pass; a pulse may have
    // started or stopped since the snapshot at the beginning of computeAndSend().
    s_st.mixPulsing = s_mix.active;
    s_st.mixManual = s_mix.manual;
    s_st.mixManualDir = s_mix.manual ? String((s_mix.dir == kMixDirectionA) ? "A" : "B") : String("");
    s_st.mixLastActMs = s_mix.lastActMs;
    s_st.mixPulseReqMs = s_mix.requestedMs;
    s_st.mixPulseElapsedMs = mixElapsedMs(nowMs);
    s_st.mixPulseRemainingMs = (s_mix.active && s_mix.requestedMs > s_st.mixPulseElapsedMs)
      ? (s_mix.requestedMs - s_st.mixPulseElapsedMs) : 0;
    s_st.mixLastPulseMs = s_mix.lastPulseMs;
    s_st.mixPositionPct = s_mix.positionPct;
    s_st.mixPositionTrusted = s_mix.positionTrusted;
    s_st.mixLastCalibrationMs = s_mix.lastCalibrationMs;
    s_st.mixCalibrationState = mixCalibrationStateText(nowMs);
    s_st.mixRelayApplyOk = s_mix.lastRelayApplyOk;
    s_st.mixRelayMask = s_mix.lastRelayMask;

    // The boiler always receives the base equitherm target. Accumulator support
    // raises only the valve/support target, e.g. OT=22 °C and valve target=27 °C.
    float boilerSp = baseTargetFlow;
    // Determine effective CH setpoint clamp (prefer OpenTherm limits when available)
    s_st.boilerMaxChC = ot.maxChSetpointC;
    s_st.boilerMaxBoundMinC = ot.maxChBoundMinC;
    s_st.boilerMaxBoundMaxC = ot.maxChBoundMaxC;
    float clampMin = s_cfg.minChSetpointC;
    float clampMax = s_cfg.maxChSetpointC;
    if (isfinite(ot.maxChBoundMinC)) clampMin = fmaxf(clampMin, ot.maxChBoundMinC);
    if (isfinite(ot.maxChBoundMaxC)) clampMax = fminf(clampMax, ot.maxChBoundMaxC);
    if (isfinite(ot.maxChSetpointC)) clampMax = fminf(clampMax, ot.maxChSetpointC);
    if (s_cfg.applyBoilerMaxCh && isfinite(s_cfg.boilerMaxChC)) clampMax = fminf(clampMax, s_cfg.boilerMaxChC);
    if (clampMin > clampMax) clampMin = clampMax;
    s_st.boilerClampMinC = clampMin;
    s_st.boilerClampMaxC = clampMax;

    // Safety clamp for what we send to boiler
    clampFloat(boilerSp, clampMin, clampMax);
    s_st.boilerSetpointC = boilerSp;
    s_st.active = true;

    const uint32_t now = millis();
    const bool intervalOk = (now - lastSentMs) >= s_cfg.minSendIntervalMs;
    bool deltaOk = (!isfinite(lastSent)) || fabsf(boilerSp - lastSent) >= s_cfg.minSendDeltaC;
    const bool modeChanged = (eff.length() && eff != lastEffMode);
    const bool supportStateChanged = !lastSupportStateKnown
        || (s_accumulatorSupportActive != lastSupportActive);
    if (modeChanged || supportStateChanged) deltaOk = true;

    if (!intervalOk && !modeChanged && !supportStateChanged) {
      s_st.reason = "hold_interval";
      return;
    }
    if (!deltaOk) {
      s_st.reason = "hold_delta";
      return;
    }

    // Optionally align boiler Max CH.
    String maxErr;
    if (s_cfg.applyBoilerMaxCh) {
      if (!applyBoilerMaxChIfNeeded(s_cfg.boilerMaxChC, maxErr)) {
        // Not fatal; continue with setpoint write.
        lastErr = String("boilerMax:") + maxErr;
      }
    }

    String err;
    const bool forceCh = (s_cfg.boilerAssistForceChEnable && s_accumulatorSupportActive);
    const bool ok = sendChSetpoint(boilerSp, forceCh, err);

    lastOk = ok;
    lastErr = ok ? "" : err;
    if (ok) {
      lastSent = boilerSp;
      lastSentMs = now;
      lastEffMode = eff;
      lastSupportActive = s_accumulatorSupportActive;
      lastSupportStateKnown = true;
    }

    s_st.lastSentChC = lastSent;
    s_st.lastSendMs = lastSentMs;
    s_st.lastSendErr = lastErr;
    s_st.lastSendOk = lastOk;
    s_st.reason = ok ? "sent" : "send failed";
  }

  static void fillConfigJson(JsonObject out) {
    out["enabled"] = s_cfg.enabled;
    out["mode"] = s_cfg.mode;
    out["useIn1NightOverride"] = s_cfg.useIn1NightOverride;

    JsonObject sched = out.createNestedObject("schedule");
    sched["enabled"] = s_cfg.scheduleEnabled;
    JsonArray week = sched.createNestedArray("week");
    static const char* dayNames[7] = {"mon","tue","wed","thu","fri","sat","sun"};
    for (int i = 0; i < 7; i++) {
      JsonObject d = week.createNestedObject();
      d["day"] = dayNames[i];
      d["intervalCount"] = s_cfg.intervalCount[i];
      const uint16_t firstStart = (s_cfg.intervalCount[i] > 0) ? s_cfg.intervals[i][0].startMin : 360;
      const uint16_t firstEnd = (s_cfg.intervalCount[i] > 0) ? s_cfg.intervals[i][0].endMin : 1320;
      d["dayStart"] = minToHm(firstStart);
      d["nightStart"] = minToHm(firstEnd);
      d["dayStartMin"] = firstStart;
      d["nightStartMin"] = firstEnd;
      JsonArray intervals = d.createNestedArray("intervals");
      for (uint8_t j = 0; j < s_cfg.intervalCount[i] && j < HEATING_MAX_INTERVALS_PER_DAY; j++) {
        JsonObject it = intervals.createNestedObject();
        it["startMin"] = s_cfg.intervals[i][j].startMin;
        it["endMin"] = s_cfg.intervals[i][j].endMin;
        it["start"] = minToHm(s_cfg.intervals[i][j].startMin);
        it["end"] = minToHm(s_cfg.intervals[i][j].endMin);
      }
    }

    JsonObject day = out.createNestedObject("day");
    day["outColdC"] = s_cfg.day.outColdC;
    day["flowColdC"] = s_cfg.day.flowColdC;
    day["outWarmC"] = s_cfg.day.outWarmC;
    day["flowWarmC"] = s_cfg.day.flowWarmC;

    JsonObject night = out.createNestedObject("night");
    night["outColdC"] = s_cfg.night.outColdC;
    night["flowColdC"] = s_cfg.night.flowColdC;
    night["outWarmC"] = s_cfg.night.outWarmC;
    night["flowWarmC"] = s_cfg.night.flowWarmC;

    JsonObject lim = out.createNestedObject("limits");
    lim["minFlowC"] = s_cfg.minFlowC;
    lim["maxFlowC"] = s_cfg.maxFlowC;
    lim["minChSetpointC"] = s_cfg.minChSetpointC;
    lim["maxChSetpointC"] = s_cfg.maxChSetpointC;

    JsonObject temps = out.createNestedObject("temps");
    temps["maxAgeMs"] = s_cfg.tempMaxAgeMs;

    JsonObject send = out.createNestedObject("send");
    send["minIntervalMs"] = s_cfg.minSendIntervalMs;
    send["minDeltaC"] = s_cfg.minSendDeltaC;

    JsonObject outm = out.createNestedObject("output");
    outm["useOpenTherm"] = s_cfg.useOpenTherm;
    outm["applyBoilerMaxCh"] = s_cfg.applyBoilerMaxCh;
    outm["boilerMaxChC"] = s_cfg.boilerMaxChC;
    outm["driveNightRelay"] = s_cfg.driveNightRelay;
    outm["nightRelay"] = (uint32_t)(s_cfg.nightRelayIndex + 1);
    outm["nightRelayOnWhenNight"] = s_cfg.nightRelayOnWhenNight;

    JsonObject mix = out.createNestedObject("mixing");
    mix["enabled"] = s_cfg.mixingEnabled;
    mix["openRelay"] = (uint32_t)(kMixHeatRelayIndex + 1);
    mix["closeRelay"] = (uint32_t)(kMixCoolRelayIndex + 1);
    mix["heatRelay"] = (uint32_t)(kMixHeatRelayIndex + 1);
    mix["coolRelay"] = (uint32_t)(kMixCoolRelayIndex + 1);
    mix["deadbandC"] = s_cfg.mixDeadbandC;
    mix["targetOffsetC"] = s_cfg.mixTargetOffsetC;
    mix["targetReachedAction"] = s_cfg.mixTargetReachedAction;
    mix["pulseMs"] = (uint32_t)s_cfg.mixPulseMs;
    mix["minIntervalMs"] = (uint32_t)s_cfg.mixMinIntervalMs;
    mix["travelMs"] = (uint32_t)s_cfg.mixTravelMs;
    mix["calibrationSeatMs"] = (uint32_t)s_cfg.mixCalibrationSeatMs;
    mix["autoRecalibrationMs"] = (uint32_t)s_cfg.mixAutoRecalibrationMs;
    mix["sourceA"] = s_cfg.mixTempSourceA;
    mix["sourceB"] = s_cfg.mixTempSourceB;
    mix["sourceAB"] = s_cfg.mixTempSourceAB;

    JsonObject ba = out.createNestedObject("boilerAssist");
    ba["enabled"] = s_cfg.boilerAssistEnabled;
    ba["deltaC"] = s_cfg.boilerAssistDeltaC;
    ba["forceChEnable"] = s_cfg.boilerAssistForceChEnable;
  }

  static void fillStatusJson(JsonObject out) {
    out["enabled"] = s_st.enabled;
    out["active"] = s_st.active;
    out["reason"] = s_st.reason;

    JsonObject mode = out.createNestedObject("mode");
    mode["req"] = s_st.modeReq;
    mode["eff"] = s_st.modeEff;
    mode["scheduleUsed"] = s_st.scheduleUsed;
    mode["in1Active"] = s_st.in1Active;
    mode["in1ForcingNight"] = s_st.in1ForcingNight;
    mode["summerActive"] = s_st.summerActive;

    JsonObject time = out.createNestedObject("time");
    time["valid"] = s_st.timeValid;
    if (s_st.timeIso.length()) time["iso"] = s_st.timeIso; else time["iso"] = nullptr;

    JsonObject temps = out.createNestedObject("temps");
    if (isfinite(s_st.outsideC)) temps["outsideC"] = s_st.outsideC; else temps["outsideC"] = nullptr;
    temps["outsideAgeMs"] = (uint32_t)s_st.outsideAgeMs;
    temps["outsideSrc"] = s_st.outsideSrc.length() ? s_st.outsideSrc : "none";

    if (isfinite(s_st.flowC)) temps["flowC"] = s_st.flowC; else temps["flowC"] = nullptr;
    temps["flowAgeMs"] = (uint32_t)s_st.flowAgeMs;
    temps["flowSrc"] = s_st.flowSrc.length() ? s_st.flowSrc : "none";
    if (isfinite(s_st.mixFeedbackC)) temps["mixFeedbackC"] = s_st.mixFeedbackC; else temps["mixFeedbackC"] = nullptr;
    temps["mixFeedbackAgeMs"] = (uint32_t)s_st.mixFeedbackAgeMs;
    temps["mixFeedbackSrc"] = s_st.mixFeedbackSrc.length() ? s_st.mixFeedbackSrc : "none";
    if (isfinite(s_st.mixTempAC)) temps["mixAC"] = s_st.mixTempAC; else temps["mixAC"] = nullptr;
    temps["mixAAgeMs"] = (uint32_t)s_st.mixTempAAgeMs;
    temps["mixASrc"] = s_st.mixTempASrc.length() ? s_st.mixTempASrc : "none";
    temps["mixASelected"] = s_st.mixTempASelected.length() ? s_st.mixTempASelected : "none";
    if (isfinite(s_st.mixTempBC)) temps["mixBC"] = s_st.mixTempBC; else temps["mixBC"] = nullptr;
    temps["mixBAgeMs"] = (uint32_t)s_st.mixTempBAgeMs;
    temps["mixBSrc"] = s_st.mixTempBSrc.length() ? s_st.mixTempBSrc : "none";
    temps["mixBSelected"] = s_st.mixTempBSelected.length() ? s_st.mixTempBSelected : "none";
    if (isfinite(s_st.mixTempABC)) temps["mixABC"] = s_st.mixTempABC; else temps["mixABC"] = nullptr;
    temps["mixABAgeMs"] = (uint32_t)s_st.mixTempABAgeMs;
    temps["mixABSrc"] = s_st.mixTempABSrc.length() ? s_st.mixTempABSrc : "none";
    temps["mixABSelected"] = s_st.mixTempABSelected.length() ? s_st.mixTempABSelected : "none";

    JsonObject outv = out.createNestedObject("out");
    if (isfinite(s_st.targetBaseFlowC)) outv["targetBaseFlowC"] = s_st.targetBaseFlowC; else outv["targetBaseFlowC"] = nullptr;
    if (isfinite(s_st.targetFlowC)) outv["targetFlowC"] = s_st.targetFlowC; else outv["targetFlowC"] = nullptr;
    if (isfinite(s_st.supportTargetFlowC)) outv["supportTargetFlowC"] = s_st.supportTargetFlowC; else outv["supportTargetFlowC"] = nullptr;
    if (isfinite(s_st.boilerSetpointC)) outv["boilerSetpointC"] = s_st.boilerSetpointC; else outv["boilerSetpointC"] = nullptr;
    if (isfinite(s_st.lastSentChC)) outv["lastSentChC"] = s_st.lastSentChC; else outv["lastSentChC"] = nullptr;
    outv["lastSendOk"] = s_st.lastSendOk;
    if (s_st.lastSendErr.length()) outv["lastSendErr"] = s_st.lastSendErr; else outv["lastSendErr"] = nullptr;
    outv["lastSendMs"] = (uint32_t)s_st.lastSendMs;

    JsonObject mix = out.createNestedObject("mix");
    mix["heatRelay"] = (uint32_t)(kMixHeatRelayIndex + 1);
    mix["coolRelay"] = (uint32_t)(kMixCoolRelayIndex + 1);
    mix["state"] = s_st.mixState.length() ? s_st.mixState : "idle";
    mix["pulsing"] = s_st.mixPulsing;
    mix["manual"] = s_st.mixManual;
    if (s_st.mixManualDir.length()) mix["manualDir"] = s_st.mixManualDir; else mix["manualDir"] = nullptr;
    mix["lastActMs"] = (uint32_t)s_st.mixLastActMs;
    mix["pulseReqMs"] = (uint32_t)s_st.mixPulseReqMs;
    mix["pulseElapsedMs"] = (uint32_t)s_st.mixPulseElapsedMs;
    mix["pulseRemainingMs"] = (uint32_t)s_st.mixPulseRemainingMs;
    mix["lastPulseMs"] = (uint32_t)s_st.mixLastPulseMs;
    if (isfinite(s_st.mixPositionPct)) mix["pct"] = s_st.mixPositionPct; else mix["pct"] = nullptr;
    mix["positionTrusted"] = s_st.mixPositionTrusted;
    mix["supportConfigured"] = s_st.accumulatorSupportConfigured;
    mix["supportAvailable"] = s_st.accumulatorSupportAvailable;
    mix["supportActive"] = s_st.accumulatorSupportActive;
    mix["supportTargetReached"] = s_st.accumulatorSupportTargetReached;
    mix["supportAction"] = s_st.accumulatorSupportAction.length() ? s_st.accumulatorSupportAction : s_cfg.mixTargetReachedAction;
    mix["lastCalibrationMs"] = (uint32_t)s_st.mixLastCalibrationMs;
    if (s_st.mixCalibrationState.length()) mix["calibration"] = s_st.mixCalibrationState; else mix["calibration"] = nullptr;
    mix["relayApplied"] = s_st.mixRelayApplyOk;
    mix["relayMask"] = (uint32_t)s_st.mixRelayMask;
    mix["relayOk"] = relayIsOk();

    JsonObject b = out.createNestedObject("boiler");
    if (isfinite(s_st.boilerMaxChC)) b["maxChC"] = s_st.boilerMaxChC; else b["maxChC"] = nullptr;
    if (isfinite(s_st.boilerMaxBoundMinC)) b["boundMinC"] = s_st.boilerMaxBoundMinC; else b["boundMinC"] = nullptr;
    if (isfinite(s_st.boilerMaxBoundMaxC)) b["boundMaxC"] = s_st.boilerMaxBoundMaxC; else b["boundMaxC"] = nullptr;
    if (isfinite(s_st.boilerClampMinC)) b["clampMinC"] = s_st.boilerClampMinC; else b["clampMinC"] = nullptr;
    if (isfinite(s_st.boilerClampMaxC)) b["clampMaxC"] = s_st.boilerClampMaxC; else b["clampMaxC"] = nullptr;
  }
}

void equithermInit() {
  if (s_inited) return;
  s_inited = true;
  equithermReloadFromStore();
  s_lastComputeMs = 0;
  s_lastOutsideFilterMs = 0;
  s_outsideFiltered = NAN;
  resetMixFeedbackTracking();
  resetAccumulatorSupportTracking();
  s_mixManualHoldUntilMs = 0;
  s_st = EquithermStatus{};
  s_mix = MixPulse{};
  s_mix.positionPct = 50.0f;
  mixAllOff();
  Serial.println("[EQ] Init");
}

void equithermLoop() {
  equithermInit();
  const uint32_t now = millis();
  mixUpdate(now);
  // Run control loop frequently so mixing valve pulse timing matches UI settings.
  // OpenTherm writes are still rate-limited by minSendIntervalMs inside computeAndSend().
  if (now - s_lastComputeMs < kEqControlIntervalMs) return;
  s_lastComputeMs = now;
  computeAndSend();
}

EquithermConfig equithermGetConfig() {
  equithermInit();
  return s_cfg;
}

EquithermStatus equithermGetStatus() {
  equithermInit();
  return s_st;
}

void equithermFillFastJson(JsonObject& out) {
  equithermInit();
  out["en"] = s_cfg.enabled;
  if (s_st.modeReq.length()) out["m"] = s_st.modeReq; else out["m"] = nullptr;
  if (s_st.modeEff.length()) out["me"] = s_st.modeEff; else out["me"] = nullptr;
  out["su"] = s_st.scheduleUsed;
  out["ia"] = s_st.in1Active;
  out["i1"] = s_st.in1ForcingNight;
  out["sm"] = s_st.summerActive;
  out["tv"] = s_st.timeValid;
  out["ac"] = s_st.active;
  if (s_st.reason.length()) out["rs"] = s_st.reason; else out["rs"] = nullptr;
  if (isfinite(s_st.outsideC)) out["oc"] = s_st.outsideC; else out["oc"] = nullptr;
  if (isfinite(s_st.flowC)) out["fc"] = s_st.flowC; else out["fc"] = nullptr;
  if (isfinite(s_st.mixTempAC)) out["ma"] = s_st.mixTempAC; else out["ma"] = nullptr;
  if (isfinite(s_st.mixTempBC)) out["mb"] = s_st.mixTempBC; else out["mb"] = nullptr;
  if (isfinite(s_st.mixFeedbackC)) out["mf"] = s_st.mixFeedbackC; else out["mf"] = nullptr;
  if (isfinite(s_st.targetBaseFlowC)) out["tb"] = s_st.targetBaseFlowC; else out["tb"] = nullptr;
  if (isfinite(s_st.targetFlowC)) out["tf"] = s_st.targetFlowC; else out["tf"] = nullptr;
  if (isfinite(s_st.supportTargetFlowC)) out["ts"] = s_st.supportTargetFlowC; else out["ts"] = nullptr;
  out["sa"] = s_st.accumulatorSupportActive;
  out["sv"] = s_st.accumulatorSupportAvailable;
  out["sr"] = s_st.accumulatorSupportTargetReached;
  JsonObject mix = out.createNestedObject("mix");
  mix["hr"] = (uint32_t)(kMixHeatRelayIndex + 1);
  mix["cr"] = (uint32_t)(kMixCoolRelayIndex + 1);
  mix["state"] = s_st.mixState.length() ? s_st.mixState : "idle";
  mix["pulsing"] = s_st.mixPulsing;
  mix["manual"] = s_st.mixManual;
  mix["prm"] = (uint32_t)s_st.mixPulseRemainingMs;
  mix["elp"] = (uint32_t)s_st.mixPulseElapsedMs;
  if (isfinite(s_st.mixPositionPct)) mix["pct"] = s_st.mixPositionPct; else mix["pct"] = nullptr;
  mix["pt"] = s_st.mixPositionTrusted;
  mix["sa"] = s_st.accumulatorSupportActive;
  mix["sr"] = s_st.accumulatorSupportTargetReached;
  mix["act"] = s_st.accumulatorSupportAction.length() ? s_st.accumulatorSupportAction : s_cfg.mixTargetReachedAction;
  mix["ra"] = s_st.mixRelayApplyOk;
  mix["rm"] = (uint32_t)s_st.mixRelayMask;
  if (s_st.mixCalibrationState.length()) mix["cal"] = s_st.mixCalibrationState; else mix["cal"] = nullptr;
  out["ok"] = s_st.lastSendOk;
}

String equithermGetStatusJson() {
  equithermInit();
  DynamicJsonDocument doc(8192);
  doc["ok"] = true;
  JsonObject cfg = doc.createNestedObject("config");
  fillConfigJson(cfg);
  JsonObject st = doc.createNestedObject("status");
  fillStatusJson(st);
  String out;
  serializeJson(doc, out);
  return out;
}

static void applyConfigDoc(JsonObjectConst o) {
  if (o.containsKey("enabled")) {
    const bool en = (bool)(o["enabled"] | false);
    ConfigStore::setEqEnabled(en);
  }
  if (o.containsKey("mode")) {
    String m = String((const char*)(o["mode"] | "auto"));
    m.trim();
    m.toLowerCase();
    ConfigStore::setEqMode(m);
  }
  if (o.containsKey("useIn1NightOverride")) {
    ConfigStore::setEqUseIn1NightOverride((bool)(o["useIn1NightOverride"] | true));
  }

  if (o.containsKey("schedule") && o["schedule"].is<JsonObjectConst>()) {
    JsonObjectConst s = o["schedule"].as<JsonObjectConst>();
    if (s.containsKey("enabled")) ConfigStore::setEqScheduleEnabled((bool)(s["enabled"] | false));

    // week: array of 7 objects; accept either HH:MM strings or minutes.
    if (s.containsKey("week") && s["week"].is<JsonArrayConst>()) {
      uint8_t counts[7] = {};
      uint16_t starts[7][HEATING_MAX_INTERVALS_PER_DAY] = {};
      uint16_t ends[7][HEATING_MAX_INTERVALS_PER_DAY] = {};
      ConfigStore::getEqScheduleIntervals(counts, starts, ends);

      JsonArrayConst a = s["week"].as<JsonArrayConst>();
      int i = 0;
      for (JsonVariantConst vv : a) {
        if (i >= 7) break;
        if (!vv.is<JsonObjectConst>()) { i++; continue; }
        JsonObjectConst d = vv.as<JsonObjectConst>();

        bool usedIntervals = false;
        if (d.containsKey("intervals") && d["intervals"].is<JsonArrayConst>()) {
          JsonArrayConst ia = d["intervals"].as<JsonArrayConst>();
          uint8_t c = 0;
          for (JsonVariantConst iv : ia) {
            if (c >= HEATING_MAX_INTERVALS_PER_DAY || !iv.is<JsonObjectConst>()) break;
            JsonObjectConst io = iv.as<JsonObjectConst>();
            uint16_t sm = 0, em = 0;
            bool okS = false, okE = false;
            if (io.containsKey("startMin")) { sm = (uint16_t)(io["startMin"] | 0); okS = true; }
            if (io.containsKey("endMin")) { em = (uint16_t)(io["endMin"] | 0); okE = true; }
            if (io.containsKey("start")) { String v = String((const char*)(io["start"] | "")); v.trim(); uint16_t mv = parseHmToMin(v, okS); if (okS) sm = mv; }
            if (io.containsKey("end")) { String v = String((const char*)(io["end"] | "")); v.trim(); uint16_t mv = parseHmToMin(v, okE); if (okE) em = mv; }
            if (okS && okE && sm != em) { starts[i][c] = sm; ends[i][c] = em; c++; }
          }
          counts[i] = c;
          usedIntervals = true;
        }

        if (!usedIntervals) {
          uint16_t ds = (counts[i] > 0) ? starts[i][0] : 360;
          uint16_t ns = (counts[i] > 0) ? ends[i][0] : 1320;
          if (d.containsKey("dayStartMin")) ds = (uint16_t)(d["dayStartMin"] | ds);
          if (d.containsKey("nightStartMin")) ns = (uint16_t)(d["nightStartMin"] | ns);
          if (d.containsKey("dayStart")) { String sds = String((const char*)(d["dayStart"] | "")); sds.trim(); bool ok=false; uint16_t mv = parseHmToMin(sds, ok); if (ok) ds = mv; }
          if (d.containsKey("nightStart")) { String sns = String((const char*)(d["nightStart"] | "")); sns.trim(); bool ok=false; uint16_t mv = parseHmToMin(sns, ok); if (ok) ns = mv; }
          counts[i] = 1;
          starts[i][0] = ds;
          ends[i][0] = ns;
        }

        i++;
      }
      ConfigStore::setEqScheduleIntervals(counts, starts, ends);
    }
  }

  auto applyCurve = [&](const char* key, void (*setter)(float,float,float,float), float oc, float fc, float ow, float fw) {
    if (!o.containsKey(key) || !o[key].is<JsonObjectConst>()) return;
    JsonObjectConst c = o[key].as<JsonObjectConst>();
    float outCold = c["outColdC"] | oc;
    float flowCold = c["flowColdC"] | fc;
    float outWarm = c["outWarmC"] | ow;
    float flowWarm = c["flowWarmC"] | fw;
    setter(outCold, flowCold, outWarm, flowWarm);
  };

  applyCurve("day", ConfigStore::setEqDayCurve,
             ConfigStore::getEqDayOutColdC(), ConfigStore::getEqDayFlowColdC(),
             ConfigStore::getEqDayOutWarmC(), ConfigStore::getEqDayFlowWarmC());

  applyCurve("night", ConfigStore::setEqNightCurve,
             ConfigStore::getEqNightOutColdC(), ConfigStore::getEqNightFlowColdC(),
             ConfigStore::getEqNightOutWarmC(), ConfigStore::getEqNightFlowWarmC());

  if (o.containsKey("limits") && o["limits"].is<JsonObjectConst>()) {
    JsonObjectConst l = o["limits"].as<JsonObjectConst>();
    if (l.containsKey("minFlowC") || l.containsKey("maxFlowC")) {
      float mn = l["minFlowC"] | ConfigStore::getEqMinFlowC();
      float mx = l["maxFlowC"] | ConfigStore::getEqMaxFlowC();
      ConfigStore::setEqFlowLimits(mn, mx);

      // The heating page exposes one pair of flow limits. Use the same pair as
      // the safety limits for the CH setpoint sent through OpenTherm unless the
      // request explicitly supplies separate CH limits below.
      if (!l.containsKey("minChSetpointC") && !l.containsKey("maxChSetpointC")) {
        ConfigStore::setEqChSetpointLimits(mn, mx);
      }
    }
    if (l.containsKey("minChSetpointC") || l.containsKey("maxChSetpointC")) {
      float mn = l["minChSetpointC"] | ConfigStore::getEqMinChSetpointC();
      float mx = l["maxChSetpointC"] | ConfigStore::getEqMaxChSetpointC();
      ConfigStore::setEqChSetpointLimits(mn, mx);
    }
  }

  if (o.containsKey("temps") && o["temps"].is<JsonObjectConst>()) {
    JsonObjectConst t = o["temps"].as<JsonObjectConst>();
    if (t.containsKey("maxAgeMs")) ConfigStore::setEqTempMaxAgeMs((uint32_t)(t["maxAgeMs"] | ConfigStore::getEqTempMaxAgeMs()));
  }

  if (o.containsKey("send") && o["send"].is<JsonObjectConst>()) {
    JsonObjectConst s = o["send"].as<JsonObjectConst>();
    if (s.containsKey("minIntervalMs")) ConfigStore::setEqMinSendIntervalMs((uint32_t)(s["minIntervalMs"] | ConfigStore::getEqMinSendIntervalMs()));
    if (s.containsKey("minDeltaC")) ConfigStore::setEqMinSendDeltaC(s["minDeltaC"].as<float>());
  }

  if (o.containsKey("output") && o["output"].is<JsonObjectConst>()) {
    JsonObjectConst out = o["output"].as<JsonObjectConst>();
    if (out.containsKey("useOpenTherm")) ConfigStore::setEqUseOpenTherm((bool)(out["useOpenTherm"] | true));
    if (out.containsKey("applyBoilerMaxCh")) ConfigStore::setEqApplyBoilerMaxCh((bool)(out["applyBoilerMaxCh"] | false));
    if (out.containsKey("boilerMaxChC")) ConfigStore::setEqBoilerMaxChC(out["boilerMaxChC"].as<float>());

    if (out.containsKey("driveNightRelay")) ConfigStore::setEqDriveNightRelay((bool)(out["driveNightRelay"] | true));
    if (out.containsKey("nightRelay")) {
      int r = (int)(out["nightRelay"] | (int)(kDefaultNightRelayIndex + 1));
      if (r < 1) r = 1;
      if (r > 8) r = 8;
      uint8_t idx = (uint8_t)(r - 1);
      if (isMixRelayIndex(idx)) idx = kDefaultNightRelayIndex;
      ConfigStore::setEqNightRelayIndex(idx);
    }
    if (out.containsKey("nightRelayOnWhenNight")) ConfigStore::setEqNightRelayOnWhenNight((bool)(out["nightRelayOnWhenNight"] | true));
  }

  if (o.containsKey("mixing") && o["mixing"].is<JsonObjectConst>()) {
    JsonObjectConst m = o["mixing"].as<JsonObjectConst>();
    if (m.containsKey("enabled")) ConfigStore::setEqMixingEnabled((bool)(m["enabled"] | false));
    if (m.containsKey("openRelay") || m.containsKey("heatRelay")) {
      // Ignored: relay mapping is fixed by hardware (R1 = heating/open direction).
      ConfigStore::setEqMixOpenRelayIndex(kMixHeatRelayIndex);
    }
    if (m.containsKey("closeRelay") || m.containsKey("coolRelay")) {
      // Ignored: relay mapping is fixed by hardware (R2 = cooling/close direction).
      ConfigStore::setEqMixCloseRelayIndex(kMixCoolRelayIndex);
    }
    if (m.containsKey("deadbandC")) ConfigStore::setEqMixDeadbandC(m["deadbandC"].as<float>());
    if (m.containsKey("targetOffsetC")) ConfigStore::setEqMixTargetOffsetC(m["targetOffsetC"].as<float>());
    if (m.containsKey("targetReachedAction")) {
      const char* value = m["targetReachedAction"] | "return_a";
      ConfigStore::setEqMixTargetReachedAction(String(value ? value : "return_a"));
    }
    if (m.containsKey("pulseMs")) ConfigStore::setEqMixPulseMs((uint32_t)(m["pulseMs"] | ConfigStore::getEqMixPulseMs()));
    if (m.containsKey("minIntervalMs")) ConfigStore::setEqMixMinIntervalMs((uint32_t)(m["minIntervalMs"] | ConfigStore::getEqMixMinIntervalMs()));
    if (m.containsKey("travelMs")) ConfigStore::setEqMixTravelMs((uint32_t)(m["travelMs"] | ConfigStore::getEqMixTravelMs()));
    if (m.containsKey("calibrationSeatMs")) ConfigStore::setEqMixCalibrationSeatMs((uint32_t)(m["calibrationSeatMs"] | ConfigStore::getEqMixCalibrationSeatMs()));
    if (m.containsKey("autoRecalibrationMs")) ConfigStore::setEqMixAutoRecalibrationMs((uint32_t)(m["autoRecalibrationMs"] | ConfigStore::getEqMixAutoRecalibrationMs()));
    if (m.containsKey("sourceA") || m.containsKey("tempSourceA")) {
      const char* value = m.containsKey("sourceA") ? (m["sourceA"] | "tank_mid") : (m["tempSourceA"] | "tank_mid");
      ConfigStore::setEqMixTempSourceA(String(value ? value : "tank_mid"));
    }
    if (m.containsKey("sourceB") || m.containsKey("tempSourceB")) {
      const char* value = m.containsKey("sourceB") ? (m["sourceB"] | "return_dallas") : (m["tempSourceB"] | "return_dallas");
      ConfigStore::setEqMixTempSourceB(String(value ? value : "return_dallas"));
    }
    if (m.containsKey("sourceAB") || m.containsKey("tempSourceAB")) {
      const char* value = m.containsKey("sourceAB") ? (m["sourceAB"] | "opentherm_ch") : (m["tempSourceAB"] | "opentherm_ch");
      ConfigStore::setEqMixTempSourceAB(String(value ? value : "opentherm_ch"));
    }
  }

  if (o.containsKey("boilerAssist") && o["boilerAssist"].is<JsonObjectConst>()) {
    JsonObjectConst b = o["boilerAssist"].as<JsonObjectConst>();
    if (b.containsKey("enabled")) ConfigStore::setEqBoilerAssistEnabled((bool)(b["enabled"] | false));
    if (b.containsKey("deltaC")) ConfigStore::setEqBoilerAssistDeltaC(b["deltaC"].as<float>());
    if (b.containsKey("forceChEnable")) ConfigStore::setEqBoilerAssistForceChEnable((bool)(b["forceChEnable"] | true));
  }
}

void equithermReloadFromStore() {
  const String previousFeedbackSource = s_cfg.mixTempSourceAB;
  const float previousSupportOffsetC = s_cfg.mixTargetOffsetC;
  const String previousTargetAction = s_cfg.mixTargetReachedAction;
  const bool previousSupportEnabled = s_cfg.boilerAssistEnabled;
  loadFromPrefs();
  if (previousFeedbackSource.length() && previousFeedbackSource != s_cfg.mixTempSourceAB) {
    if (s_mix.active) stopMixingNow(millis(), true);
    resetMixFeedbackTracking();
  }
  if (fabsf(previousSupportOffsetC - s_cfg.mixTargetOffsetC) > 0.01f
      || previousTargetAction != s_cfg.mixTargetReachedAction
      || previousSupportEnabled != s_cfg.boilerAssistEnabled) {
    if (s_mix.active && !s_mix.manual) stopMixingNow(millis(), true);
    resetAccumulatorSupportTracking();
  }
}

void equithermApplyConfig(const String& json) {
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, json)) return;
  if (!doc.is<JsonObject>()) return;
  JsonObjectConst o = doc.as<JsonObjectConst>();
  {
    ConfigStore::BatchGuard storeBatch;
    applyConfigDoc(o);
  }
  equithermReloadFromStore();

  // If Ekviterm is enabled and should control via OpenTherm, ensure OT is in control mode.
  if (s_cfg.enabled && s_cfg.useOpenTherm) {
    // Ensure OpenTherm module is in control mode (required for Ekviterm writes).
    if (!ConfigStore::getOtEnabled()) ConfigStore::setOtEnabled(true);
    if (!ConfigStore::getOtAutoStart()) ConfigStore::setOtAutoStart(true);
    if (ConfigStore::getOtMode() == "readOnly") ConfigStore::setOtMode("control");

    DynamicJsonDocument wrap(256);
    JsonObject ot = wrap.createNestedObject("opentherm");
    ot["enabled"] = true;
    ot["autoStart"] = true;
    ot["mode"] = "control";
    // IMPORTANT: boilerControl="relay" forces read-only; we need any other value here.
    ot["boilerControl"] = "opentherm";
    String js;
    serializeJson(wrap, js);
    openthermApplyConfig(js);
  }

  recomputeNow();
}

bool equithermHandleCmdJson(const String& json, String& outErr) {
  outErr = "";
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json) || !doc.is<JsonObject>()) {
    outErr = "bad json";
    return false;
  }

  JsonObjectConst o = doc.as<JsonObjectConst>();
  if (o.containsKey("enabled")) {
    ConfigStore::setEqEnabled((bool)(o["enabled"] | false));
  }
  if (o.containsKey("mode")) {
    String m = String((const char*)(o["mode"] | "auto"));
    m.trim();
    m.toLowerCase();
    if (m != "auto" && m != "day" && m != "night") {
      outErr = "bad mode";
      return false;
    }
    ConfigStore::setEqMode(m);
  }
  if (o.containsKey("mixMove")) {
    String cmd = String((const char*)(o["mixMove"] | ""));
    cmd.trim();
    cmd.toLowerCase();
    const uint32_t now = millis();
    if (s_externalBlock || dhwIsPriorityActive()) {
      outErr = "blocked_dhw";
      return false;
    }
    int8_t dir = 0;
    float endPos = NAN;
    if (cmd == "a_end" || cmd == "open_end") { dir = kMixDirectionA; endPos = 100.0f; }
    else if (cmd == "b_end" || cmd == "close_end") { dir = kMixDirectionB; endPos = 0.0f; }
    else { outErr = "bad mixMove"; return false; }
    const uint32_t seatMs = s_cfg.mixTravelMs + s_cfg.mixCalibrationSeatMs;
    if (!mixStartPulse(dir, now, true, seatMs, true)) {
      outErr = "mix move rejected";
      return false;
    }
    s_mix.forceEndPosition = true;
    s_mix.endPositionPct = endPos;
    s_lastComputeMs = 0;
    return true;
  }

  if (o.containsKey("mixCalibrate")) {
    String cmd = String((const char*)(o["mixCalibrate"] | ""));
    cmd.trim();
    cmd.toLowerCase();
    const uint32_t now = millis();
    if (cmd == "invalidate") {
      stopMixingNow(now, false);
      s_mixManualHoldUntilMs = 0;
      s_autoCalibrationAttemptedThisCycle = true;
      mixInvalidateCalibration(false);
      s_mixActuatorRetryAfterMs = 0;
      s_mixIneffectivePulseCount = 0;
      s_mixFeedbackFaultLatched = false;
      s_lastComputeMs = 0;
      return true;
    }
    if (cmd == "sync_a") {
      stopMixingNow(now, false);
      s_mixManualHoldUntilMs = 0;
      mixMarkCalibrated(100.0f, kMixDirectionA);
      s_lastComputeMs = 0;
      return true;
    }
    if (cmd == "sync_b") {
      stopMixingNow(now, false);
      s_mixManualHoldUntilMs = 0;
      mixMarkCalibrated(0.0f, kMixDirectionB);
      s_lastComputeMs = 0;
      return true;
    }
    if (s_externalBlock || dhwIsPriorityActive()) {
      outErr = "blocked_dhw";
      return false;
    }
    int8_t dir = 0;
    float endPos = NAN;
    if (cmd == "a") { dir = kMixDirectionA; endPos = 100.0f; }
    else if (cmd == "b") { dir = kMixDirectionB; endPos = 0.0f; }
    else { outErr = "bad mixCalibrate"; return false; }
    const uint32_t seatMs = s_cfg.mixTravelMs + s_cfg.mixCalibrationSeatMs;
    if (!mixStartPulse(dir, now, true, seatMs, true)) {
      outErr = "mix calibration rejected";
      return false;
    }
    s_mix.forceEndPosition = true;
    s_mix.endPositionPct = endPos;
    s_lastComputeMs = 0;
    return true;
  }

  if (o.containsKey("mixPulse")) {
    String cmd = String((const char*)(o["mixPulse"] | ""));
    cmd.trim();
    cmd.toLowerCase();
    const uint32_t pulseMs = o.containsKey("pulseMs") ? (uint32_t)(o["pulseMs"] | s_cfg.mixPulseMs) : s_cfg.mixPulseMs;
    const uint32_t now = millis();
    if (cmd == "stop") {
      stopMixingNow(now, true);
      s_mixManualHoldUntilMs = 0;
      s_lastComputeMs = 0;
      return true;
    }
    if (s_externalBlock || dhwIsPriorityActive()) {
      outErr = "blocked_dhw";
      return false;
    }
    int8_t dir = 0;
    if (cmd == "open" || cmd == "a") dir = kMixDirectionA;
    else if (cmd == "close" || cmd == "b") dir = kMixDirectionB;
    else {
      outErr = "bad mixPulse";
      return false;
    }
    if (!mixStartPulse(dir, now, true, pulseMs, true)) {
      outErr = "mix pulse rejected";
      return false;
    }
    s_lastComputeMs = 0;
    return true;
  }

  if (s_cfg.enabled && s_cfg.useOpenTherm) {
    // Ensure OpenTherm module is in control mode (required for Ekviterm writes).
    if (!ConfigStore::getOtEnabled()) ConfigStore::setOtEnabled(true);
    if (!ConfigStore::getOtAutoStart()) ConfigStore::setOtAutoStart(true);
    if (ConfigStore::getOtMode() == "readOnly") ConfigStore::setOtMode("control");

    DynamicJsonDocument wrap(256);
    JsonObject ot = wrap.createNestedObject("opentherm");
    ot["enabled"] = true;
    ot["autoStart"] = true;
    ot["mode"] = "control";
    // IMPORTANT: boilerControl="relay" forces read-only; we need any other value here.
    ot["boilerControl"] = "opentherm";
    String js;
    serializeJson(wrap, js);
    openthermApplyConfig(js);
  }
  // Force immediate recompute
  s_lastComputeMs = 0;
  computeAndSend();
  return true;
}

void equithermBackgroundService() {
  equithermInit();
  mixUpdate(millis());
  // OpenTherm transactions can block for several hundred milliseconds. Service
  // the allow-listed WebSocket command channel during those waits so a manual
  // valve action reaches R1/R2 immediately after the pointer press.
  webPortalBackgroundService();
}

void equithermRequestRecompute() {
  recomputeNow();
}

extern "C" void openThermBackgroundService(void) {
  equithermBackgroundService();
}

void equithermSetExternalBlock(bool blocked) {
  const bool changed = (s_externalBlock != blocked);
  s_externalBlock = blocked;
  if (blocked) {
    stopMixingNow();
    // During DHW priority keep the boiler on its default/day curve, not on night setback.
    // This prevents R6 from unintentionally limiting boiler behavior while CH is blocked.
    resetNightRelayToSafeDay();
    if (changed) {
      openthermClearEquithermRequest();
    }
  }
}

#endif // FEATURE_EQUITHERM
