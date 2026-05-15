#include "PressureAlarmController.h"

#include "ConfigStore.h"
#include "OpenThermController.h"
#include "BuzzerController.h"

namespace {
  PressureAlarmConfig s_cfg;
  PressureAlarmStatus s_st;

  void loadCfg() {
    s_cfg.enabled = ConfigStore::getPressureAlarmEnabled();
    s_cfg.minBar = ConfigStore::getPressureAlarmMinBar();
    s_cfg.maxBar = ConfigStore::getPressureAlarmMaxBar();
    s_cfg.hysteresisBar = ConfigStore::getPressureAlarmHysteresisBar();

    if (!isfinite(s_cfg.minBar)) s_cfg.minBar = 0.8f;
    if (!isfinite(s_cfg.maxBar)) s_cfg.maxBar = 2.8f;
    if (!isfinite(s_cfg.hysteresisBar)) s_cfg.hysteresisBar = 0.05f;
    if (s_cfg.minBar < 0.1f) s_cfg.minBar = 0.1f;
    if (s_cfg.maxBar > 6.0f) s_cfg.maxBar = 6.0f;
    if (s_cfg.minBar > s_cfg.maxBar) {
      const float tmp = s_cfg.minBar;
      s_cfg.minBar = s_cfg.maxBar;
      s_cfg.maxBar = tmp;
    }
    if (s_cfg.hysteresisBar < 0.01f) s_cfg.hysteresisBar = 0.01f;
    if (s_cfg.hysteresisBar > 1.0f) s_cfg.hysteresisBar = 1.0f;
  }

  void setState(bool low, bool high, bool sensorValid, float pressureBar) {
    const bool active = low || high;
    const bool changed =
      (s_st.lowActive != low) ||
      (s_st.highActive != high) ||
      (s_st.sensorValid != sensorValid);

    s_st.enabled = s_cfg.enabled;
    s_st.sensorValid = sensorValid;
    s_st.pressureBar = pressureBar;
    s_st.lowActive = low;
    s_st.highActive = high;
    s_st.active = active;

    if (!sensorValid) s_st.state = "sensor_invalid";
    else if (high) s_st.state = "high";
    else if (low) s_st.state = "low";
    else s_st.state = "ok";

    if (changed) s_st.lastChangeMs = millis();

    buzzerPlayWarning(s_cfg.enabled && active);
  }
}

void pressureAlarmReloadFromStore() {
  loadCfg();
}

void pressureAlarmInit() {
  pressureAlarmReloadFromStore();
  s_st = PressureAlarmStatus{};
  s_st.enabled = s_cfg.enabled;
  s_st.state = "init";
  s_st.lastChangeMs = millis();
}

void pressureAlarmLoop() {
  OpenThermStatusSnapshot ot = openthermGetStatus();
  const bool sensorValid = ot.present && ot.ready && isfinite(ot.pressureBar) && ot.pressureBar > 0.0f && ot.pressureBar < 10.0f;
  const float p = sensorValid ? ot.pressureBar : NAN;

  if (!s_cfg.enabled) {
    setState(false, false, sensorValid, p);
    return;
  }

  bool low = s_st.lowActive;
  bool high = s_st.highActive;

  if (!sensorValid) {
    setState(false, false, false, NAN);
    return;
  }

  if (!low && !high) {
    if (p <= s_cfg.minBar) low = true;
    else if (p >= s_cfg.maxBar) high = true;
  } else if (low) {
    if (p >= (s_cfg.minBar + s_cfg.hysteresisBar)) low = false;
  } else if (high) {
    if (p <= (s_cfg.maxBar - s_cfg.hysteresisBar)) high = false;
  }

  setState(low, high, true, p);
}

PressureAlarmConfig pressureAlarmGetConfig() {
  pressureAlarmReloadFromStore();
  return s_cfg;
}

PressureAlarmStatus pressureAlarmGetStatus() {
  return s_st;
}

void pressureAlarmFillFastJson(JsonObject& out) {
  out["en"] = s_st.enabled;
  out["sv"] = s_st.sensorValid;
  if (isfinite(s_st.pressureBar)) out["p"] = s_st.pressureBar; else out["p"] = nullptr;
  out["act"] = s_st.active;
  out["lo"] = s_st.lowActive;
  out["hi"] = s_st.highActive;
  if (s_st.state.length()) out["st"] = s_st.state; else out["st"] = nullptr;
  out["chg"] = (uint32_t)s_st.lastChangeMs;
}
