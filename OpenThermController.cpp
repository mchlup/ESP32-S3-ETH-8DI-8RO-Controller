#include "OpenThermController.h"

#include "LogicController.h"

#include <math.h>

// ------------------------------------------------------------
// OpenTherm placeholder implementation
// ------------------------------------------------------------
// Cíl: projekt se musí zkompilovat + UI/REST status musí fungovat.
// Fyzická komunikace OpenTherm vyžaduje HW transceiver a konkrétní driver.

static OpenThermCfg s_cfg;
static OpenThermStatus s_st;

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static const char* modeToString(OpenThermMode m) {
    switch (m) {
        case OpenThermMode::OFF:      return "off";
        case OpenThermMode::MANUAL:   return "manual";
        case OpenThermMode::EQUITHERM:return "equitherm";
        default:                      return "off";
    }
}

static OpenThermMode stringToOtMode(const String& sIn) {
    String s = sIn;
    s.trim();
    s.toLowerCase();
    if (s == "manual") return OpenThermMode::MANUAL;
    if (s == "equitherm" || s == "equithermal" || s == "ekviterm") return OpenThermMode::EQUITHERM;
    return OpenThermMode::OFF;
}

// --- Transport stubs ---

static bool transportReady() {
    // TODO: Implementovat OpenTherm transport pro ESP32-S3 + OT interface.
    return false;
}

static bool transportPoll(OpenThermStatus& st) {
    (void)st;
    // TODO: Poll (read boiler temp, modulation, fault codes...)
    return false;
}

static bool transportWriteSetpoint(float setpointC) {
    (void)setpointC;
    // TODO: Write CH setpoint (OpenTherm ID 1)
    return false;
}

void openthermInit() {
    s_cfg = OpenThermCfg();
    s_st = OpenThermStatus();
}

void openthermApplyConfig(const String& json) {
    StaticJsonDocument<4096> doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return;

    JsonObject root = doc.as<JsonObject>();
    JsonObject ot = root["opentherm"].as<JsonObject>();
    if (!ot) return;

    s_cfg.enabled = ot["enabled"] | false;
    s_cfg.mode = stringToOtMode(String((const char*)(ot["mode"] | "off")));
    s_cfg.inPin = (int8_t)(ot["inPin"] | -1);
    s_cfg.outPin = (int8_t)(ot["outPin"] | -1);
    s_cfg.pollIntervalMs = ot["pollIntervalMs"] | ot["pollMs"] | 1000; // kompatibilita
    if (s_cfg.pollIntervalMs < 250) s_cfg.pollIntervalMs = 250;
    if (s_cfg.pollIntervalMs > 30000) s_cfg.pollIntervalMs = 30000;

    s_cfg.chEnable = ot["chEnable"] | true;
    s_cfg.manualSetpointC = clampf((float)(ot["manualSetpointC"] | 45.0f), 10.0f, 90.0f);

    s_cfg.minDeltaWriteC = clampf((float)(ot["minDeltaWriteC"] | 0.5f), 0.1f, 20.0f);
    s_cfg.minWriteIntervalMs = ot["minWriteIntervalMs"] | 5000;
    if (s_cfg.minWriteIntervalMs < 250) s_cfg.minWriteIntervalMs = 250;
    if (s_cfg.minWriteIntervalMs > 600000) s_cfg.minWriteIntervalMs = 600000;

    // reset status on disable
    s_st.enabled = s_cfg.enabled;
    s_st.mode = s_cfg.mode;
    if (!s_cfg.enabled) {
        s_st.ready = false;
        s_st.lastError = "";
        s_st.setpointC = NAN;
    }
}

OpenThermCfg openthermGetConfig() {
    return s_cfg;
}

OpenThermStatus openthermGetStatus() {
    return s_st;
}

bool openthermRequestSetpoint(float tempC) {
    tempC = clampf(tempC, 10.0f, 90.0f);
    if (!s_cfg.enabled) return false;
    if (!s_st.ready) return false;

    const uint32_t now = millis();
    if ((uint32_t)(now - s_st.lastWriteMs) < s_cfg.minWriteIntervalMs) return false;
    if (isfinite(s_st.setpointC) && fabsf(tempC - s_st.setpointC) < s_cfg.minDeltaWriteC) return false;

    bool ok = transportWriteSetpoint(tempC);
    s_st.lastWriteMs = now;
    if (ok) {
        s_st.okFrames++;
        s_st.lastError = "";
    } else {
        s_st.errFrames++;
        if (!s_st.ready) s_st.lastError = "OpenTherm transport not ready";
    }
    return ok;
}

void openthermLoop() {
    const uint32_t nowMs = millis();

    if (!s_cfg.enabled) {
        s_st.enabled = false;
        s_st.mode = OpenThermMode::OFF;
        return;
    }

    s_st.enabled = true;
    s_st.mode = s_cfg.mode;
    s_st.ready = transportReady();

    // Decide desired setpoint
    float desired = s_cfg.manualSetpointC;
    if (s_cfg.mode == OpenThermMode::EQUITHERM) {
        EquithermStatus eq = logicGetEquithermStatus();
        if (eq.enabled && isfinite(eq.targetFlowC)) desired = eq.targetFlowC;
    }
    desired = clampf(desired, 10.0f, 90.0f);
    s_st.setpointC = desired;

    // Poll
    if ((uint32_t)(nowMs - s_st.lastPollMs) >= s_cfg.pollIntervalMs) {
        s_st.lastPollMs = nowMs;
        bool ok = transportPoll(s_st);
        if (ok) {
            s_st.okFrames++;
            s_st.lastError = "";
        } else {
            s_st.errFrames++;
            if (!s_st.ready) s_st.lastError = "OpenTherm transport not ready";
        }
    }

    // Write setpoint (rate-limited)
    (void)openthermRequestSetpoint(desired);
}

void openthermFillJson(JsonObject obj) {
    // cfg
    obj["enabled"] = s_cfg.enabled;
    obj["mode"] = modeToString(s_cfg.mode);
    obj["inPin"] = s_cfg.inPin;
    obj["outPin"] = s_cfg.outPin;
    obj["pollIntervalMs"] = s_cfg.pollIntervalMs;
    obj["chEnable"] = s_cfg.chEnable;
    obj["manualSetpointC"] = s_cfg.manualSetpointC;
    obj["minDeltaWriteC"] = s_cfg.minDeltaWriteC;
    obj["minWriteIntervalMs"] = s_cfg.minWriteIntervalMs;

    // status
    obj["ready"] = s_st.ready;
    obj["lastPollMs"] = s_st.lastPollMs;
    obj["lastWriteMs"] = s_st.lastWriteMs;
    if (isfinite(s_st.setpointC)) obj["setpointC"] = s_st.setpointC; else obj["setpointC"] = nullptr;
    if (isfinite(s_st.boilerTempC)) obj["boilerTempC"] = s_st.boilerTempC; else obj["boilerTempC"] = nullptr;
    if (isfinite(s_st.returnTempC)) obj["returnTempC"] = s_st.returnTempC; else obj["returnTempC"] = nullptr;
    if (isfinite(s_st.modulationPct)) obj["modulationPct"] = s_st.modulationPct; else obj["modulationPct"] = nullptr;
    obj["faultCode"] = s_st.faultCode;
    obj["okFrames"] = s_st.okFrames;
    obj["errFrames"] = s_st.errFrames;
    obj["lastError"] = s_st.lastError;
}

