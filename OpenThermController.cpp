#include "Features.h"
#include "OpenThermController.h"

#if defined(FEATURE_OPENTHERM)

#include <Arduino.h>
#include "Log.h"
#include "config_pins.h"
#include "OpenThermDataIds.h"

#include "OTBusESP32Pro.h"   // OTBusESP32Pro (wraps Ihor Melnyk OpenTherm backend)

namespace {
  OpenThermConfig g_cfg;
  OpenThermStatusSnapshot g_st;

  OTBusESP32Pro* g_bus = nullptr;
  bool g_inited = false;

  // Centralized requested outputs.
  OpenThermSourceRequest g_manualReq;
  OpenThermSourceRequest g_equithermReq;
  OpenThermSourceRequest g_dhwReq;

  bool  g_reqChEnable = true;
  bool  g_reqDhwEnable = true;
  float g_reqChSetpointC = NAN;
  float g_reqDhwSetpointC = NAN;
  float g_reqMaxModPct = NAN;
  String g_activeSource = "manual";

  uint32_t g_lastPollMs = 0;
  uint8_t g_pollStep = 0;
  uint8_t g_extPollIndex = 0;
  uint8_t g_remotePollIndex = 0;
  uint32_t g_lastRemotePollMs = 0;
  uint32_t g_lastLinkRxMs = 0;
  bool g_cycleStatusSeen = false;
  bool g_cycleCoreData = false;

  // Control requests are queued and applied from openthermLoop() one OT
  // transaction at a time. HTTP/DHW handlers never execute a multi-second
  // OpenTherm transaction bundle synchronously.
  bool g_controlDirty = false;
  bool g_controlRetryNeeded = false;
  uint8_t g_controlStep = 0;
  uint32_t g_controlNextAttemptMs = 0;
  float g_appliedChSetpointC = NAN;
  float g_appliedDhwSetpointC = NAN;
  float g_appliedMaxModPct = NAN;
  bool g_appliedStatusValid = false;

  // Boot time reference for safe delayed init
  uint32_t g_bootMs = 0;

  // ---- Data-ID scan state (non-blocking) ----
  struct ScanState {
    bool active = false;
    bool done = false;
    bool includeAll = false;
    uint8_t startId = 0;
    uint8_t endId = 127;
    uint8_t curId = 0;
    uint16_t delayMs = 50;
    uint32_t nextStepMs = 0;
    uint32_t startedMs = 0;
    uint32_t finishedMs = 0;
    uint16_t supportedCount = 0;
    uint8_t respStatus[128] = {0};
    uint8_t msgType[128] = {0};
    uint16_t value[128] = {0};
    bool supported[128] = {false};
  };

  ScanState g_scan;

  // Default wiring for ESP32-S3-ETH-8DI-8RO.
  static constexpr int kOT_RX = OT_RX_PIN; // adapter OUT -> MCU (interrupt)
  static constexpr int kOT_TX = OT_TX_PIN; // MCU -> adapter IN

  // Forward declarations
  static bool cfgIsReadOnly();
  static bool clampMaybe(float& v, float lo, float hi);
  static void otInitIfNeeded();
  static bool otPollStep();
  static bool processPendingControlStep();

  // Transport profile: keep the electrical layer byte-for-byte compatible with
  // the older, proven 3.3.14 build. Logic inversion fields remain in the public
  // configuration only for backward JSON compatibility; the legacy transport
  // itself uses the fixed polarity implemented by OpenTherm.cpp.
  static constexpr const char* kTransportProfile = "legacy-3.3.14";

  // Old Ihor Melnyk transport reports DATA_INVALID/UNKNOWN_DATA_ID as INVALID
  // because isValidResponse() accepts only READ_ACK/WRITE_ACK. Inspect the raw
  // frame only to distinguish a syntactically valid semantic NACK from a real
  // framing/parity error. This does not change the proven low-level driver.
  static bool lastRawSemanticResponse(uint8_t expectedId, otbus::MessageType* outType = nullptr) {
    if (!g_bus) return false;
    const uint32_t raw = g_bus->lastResponseRaw();
    if (OpenTherm::parity(raw)) return false; // valid OpenTherm frame has even parity
    if ((uint8_t)OpenTherm::getDataID(raw) != expectedId) return false;
    const otbus::MessageType mt = (otbus::MessageType)OpenTherm::getMessageType(raw);
    if (outType) *outType = mt;
    return mt == otbus::MessageType::DATA_INVALID || mt == otbus::MessageType::UNKNOWN_DATA_ID;
  }

  static bool isSupportedByFrame(OpenThermResponseStatus st, const otbus::Frame& f) {
    const otbus::MessageType mt = f.type();
    if (st == OpenThermResponseStatus::SUCCESS) {
      return mt == otbus::MessageType::READ_ACK || mt == otbus::MessageType::WRITE_ACK;
    }
    // Legacy OpenTherm.cpp classifies DATA_INVALID as INVALID at transport level.
    // It still proves that this Data-ID is implemented when parity and ID match.
    if (st == OpenThermResponseStatus::INVALID && mt == otbus::MessageType::DATA_INVALID && !OpenTherm::parity(f.raw)) {
      return true;
    }
    return false;
  }
  static void otDestroy() {
    if (g_bus) {
      delete g_bus;
      g_bus = nullptr;
    }
    g_inited = false;
    g_pollStep = 0;
    g_extPollIndex = 0;
    g_remotePollIndex = 0;
    g_lastRemotePollMs = 0;
    g_cycleStatusSeen = false;
    g_cycleCoreData = false;
    g_appliedChSetpointC = NAN;
    g_appliedDhwSetpointC = NAN;
    g_appliedMaxModPct = NAN;
    g_appliedStatusValid = false;
    g_controlDirty = true;
    g_controlStep = 0;
    g_controlNextAttemptMs = 0;
  }

  static void stNoteResponse(OpenThermResponseStatus rs) {
    if (rs == OpenThermResponseStatus::SUCCESS) g_st.okCount++;
    else if (rs == OpenThermResponseStatus::TIMEOUT) g_st.timeoutCount++;
    else if (rs == OpenThermResponseStatus::INVALID) g_st.invalidCount++;
  }

  static bool sameFloat(float a, float b, float eps = 0.01f) {
    if (!isfinite(a) && !isfinite(b)) return true;
    if (!isfinite(a) || !isfinite(b)) return false;
    return fabsf(a - b) <= eps;
  }

  static void markLinkAlive() { g_lastLinkRxMs = millis(); }

  static void markTelemetryAlive() {
    const uint32_t now = millis();
    g_lastLinkRxMs = now;
    g_st.lastUpdateMs = now;
  }

  static uint32_t linkFreshWindowMs() {
    uint32_t w = g_cfg.pollMs * 4UL;
    if (w < 6000UL) w = 6000UL;
    if (w > 30000UL) w = 30000UL;
    return w;
  }

  static bool linkIsFresh() {
    return g_lastLinkRxMs != 0 && (uint32_t)(millis() - g_lastLinkRxMs) <= linkFreshWindowMs();
  }

  static bool responseProvesLink(uint8_t expectedId, OpenThermResponseStatus rs) {
    if (rs == OpenThermResponseStatus::SUCCESS) return true;
    if (rs == OpenThermResponseStatus::INVALID) return lastRawSemanticResponse(expectedId, nullptr);
    return false;
  }

  static void clearCoreTelemetry() {
    g_st.fault = false;
    g_st.chEnable = false;
    g_st.dhwEnable = false;
    g_st.chActive = false;
    g_st.dhwActive = false;
    g_st.flameOn = false;
    g_st.coolingActive = false;
    g_st.diagnostic = false;

    g_st.statusRaw = 0;
    g_st.masterStatusRaw = 0;
    g_st.slaveStatusRaw = 0;
    g_st.otcActive = false;
    g_st.ch2Enable = false;
    g_st.ch2Active = false;

    g_st.boilerTempC = NAN;
    g_st.returnTempC = NAN;
    g_st.dhwTempC = NAN;
    g_st.modulationPct = NAN;
    g_st.pressureBar = NAN;

    g_st.faultFlags = 0;
    g_st.oemFaultCode = 0;
    g_st.lastUpdateMs = 0;
  }

  static void clearAllTelemetry() {
    clearCoreTelemetry();
    g_st.outsideTempC = NAN;
    g_st.roomTempC = NAN;
    g_st.solarStorageTempC = NAN;
    g_st.solarCollectorTempC = NAN;
    g_st.ch2FlowTempC = NAN;
    g_st.dhw2TempC = NAN;
    g_st.exhaustTempC = NAN;
    g_st.heatExchangerTempC = NAN;

    g_st.maxChSetpointC = NAN;
    g_st.maxChBoundMinC = NAN;
    g_st.maxChBoundMaxC = NAN;
    g_st.dhwSetpointC = NAN;
    g_st.dhwBoundMinC = NAN;
    g_st.dhwBoundMaxC = NAN;
    g_st.maxCapacityKw = NAN;
    g_st.minModulationPct = NAN;

    g_st.reqChSetpointC = NAN;
    g_st.reqDhwSetpointC = NAN;
    g_st.reqMaxModulationPct = NAN;
    g_st.activeSource = "";
    g_st.lastCmd = "";
  }

  static OpenThermSourceRequest defaultManualBaseRequest() {
    OpenThermSourceRequest req{};
    req.active = true;
    req.chEnableSet = true;
    req.chEnable = true;
    req.dhwEnableSet = true;
    req.dhwEnable = true;
    return req;
  }

  static void resetManualReq(OpenThermSourceRequest& req) {
    req = defaultManualBaseRequest();
  }

  static void clearReq(OpenThermSourceRequest& req) {
    req = OpenThermSourceRequest{};
  }

  static void clearManualField(OpenThermSourceRequest& req, const String& field) {
    if (field == "chEnable") {
      req.chEnableSet = true;
      req.chEnable = true;
      return;
    }
    if (field == "dhwEnable") {
      req.dhwEnableSet = true;
      req.dhwEnable = true;
      return;
    }
    if (field == "chSetpointC") {
      req.chSetpointSet = false;
      req.chSetpointC = NAN;
      return;
    }
    if (field == "dhwSetpointC") {
      req.dhwSetpointSet = false;
      req.dhwSetpointC = NAN;
      return;
    }
    if (field == "maxModulationPct") {
      req.maxModulationSet = false;
      req.maxModulationPct = NAN;
      return;
    }
  }

  static bool applySourceRequest(const OpenThermSourceRequest& src, const char* name,
                                 bool& chEnable, bool& dhwEnable,
                                 float& chSetpointC, float& dhwSetpointC, float& maxModPct,
                                 bool& haveChSetpoint, bool& haveDhwSetpoint, bool& haveMaxMod,
                                 String& activeSource) {
    if (!src.active) return false;
    if (src.chEnableSet) chEnable = src.chEnable;
    if (src.dhwEnableSet) dhwEnable = src.dhwEnable;
    if (src.chSetpointSet && isfinite(src.chSetpointC)) { chSetpointC = src.chSetpointC; haveChSetpoint = true; }
    if (src.dhwSetpointSet && isfinite(src.dhwSetpointC)) { dhwSetpointC = src.dhwSetpointC; haveDhwSetpoint = true; }
    if (src.maxModulationSet && isfinite(src.maxModulationPct)) { maxModPct = src.maxModulationPct; haveMaxMod = true; }
    activeSource = String(name);
    return true;
  }

  // Merge policy (explicit, field-by-field):
  // 1) manual = base defaults / service override layer
  //    - starts from safe defaults (CH enabled, DHW enabled)
  //    - may override individual fields only
  //    - can be reset as a whole or per field
  // 2) equitherm = heating layer
  //    - overrides manual per field when active
  // 3) dhw = highest-priority layer
  //    - overrides manual/equitherm per field when active
  //    - may explicitly disable CH and clear inherited CH setpoint
  static void rebuildEffectiveRequest() {
    const OpenThermSourceRequest manualBase = defaultManualBaseRequest();

    bool chEnable = manualBase.chEnable;
    bool dhwEnable = manualBase.dhwEnable;
    float chSetpointC = NAN;
    float dhwSetpointC = NAN;
    float maxModPct = NAN;
    bool haveChSetpoint = false;
    bool haveDhwSetpoint = false;
    bool haveMaxMod = false;
    String activeSource = String("default");

    applySourceRequest(g_manualReq, "manual", chEnable, dhwEnable, chSetpointC, dhwSetpointC, maxModPct,
                       haveChSetpoint, haveDhwSetpoint, haveMaxMod, activeSource);
    applySourceRequest(g_equithermReq, "equitherm", chEnable, dhwEnable, chSetpointC, dhwSetpointC, maxModPct,
                       haveChSetpoint, haveDhwSetpoint, haveMaxMod, activeSource);
    const bool dhwActive = applySourceRequest(g_dhwReq, "dhw", chEnable, dhwEnable, chSetpointC, dhwSetpointC, maxModPct,
                                              haveChSetpoint, haveDhwSetpoint, haveMaxMod, activeSource);
    if (dhwActive) {
      // During active DHW priority, CH setpoint from lower-priority sources must not leak through.
      if (g_dhwReq.chEnableSet && !g_dhwReq.chEnable && (!g_dhwReq.chSetpointSet || !isfinite(g_dhwReq.chSetpointC))) {
        chSetpointC = NAN;
        haveChSetpoint = false;
      }
    }

    g_reqChEnable = chEnable;
    g_reqDhwEnable = dhwEnable;
    g_reqChSetpointC = haveChSetpoint ? chSetpointC : NAN;
    g_reqDhwSetpointC = haveDhwSetpoint ? dhwSetpointC : NAN;
    g_reqMaxModPct = haveMaxMod ? maxModPct : NAN;
    g_activeSource = activeSource;
    g_st.activeSource = activeSource;
  }

  static bool validateReadyForControl(String& outErr) {
    outErr = "";
    if (!g_cfg.enabled) { outErr = "disabled"; return false; }
    if (!g_cfg.autoStart) { outErr = "paused"; return false; }
    if (g_bootMs && (uint32_t)(millis() - g_bootMs) < g_cfg.bootDelayMs) { outErr = "boot delay"; return false; }
    otInitIfNeeded();
    if (!g_bus) { outErr = g_st.reason.length() ? g_st.reason : String("init failed"); return false; }
    if (cfgIsReadOnly()) { outErr = "readOnly"; return false; }
    return true;
  }

  static bool applyEffectiveRequestToBus(String& outErr) {
    outErr = "";

    const bool oldChEnable = g_reqChEnable;
    const bool oldDhwEnable = g_reqDhwEnable;
    const float oldChSetpoint = g_reqChSetpointC;
    const float oldDhwSetpoint = g_reqDhwSetpointC;
    const float oldMaxMod = g_reqMaxModPct;
    const String oldSource = g_activeSource;

    rebuildEffectiveRequest();

    if (!g_cfg.enabled) { outErr = "disabled"; return false; }
    if (!g_cfg.autoStart) { outErr = "paused"; return false; }
    if (cfgIsReadOnly()) { outErr = "readOnly"; return false; }

    if (isfinite(g_reqChSetpointC)) {
      const float effectiveMinCh = g_activeSource.equalsIgnoreCase("equitherm") ? 10.0f : g_cfg.minChSetpointC;
      clampMaybe(g_reqChSetpointC, effectiveMinCh, g_cfg.maxChSetpointC);
    }
    if (isfinite(g_reqMaxModPct)) {
      if (g_reqMaxModPct < 0.0f) g_reqMaxModPct = 0.0f;
      if (g_reqMaxModPct > 100.0f) g_reqMaxModPct = 100.0f;
    }

    g_st.reqChSetpointC = g_reqChSetpointC;
    g_st.reqDhwSetpointC = g_reqDhwSetpointC;
    g_st.reqMaxModulationPct = g_reqMaxModPct;
    g_st.activeSource = g_activeSource;

    const bool changed =
        oldChEnable != g_reqChEnable || oldDhwEnable != g_reqDhwEnable ||
        !sameFloat(oldChSetpoint, g_reqChSetpointC) ||
        !sameFloat(oldDhwSetpoint, g_reqDhwSetpointC) ||
        !sameFloat(oldMaxMod, g_reqMaxModPct) || oldSource != g_activeSource;

    if (changed || (!g_appliedStatusValid && !g_controlDirty)) {
      g_controlDirty = true;
      g_controlRetryNeeded = false;
      g_controlStep = 0;
      g_controlNextAttemptMs = 0;
      String cmdSummary("QUEUED ");
      if (isfinite(g_reqChSetpointC)) cmdSummary += String("CH=") + String(g_reqChSetpointC, 1) + "C ";
      if (isfinite(g_reqDhwSetpointC)) cmdSummary += String("DHW=") + String(g_reqDhwSetpointC, 1) + "C ";
      if (isfinite(g_reqMaxModPct)) cmdSummary += String("MAXMOD=") + String(g_reqMaxModPct, 0) + "% ";
      cmdSummary += String("FLAGS=") + (g_reqChEnable ? "CH" : "") + (g_reqDhwEnable ? "+DHW" : "") + " SRC=" + g_activeSource;
      g_st.lastCmdMs = millis();
      g_st.lastCmd = cmdSummary;
    }
    return true;
  }

  static bool clampMaybe(float& v, float lo, float hi) {
    if (!isfinite(v)) return false;
    if (lo > hi) { float t = lo; lo = hi; hi = t; }
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return true;
  }

  // Mode helpers
  static bool cfgIsReadOnly() {
    const String m = g_cfg.mode;
    if (m.equalsIgnoreCase("readOnly") || m.equalsIgnoreCase("readonly")) return true;
    // Back-compat: boilerControl=relay means read-only
    if (g_cfg.boilerControl.equalsIgnoreCase("relay")) return true;
    return false;
  }

  static void otInitIfNeeded() {
    if (g_inited) return;

    // Enable gate: do nothing unless enabled
    if (!g_cfg.enabled) {
      g_st.present = false;
      g_st.ready = false;
      g_st.reason = "disabled";
      return;
    }

    // Safety: enabled but not allowed to auto-start.
    if (!g_cfg.autoStart) {
      g_st.present = false;
      g_st.ready = false;
      g_st.reason = "paused";
      return;
    }

    // Safety: boot delay
    if (g_bootMs && (uint32_t)(millis() - g_bootMs) < g_cfg.bootDelayMs) {
      g_st.present = false;
      g_st.ready = false;
      g_st.reason = "boot delay";
      return;
    }

    // Pins + master
    g_bus = new OTBusESP32Pro();
    if (!g_bus) {
      g_st.reason = "alloc failed";
      return;
    }

    // The attached functional 3.3.14 build and the Waveshare wiring use fixed
    // OpenTherm pins. Do not let an imported/stale LittleFS snapshot silently
    // move the transport to another GPIO.
    const int rx = kOT_RX;
    const int tx = kOT_TX;
    g_cfg.rxPin = rx;
    g_cfg.txPin = tx;

    pinMode(tx, OUTPUT);
    pinMode(rx, INPUT);

    // NOTE: OTBusESP32Pro::begin(inPin, outPin, roleMaster)
    // This is the historically most fragile call on ESP32-S3 when wiring is wrong.
    g_bus->begin(rx, tx, true /*master*/);

    // Apply OTBus control/watchdog configuration.
    {
      OTBusESP32Pro::ControlConfig cc = g_bus->controlConfig();
      cc.enabled = !cfgIsReadOnly();
      cc.sendStatus = false;
      cc.sendTSet = false;
      cc.periodMs = 1000;
      g_bus->setControlConfig(cc);

      OTBusESP32Pro::WatchdogConfig wd = g_bus->watchdogConfig();
      wd.enabled = !cfgIsReadOnly();
      wd.timeoutMs = g_cfg.watchdogTimeoutMs;
      wd.maxConsecutiveFailures = g_cfg.maxConsecutiveFailures;
      wd.disarmOnTrip = true;
      wd.sendDisableStatusOnDisarm = true;
      g_bus->setWatchdogConfig(wd);
    }

    g_inited = true;
    g_st.present = true;
    g_st.reason = "";
    g_lastPollMs = 0;

    Serial.printf("[OT] Initialized (%s, RX=%d TX=%d, master)\n", kTransportProfile, rx, tx);
  }

  static bool otReadF88(otbus::DataID id, float& outVal) {
    otbus::Frame f; f.raw = 0;
    const bool ok = g_bus->read(id, f);
    const OpenThermResponseStatus rs = g_bus->lastStatus();
    stNoteResponse(rs);
    if (responseProvesLink((uint8_t)id, rs)) markLinkAlive();
    if (!ok) return false;
    outVal = otbus::F88::decode(f.value());
    return true;
  }

  static bool otReadU16(otbus::DataID id, uint16_t& outVal, uint16_t requestValue = 0) {
    otbus::Frame f; f.raw = 0;
    const bool ok = g_bus->read(id, f, requestValue);
    const OpenThermResponseStatus rs = g_bus->lastStatus();
    stNoteResponse(rs);
    if (responseProvesLink((uint8_t)id, rs)) markLinkAlive();
    if (!ok) return false;
    outVal = f.value();
    return true;
  }

  static void decodeStatusFrame(const otbus::Frame& f) {
    const uint16_t statusRaw = f.value();
    const otbus::StatusFlags flags = otbus::StatusFlags::decode(statusRaw);
    g_st.statusRaw = statusRaw;
    g_st.masterStatusRaw = (uint8_t)((statusRaw >> 8) & 0xFF);
    g_st.slaveStatusRaw = (uint8_t)(statusRaw & 0xFF);
    g_st.otcActive = flags.otcActive;
    g_st.ch2Enable = flags.ch2Enable;
    g_st.fault = flags.fault;
    g_st.chActive = flags.chActive;
    g_st.dhwActive = flags.dhwActive;
    g_st.flameOn = flags.flameOn;
    g_st.coolingActive = flags.coolingActive;
    g_st.ch2Active = flags.ch2Active;
    g_st.diagnostic = flags.diagnostic;
    g_st.chEnable = g_reqChEnable;
    g_st.dhwEnable = g_reqDhwEnable;
    g_st.activeSource = g_activeSource;
  }

  static void finishControlSequence() {
    if (g_controlRetryNeeded) {
      g_controlDirty = true;
      g_controlStep = 0;
      g_controlNextAttemptMs = millis() + 1500UL;
      g_st.lastCmd += " RETRY";
    } else {
      g_controlDirty = false;
      g_controlStep = 0;
      g_controlNextAttemptMs = 0;
      g_st.lastCmd = String("APPLIED FLAGS=") + (g_reqChEnable ? "CH" : "") +
                     (g_reqDhwEnable ? "+DHW" : "") + " SRC=" + g_activeSource;
    }
    g_controlRetryNeeded = false;
  }

  static bool processPendingControlStep() {
    if (!g_controlDirty) return false;
    const uint32_t now = millis();
    if (g_controlNextAttemptMs && (int32_t)(now - g_controlNextAttemptMs) < 0) return false;
    if (!g_cfg.enabled || !g_cfg.autoStart || cfgIsReadOnly()) return false;
    otInitIfNeeded();
    if (!g_bus) return false;
    g_bus->loop();
    if (!g_bus->isReady()) return false;

    for (uint8_t guard = 0; guard < 5; ++guard) {
      if (g_controlStep == 0) {
        if (!isfinite(g_reqChSetpointC) || sameFloat(g_reqChSetpointC, g_appliedChSetpointC, 0.05f)) { g_controlStep++; continue; }
        const bool ok = g_bus->setCHWaterSetpoint(g_reqChSetpointC);
        const OpenThermResponseStatus rs = g_bus->lastStatus(); stNoteResponse(rs);
        if (responseProvesLink((uint8_t)otbus::DataID::ControlSetpoint_TSet, rs)) markLinkAlive();
        if (ok) g_appliedChSetpointC = g_reqChSetpointC; else g_controlRetryNeeded = true;
        g_controlStep++; g_st.lastCmdMs = millis(); return true;
      }
      if (g_controlStep == 1) {
        if (!isfinite(g_reqDhwSetpointC) || sameFloat(g_reqDhwSetpointC, g_appliedDhwSetpointC, 0.05f)) { g_controlStep++; continue; }
        otbus::Frame f; f.raw = 0;
        const bool ok = g_bus->write(otbus::DataID::DHWSetpoint, otbus::F88::encode(g_reqDhwSetpointC), f);
        const OpenThermResponseStatus rs = g_bus->lastStatus(); stNoteResponse(rs);
        if (responseProvesLink((uint8_t)otbus::DataID::DHWSetpoint, rs)) markLinkAlive();
        if (ok) g_appliedDhwSetpointC = g_reqDhwSetpointC; else g_controlRetryNeeded = true;
        g_controlStep++; g_st.lastCmdMs = millis(); return true;
      }
      if (g_controlStep == 2) {
        if (!isfinite(g_reqMaxModPct) || sameFloat(g_reqMaxModPct, g_appliedMaxModPct, 0.1f)) { g_controlStep++; continue; }
        const bool ok = g_bus->setMaxRelModulationLevel(g_reqMaxModPct);
        const OpenThermResponseStatus rs = g_bus->lastStatus(); stNoteResponse(rs);
        if (responseProvesLink((uint8_t)otbus::DataID::MaxRelModLevelSetting, rs)) markLinkAlive();
        if (ok) g_appliedMaxModPct = g_reqMaxModPct; else g_controlRetryNeeded = true;
        g_controlStep++; g_st.lastCmdMs = millis(); return true;
      }
      if (g_controlStep == 3) {
        otbus::Frame f; f.raw = 0;
        const uint16_t req = otbus::StatusFlags::encodeMaster(g_reqChEnable, g_reqDhwEnable, false, false, false);
        const bool ok = g_bus->read(otbus::DataID::Status, f, req);
        const OpenThermResponseStatus rs = g_bus->lastStatus(); stNoteResponse(rs);
        if (responseProvesLink((uint8_t)otbus::DataID::Status, rs)) markLinkAlive();
        if (ok) { decodeStatusFrame(f); g_appliedStatusValid = true; }
        else { g_appliedStatusValid = false; g_controlRetryNeeded = true; }
        g_controlStep++; g_st.lastCmdMs = millis(); finishControlSequence(); return true;
      }
      finishControlSequence(); return false;
    }
    return false;
  }

  static bool otPollStep() {
    otInitIfNeeded();
    if (!g_bus) { g_st.present = false; g_st.ready = false; if (!g_st.reason.length()) g_st.reason = "no instance"; return false; }
    g_bus->loop();
    g_st.present = true;
    g_st.ready = g_bus->isReady();
    if (!g_st.ready) { g_st.reason = "not ready"; return false; }

    switch (g_pollStep) {
      case 0: {
        g_cycleStatusSeen = false; g_cycleCoreData = false;
        const uint16_t req = otbus::StatusFlags::encodeMaster(cfgIsReadOnly() ? false : g_reqChEnable, cfgIsReadOnly() ? false : g_reqDhwEnable, false, false, false);
        otbus::Frame f; f.raw = 0;
        const bool ok = g_bus->read(otbus::DataID::Status, f, req);
        const OpenThermResponseStatus rs = g_bus->lastStatus(); stNoteResponse(rs);
        if (ok) { decodeStatusFrame(f); g_cycleStatusSeen = true; markLinkAlive(); g_st.reason = ""; }
        else {
          otbus::MessageType semanticType = otbus::MessageType::RESERVED_M2S;
          if (lastRawSemanticResponse((uint8_t)otbus::DataID::Status, &semanticType)) {
            g_cycleStatusSeen = true; markLinkAlive();
            g_st.reason = semanticType == otbus::MessageType::DATA_INVALID ? "ID0 DATA_INVALID; probing temperatures" : "ID0 UNKNOWN_DATA_ID; probing temperatures";
          } else g_st.reason = rs == OpenThermResponseStatus::TIMEOUT ? "ID0 timeout; probing ID25" : "ID0 invalid; probing ID25";
        }
        g_pollStep = 1; return true;
      }
      case 1: {
        float v=NAN; if (otReadF88(otbus::DataID::BoilerWaterTemperature_Tboiler, v) && isfinite(v)) { g_st.boilerTempC=v; g_cycleCoreData=true; markTelemetryAlive(); if(!g_cycleStatusSeen) g_st.reason="ID0 unavailable; ID25 OK"; } else g_st.boilerTempC=NAN;
        g_pollStep=2; return true;
      }
      case 2: {
        float v=NAN; if (otReadF88(otbus::DataID::ReturnWaterTemperature_Tret, v) && isfinite(v)) { g_st.returnTempC=v; g_cycleCoreData=true; markTelemetryAlive(); } else g_st.returnTempC=NAN;
        g_pollStep=3; return true;
      }
      case 3: {
        float v=NAN; if (otReadF88(otbus::DataID::DHWTemperature_Tdhw, v) && isfinite(v)) { g_st.dhwTempC=v; g_cycleCoreData=true; markTelemetryAlive(); } else g_st.dhwTempC=NAN;
        g_pollStep=4; return true;
      }
      case 4: {
        float v=NAN; if (otReadF88(otbus::DataID::RelativeModulationLevel, v) && isfinite(v)) { g_st.modulationPct=v; g_cycleCoreData=true; markTelemetryAlive(); } else g_st.modulationPct=NAN;
        g_pollStep=5; return true;
      }
      case 5: {
        float v=NAN; if (otReadF88(otbus::DataID::CHWaterPressure, v) && isfinite(v)) { g_st.pressureBar=v; g_cycleCoreData=true; markTelemetryAlive(); } else g_st.pressureBar=NAN;
        if (!g_cycleStatusSeen && !g_cycleCoreData && !linkIsFresh()) { g_st.reason="no valid OpenTherm frame"; g_st.fault=false; g_st.chActive=false; g_st.dhwActive=false; g_st.flameOn=false; }
        g_pollStep=6; return true;
      }
      case 6: {
        static const uint8_t ids[]={24,27,29,30,31,32,33,34}; const uint8_t id=ids[g_extPollIndex++ % 8];
        otbus::Frame f; f.raw=0; const bool ok=g_bus->read((otbus::DataID)id,f); const OpenThermResponseStatus rs=g_bus->lastStatus(); stNoteResponse(rs); if(responseProvesLink(id,rs)) markLinkAlive();
        float v=ok ? (id==33 ? (float)((int16_t)f.value()) : otbus::F88::decode(f.value())) : NAN;
        switch(id){case 24:g_st.roomTempC=v;break;case 27:g_st.outsideTempC=v;break;case 29:g_st.solarStorageTempC=v;break;case 30:g_st.solarCollectorTempC=v;break;case 31:g_st.ch2FlowTempC=v;break;case 32:g_st.dhw2TempC=v;break;case 33:g_st.exhaustTempC=v;break;case 34:g_st.heatExchangerTempC=v;break;}
        if(ok && isfinite(v)) markTelemetryAlive(); g_pollStep=7; return true;
      }
      case 7: {
        uint16_t raw=0; if(otReadU16(otbus::DataID::ASFflags_OEMfaultCode,raw)){g_st.faultFlags=(uint8_t)(raw>>8);g_st.oemFaultCode=(uint8_t)(raw&0xff);} g_pollStep=8; return true;
      }
      case 8: {
        const uint32_t now=millis(); if((uint32_t)(now-g_lastRemotePollMs)<5000UL){g_pollStep=0;return false;} g_lastRemotePollMs=now;
        static const uint8_t ids[]={15,48,49,56,57}; const uint8_t id=ids[g_remotePollIndex++ % 5];
        if(id==15){uint16_t raw=0;if(otReadU16((otbus::DataID)15,raw)){g_st.maxCapacityKw=(float)((uint8_t)(raw>>8));g_st.minModulationPct=(float)((uint8_t)(raw&0xff));}}
        else if(id==48){uint16_t raw=0;if(otReadU16((otbus::DataID)48,raw)){g_st.dhwBoundMaxC=(float)((int8_t)(raw>>8));g_st.dhwBoundMinC=(float)((int8_t)(raw&0xff));}}
        else if(id==49){uint16_t raw=0;if(otReadU16((otbus::DataID)49,raw)){g_st.maxChBoundMaxC=(float)((int8_t)(raw>>8));g_st.maxChBoundMinC=(float)((int8_t)(raw&0xff));}}
        else if(id==56){float v=NAN;if(otReadF88((otbus::DataID)56,v))g_st.dhwSetpointC=v;}
        else if(id==57){float v=NAN;if(otReadF88((otbus::DataID)57,v))g_st.maxChSetpointC=v;}
        g_pollStep=0; return true;
      }
      default: g_pollStep=0; return false;
    }
  }

  // ---- config parsing helpers (pins are fixed; enabled can be controlled) ----
  static void applyConfigDoc(JsonObject ot) {
    const bool oldEnabled = g_cfg.enabled;
    const bool oldAutoStart = g_cfg.autoStart;
    const String oldMode = g_cfg.mode;
    const uint32_t oldWatchdogTimeoutMs = g_cfg.watchdogTimeoutMs;
    const uint8_t oldMaxConsecutiveFailures = g_cfg.maxConsecutiveFailures;

    // enabled is configurable; autoStart/bootDelay protect against boot-loops
    g_cfg.enabled = ot["enabled"] | g_cfg.enabled;
    g_cfg.autoStart = ot["autoStart"] | ot["startOnBoot"] | g_cfg.autoStart;
    g_cfg.bootDelayMs = (uint32_t)(ot["bootDelayMs"] | (int)g_cfg.bootDelayMs);
    if (g_cfg.bootDelayMs > 120000) g_cfg.bootDelayMs = 120000;
    g_cfg.pollMs = (uint32_t)(ot["pollMs"] | (int)g_cfg.pollMs);
    if (g_cfg.pollMs < 250) g_cfg.pollMs = 250;
    if (g_cfg.pollMs > 30000) g_cfg.pollMs = 30000;

    g_cfg.boilerControl = String((const char*)(ot["boilerControl"] | g_cfg.boilerControl.c_str()));
    g_cfg.mode = String((const char*)(ot["mode"] | g_cfg.mode.c_str()));
    g_cfg.mode.trim();
    if (g_cfg.mode.equalsIgnoreCase("readonly")) g_cfg.mode = "readOnly";
    else if (!g_cfg.mode.equalsIgnoreCase("control")) g_cfg.mode = "readOnly";
    if (g_cfg.mode.equalsIgnoreCase("control")) g_cfg.boilerControl = "opentherm";
    else g_cfg.boilerControl = "relay";

    // Advanced: raw Data-ID writes (dangerous)
    if (ot.containsKey("allowRawWrite")) {
      g_cfg.allowRawWrite = (bool)(ot["allowRawWrite"] | false);
    }

    // The attached functional 3.3.14 build used the board wiring
    // TX=GPIO47 / RX=GPIO48. Keep those pins fixed even when an older
    // LittleFS snapshot contains experimental pin overrides.
    g_cfg.txPin = kOT_TX;
    g_cfg.rxPin = kOT_RX;
    // The attached older working build used fixed normal MCU-side polarity.
    // Do not let stale 3.5.1 inversion settings alter the proven transport.
    g_cfg.invertTx = false;
    g_cfg.invertRx = false;
    g_cfg.autoDetectLogic = false;

    // watchdog
    {
      JsonVariant wd = ot["watchdog"];
      const uint32_t t = (uint32_t)(ot["watchdogTimeoutMs"] | (wd["timeoutMs"] | (int)g_cfg.watchdogTimeoutMs));
      const int mf = (int)(ot["maxConsecutiveFailures"] | (wd["maxConsecutiveFailures"] | (int)g_cfg.maxConsecutiveFailures));
      g_cfg.watchdogTimeoutMs = t;
      if (g_cfg.watchdogTimeoutMs < 500) g_cfg.watchdogTimeoutMs = 500;
      if (g_cfg.watchdogTimeoutMs > 120000) g_cfg.watchdogTimeoutMs = 120000;
      int mfi = mf;
      if (mfi < 0) mfi = 0;
      if (mfi > 50) mfi = 50;
      g_cfg.maxConsecutiveFailures = (uint8_t)mfi;
    }

    g_cfg.mapEquithermChSetpoint = ot["mapEquithermChSetpoint"] | g_cfg.mapEquithermChSetpoint;
    g_cfg.mapDhw = ot["mapDhw"] | g_cfg.mapDhw;
    g_cfg.mapNightMode = ot["mapNightMode"] | g_cfg.mapNightMode;

    g_cfg.minChSetpointC = ot["minChSetpointC"] | g_cfg.minChSetpointC;
    g_cfg.maxChSetpointC = ot["maxChSetpointC"] | g_cfg.maxChSetpointC;
    g_cfg.dhwSetpointC = ot["dhwSetpointC"] | g_cfg.dhwSetpointC;
    g_cfg.dhwBoostChSetpointC = ot["dhwBoostChSetpointC"] | g_cfg.dhwBoostChSetpointC;
    g_cfg.assumedMaxBoilerKw = ot["assumedMaxBoilerKw"] | g_cfg.assumedMaxBoilerKw;

    const bool needReinit = oldEnabled != g_cfg.enabled || oldAutoStart != g_cfg.autoStart ||
                            oldMode != g_cfg.mode || oldWatchdogTimeoutMs != g_cfg.watchdogTimeoutMs ||
                            oldMaxConsecutiveFailures != g_cfg.maxConsecutiveFailures;
    if (g_inited && needReinit) otDestroy();
    if (g_cfg.enabled && g_cfg.autoStart && !cfgIsReadOnly()) {
      g_controlDirty = true; g_controlStep = 0; g_controlNextAttemptMs = 0;
    }
  }

  static void fillConfigJson(JsonObject out) {
    // Report runtime config (used by UI)
    out["enabled"] = g_cfg.enabled;
    out["autoStart"] = g_cfg.autoStart;
    out["bootDelayMs"] = g_cfg.bootDelayMs;
    out["pollMs"] = g_cfg.pollMs;
    out["boilerControl"] = g_cfg.boilerControl;
    out["mode"] = g_cfg.mode;
    out["allowRawWrite"] = g_cfg.allowRawWrite;
    out["watchdogTimeoutMs"] = g_cfg.watchdogTimeoutMs;
    out["maxConsecutiveFailures"] = g_cfg.maxConsecutiveFailures;
    out["mapEquithermChSetpoint"] = g_cfg.mapEquithermChSetpoint;
    out["mapDhw"] = g_cfg.mapDhw;
    out["mapNightMode"] = g_cfg.mapNightMode;
    out["minChSetpointC"] = g_cfg.minChSetpointC;
    out["maxChSetpointC"] = g_cfg.maxChSetpointC;
    out["dhwSetpointC"] = g_cfg.dhwSetpointC;
    out["dhwBoostChSetpointC"] = g_cfg.dhwBoostChSetpointC;
    out["assumedMaxBoilerKw"] = g_cfg.assumedMaxBoilerKw;

    out["txPin"] = g_cfg.txPin;
    out["rxPin"] = g_cfg.rxPin;
    out["invertTx"] = g_cfg.invertTx;
    out["invertRx"] = g_cfg.invertRx;
    out["autoDetectLogic"] = g_cfg.autoDetectLogic;
    out["transportProfile"] = kTransportProfile;
  }

  static void fillSourceRequestJson(JsonObject out, const OpenThermSourceRequest& req) {
    out["active"] = req.active;
    JsonObject fields = out.createNestedObject("fields");

    JsonObject f = fields.createNestedObject("chEnable");
    f["set"] = req.chEnableSet;
    if (req.chEnableSet) f["value"] = req.chEnable; else f["value"] = nullptr;

    f = fields.createNestedObject("dhwEnable");
    f["set"] = req.dhwEnableSet;
    if (req.dhwEnableSet) f["value"] = req.dhwEnable; else f["value"] = nullptr;

    f = fields.createNestedObject("chSetpointC");
    f["set"] = req.chSetpointSet;
    if (req.chSetpointSet && isfinite(req.chSetpointC)) f["value"] = req.chSetpointC; else f["value"] = nullptr;

    f = fields.createNestedObject("dhwSetpointC");
    f["set"] = req.dhwSetpointSet;
    if (req.dhwSetpointSet && isfinite(req.dhwSetpointC)) f["value"] = req.dhwSetpointC; else f["value"] = nullptr;

    f = fields.createNestedObject("maxModulationPct");
    f["set"] = req.maxModulationSet;
    if (req.maxModulationSet && isfinite(req.maxModulationPct)) f["value"] = req.maxModulationPct; else f["value"] = nullptr;
  }

  static void fillStatusJson(JsonObject out) {
    out["enabled"] = g_cfg.enabled;
    out["present"] = g_st.present;
    out["ready"] = g_st.ready;
    // Link health is intentionally separate from the boiler fault flag.
    // "ready" only means the local OpenTherm state machine is idle/usable.
    out["linkOk"] = linkIsFresh();
    out["fault"] = g_st.fault;
    out["chEnable"] = g_st.chEnable;
    out["dhwEnable"] = g_st.dhwEnable;
    out["chActive"] = g_st.chActive;
    out["dhwActive"] = g_st.dhwActive;
    out["flameOn"] = g_st.flameOn;
    out["coolingActive"] = g_st.coolingActive;
    out["diagnostic"] = g_st.diagnostic;
    out["statusRaw"] = g_st.statusRaw;
    out["masterStatusRaw"] = g_st.masterStatusRaw;
    out["slaveStatusRaw"] = g_st.slaveStatusRaw;
    out["otcActive"] = g_st.otcActive;
    out["ch2Enable"] = g_st.ch2Enable;
    out["ch2Active"] = g_st.ch2Active;

    if (isfinite(g_st.boilerTempC)) out["boilerTempC"] = g_st.boilerTempC; else out["boilerTempC"] = nullptr;
    if (isfinite(g_st.returnTempC)) out["returnTempC"] = g_st.returnTempC; else out["returnTempC"] = nullptr;
    if (isfinite(g_st.dhwTempC)) out["dhwTempC"] = g_st.dhwTempC; else out["dhwTempC"] = nullptr;

    if (isfinite(g_st.outsideTempC)) out["outsideTempC"] = g_st.outsideTempC; else out["outsideTempC"] = nullptr;
    if (isfinite(g_st.roomTempC)) out["roomTempC"] = g_st.roomTempC; else out["roomTempC"] = nullptr;
    if (isfinite(g_st.solarStorageTempC)) out["solarStorageTempC"] = g_st.solarStorageTempC; else out["solarStorageTempC"] = nullptr;
    if (isfinite(g_st.solarCollectorTempC)) out["solarCollectorTempC"] = g_st.solarCollectorTempC; else out["solarCollectorTempC"] = nullptr;
    if (isfinite(g_st.ch2FlowTempC)) out["ch2FlowTempC"] = g_st.ch2FlowTempC; else out["ch2FlowTempC"] = nullptr;
    if (isfinite(g_st.dhw2TempC)) out["dhw2TempC"] = g_st.dhw2TempC; else out["dhw2TempC"] = nullptr;
    if (isfinite(g_st.exhaustTempC)) out["exhaustTempC"] = g_st.exhaustTempC; else out["exhaustTempC"] = nullptr;
    if (isfinite(g_st.heatExchangerTempC)) out["heatExchangerTempC"] = g_st.heatExchangerTempC; else out["heatExchangerTempC"] = nullptr;
    if (isfinite(g_st.modulationPct)) out["modulationPct"] = g_st.modulationPct; else out["modulationPct"] = nullptr;
    if (isfinite(g_st.maxCapacityKw)) out["maxCapacityKw"] = g_st.maxCapacityKw; else out["maxCapacityKw"] = nullptr;
    if (isfinite(g_st.minModulationPct)) out["minModulationPct"] = g_st.minModulationPct; else out["minModulationPct"] = nullptr;
    if (isfinite(g_st.maxChSetpointC)) out["maxChSetpointC"] = g_st.maxChSetpointC; else out["maxChSetpointC"] = nullptr;
    if (isfinite(g_st.maxChBoundMinC)) out["maxChBoundMinC"] = g_st.maxChBoundMinC; else out["maxChBoundMinC"] = nullptr;
    if (isfinite(g_st.maxChBoundMaxC)) out["maxChBoundMaxC"] = g_st.maxChBoundMaxC; else out["maxChBoundMaxC"] = nullptr;
    if (isfinite(g_st.dhwSetpointC)) out["dhwSetpointC"] = g_st.dhwSetpointC; else out["dhwSetpointC"] = nullptr;
    if (isfinite(g_st.dhwBoundMinC)) out["dhwBoundMinC"] = g_st.dhwBoundMinC; else out["dhwBoundMinC"] = nullptr;
    if (isfinite(g_st.dhwBoundMaxC)) out["dhwBoundMaxC"] = g_st.dhwBoundMaxC; else out["dhwBoundMaxC"] = nullptr;

    out["faultFlags"] = (uint16_t)g_st.faultFlags;
    out["oemFaultCode"] = (uint16_t)g_st.oemFaultCode;

    if (isfinite(g_st.reqChSetpointC)) out["reqChSetpointC"] = g_st.reqChSetpointC; else out["reqChSetpointC"] = nullptr;
    if (isfinite(g_st.reqDhwSetpointC)) out["reqDhwSetpointC"] = g_st.reqDhwSetpointC; else out["reqDhwSetpointC"] = nullptr;
    if (isfinite(g_st.reqMaxModulationPct)) out["reqMaxModulationPct"] = g_st.reqMaxModulationPct; else out["reqMaxModulationPct"] = nullptr;

    out["lastUpdateMs"] = g_st.lastUpdateMs;
    out["lastCmdMs"] = g_st.lastCmdMs;
    out["okCount"] = g_st.okCount;
    out["timeoutCount"] = g_st.timeoutCount;
    out["invalidCount"] = g_st.invalidCount;
    out["reason"] = g_st.reason;
    out["lastCmd"] = g_st.lastCmd;
    out["activeSource"] = g_st.activeSource;
    out["transportProfile"] = kTransportProfile;
  }
} // namespace

void openthermInit() {
  // default config (UI compatible) – enabled + pins are forced elsewhere
  g_cfg.enabled = false;
  g_cfg.autoStart = false;
  g_cfg.bootDelayMs = 15000;
  g_cfg.pollMs = 2000;
  g_cfg.boilerControl = "relay";     // read-only by default (safe)
  g_cfg.mode = "readOnly";           // explicit

  g_cfg.mapEquithermChSetpoint = true;
  g_cfg.mapDhw = true;
  g_cfg.mapNightMode = true;

  // Default only for manual/OpenTherm commands. Equitherm requests are already
  // clamped by EquithermController according to the heating UI limits.
  g_cfg.minChSetpointC = 22.0f;
  g_cfg.maxChSetpointC = 75.0f;
  g_cfg.dhwSetpointC = 50.0f;
  g_cfg.dhwBoostChSetpointC = 10.0f;
  g_cfg.assumedMaxBoilerKw = 9.0f;

  g_cfg.txPin = kOT_TX;
  g_cfg.rxPin = kOT_RX;
  g_cfg.invertTx = false;
  g_cfg.invertRx = false;
  g_cfg.autoDetectLogic = false;
  g_cfg.pollIdsCount = 0;
  g_cfg.watchdogTimeoutMs = 3000;
  g_cfg.maxConsecutiveFailures = 3;

  resetManualReq(g_manualReq);
  clearReq(g_equithermReq);
  clearReq(g_dhwReq);
  g_activeSource = "manual";
  g_st = OpenThermStatusSnapshot{};
  g_st.present = false;
  g_st.ready = false;
  g_st.reason = "";

  // Do NOT init unless enabled+autoStart via config/UI.
  g_bootMs = millis();
}

void openthermApplyConfig(const String& configJson) {
  // Parse only "opentherm" section so this works even when configJson is large.
  StaticJsonDocument<64> filter;
  filter["opentherm"] = true;
  // Legacy compatibility: some older configs stored OpenTherm under equitherm.opentherm
  filter["equitherm"]["opentherm"] = true;

  DynamicJsonDocument doc(4096);
  DeserializationError e = deserializeJson(doc, configJson, DeserializationOption::Filter(filter));
  if (e) return;

  JsonObject root = doc.as<JsonObject>();

  JsonObject ot = root["opentherm"].as<JsonObject>();
  if (ot.isNull()) {
    JsonObject eq = root["equitherm"].as<JsonObject>();
    if (!eq.isNull()) ot = eq["opentherm"].as<JsonObject>();
  }
  if (ot.isNull()) return;

  applyConfigDoc(ot);
  if (!g_cfg.enabled || !g_cfg.autoStart) {
    g_st.present = false; g_st.ready = false; clearAllTelemetry();
    g_st.reason = g_cfg.enabled ? "paused" : "disabled";
  }
}


void openthermLoop() {
  // Enable gate
  if (!g_cfg.enabled) {
    if (g_inited) otDestroy();
    g_scan.active = false;
    g_st.present = false;
    g_st.ready = false;
    clearAllTelemetry();
    g_st.reason = "disabled";
    return;
  }

  // Safety: enabled but not allowed to auto-start.
  if (!g_cfg.autoStart) {
    if (g_inited) otDestroy();
    g_scan.active = false;
    g_st.present = false;
    g_st.ready = false;
    clearAllTelemetry();
    g_st.reason = "paused";
    return;
  }

  // Safety: boot delay window.
  if (g_bootMs && (uint32_t)(millis() - g_bootMs) < g_cfg.bootDelayMs) {
    // Keep module stopped during the delay.
    if (g_inited) otDestroy();
    g_st.present = false;
    g_st.ready = false;
    clearAllTelemetry();
    g_st.reason = "boot delay";
    return;
  }

  otInitIfNeeded();
  if (!g_bus) {
    g_st.present = false;
    g_st.ready = false;
    clearAllTelemetry();
    if (!g_st.reason.length()) g_st.reason = "init failed";
    return;
  }

  const uint32_t now = millis();

  // Non-blocking Data-ID scan (temporarily pauses regular polling)
  if (g_scan.active) {
    // keep state machine moving
    g_bus->loop();

    if (!g_bus->isReady()) {
      // wait until ready
      return;
    }

    if ((int32_t)(now - g_scan.nextStepMs) < 0) return;
    g_scan.nextStepMs = now + (uint32_t)g_scan.delayMs;

    const uint8_t id = g_scan.curId;
    if (id > 127) { g_scan.active = false; g_scan.done = true; g_scan.finishedMs = now; return; }

    otbus::Frame f; f.raw = 0;
    const bool ok = g_bus->read((otbus::DataID)id, f, 0x0000);
    const OpenThermResponseStatus rs = g_bus->lastStatus();
    stNoteResponse(rs);
    if (responseProvesLink(id, rs)) markLinkAlive();
    (void)ok;

    g_scan.respStatus[id] = (uint8_t)rs;
    g_scan.msgType[id] = (uint8_t)f.type();
    g_scan.value[id] = f.value();

    const bool supported = isSupportedByFrame(rs, f);
    if (supported && !g_scan.supported[id]) g_scan.supportedCount++;
    g_scan.supported[id] = supported;

    if (id >= g_scan.endId) {
      g_scan.active = false;
      g_scan.done = true;
      g_scan.finishedMs = now;
      return;
    }
    g_scan.curId = (uint8_t)(id + 1);
    return;
  }

  if (g_controlDirty && processPendingControlStep()) return;

  g_bus->loop();
  if (!g_bus->isReady()) return;
  if (g_pollStep == 0) {
    if ((uint32_t)(now - g_lastPollMs) < g_cfg.pollMs) return;
    g_lastPollMs = now;
  }
  (void)otPollStep();
}

OpenThermConfig openthermGetConfig() {
  return g_cfg;
}

OpenThermStatusSnapshot openthermGetStatus() {
  return g_st;
}

// Helper for /api/fast JSON payload.
void openthermFillFastJson(JsonObject& out) {
  const bool en = g_cfg.enabled;
  out["en"] = en;
  out["rd"] = en ? g_st.ready : false;
  out["lk"] = en ? linkIsFresh() : false;
  out["fl"] = en ? g_st.fault : false;
  out["ce"] = en ? g_st.chEnable : false;
  out["de"] = en ? g_st.dhwEnable : false;
  out["ca"] = en ? g_st.chActive : false;
  out["da"] = en ? g_st.dhwActive : false;
  out["fo"] = en ? g_st.flameOn : false;
  out["sr"] = en ? g_st.statusRaw : 0;

  if (en && isfinite(g_st.boilerTempC)) out["bt"] = g_st.boilerTempC; else out["bt"] = nullptr;
  if (en && isfinite(g_st.returnTempC)) out["rt"] = g_st.returnTempC; else out["rt"] = nullptr;
  if (en && isfinite(g_st.dhwTempC)) out["dt"] = g_st.dhwTempC; else out["dt"] = nullptr;

  // Extended temperatures (when supported by boiler)
  if (en && isfinite(g_st.outsideTempC)) out["ot"] = g_st.outsideTempC; else out["ot"] = nullptr;
  if (en && isfinite(g_st.roomTempC)) out["rm"] = g_st.roomTempC; else out["rm"] = nullptr;
  if (en && isfinite(g_st.solarStorageTempC)) out["ss"] = g_st.solarStorageTempC; else out["ss"] = nullptr;
  if (en && isfinite(g_st.solarCollectorTempC)) out["sc"] = g_st.solarCollectorTempC; else out["sc"] = nullptr;
  if (en && isfinite(g_st.ch2FlowTempC)) out["c2"] = g_st.ch2FlowTempC; else out["c2"] = nullptr;
  if (en && isfinite(g_st.dhw2TempC)) out["d2"] = g_st.dhw2TempC; else out["d2"] = nullptr;
  if (en && isfinite(g_st.exhaustTempC)) out["ex"] = g_st.exhaustTempC; else out["ex"] = nullptr;
  if (en && isfinite(g_st.heatExchangerTempC)) out["hx"] = g_st.heatExchangerTempC; else out["hx"] = nullptr;

  if (en && isfinite(g_st.modulationPct)) out["mt"] = g_st.modulationPct; else out["mt"] = nullptr;
  if (en && isfinite(g_st.maxCapacityKw)) out["cp"] = g_st.maxCapacityKw; else out["cp"] = nullptr;
  if (en && isfinite(g_st.pressureBar)) out["pr"] = g_st.pressureBar; else out["pr"] = nullptr;

  // Remote params
  if (en && isfinite(g_st.maxChSetpointC)) out["mx"] = g_st.maxChSetpointC; else out["mx"] = nullptr;
  if (en && isfinite(g_st.maxChBoundMinC)) out["mxl"] = g_st.maxChBoundMinC; else out["mxl"] = nullptr;
  if (en && isfinite(g_st.maxChBoundMaxC)) out["mxu"] = g_st.maxChBoundMaxC; else out["mxu"] = nullptr;
  if (en && isfinite(g_st.dhwSetpointC)) out["dw"] = g_st.dhwSetpointC; else out["dw"] = nullptr;
  if (en && isfinite(g_st.dhwBoundMinC)) out["dwl"] = g_st.dhwBoundMinC; else out["dwl"] = nullptr;
  if (en && isfinite(g_st.dhwBoundMaxC)) out["dwu"] = g_st.dhwBoundMaxC; else out["dwu"] = nullptr;

  out["ff"] = (uint16_t)(en ? g_st.faultFlags : 0);
  out["oc"] = (uint16_t)(en ? g_st.oemFaultCode : 0);

  if (en && isfinite(g_st.reqChSetpointC)) out["cs"] = g_st.reqChSetpointC; else out["cs"] = nullptr;
  if (en && isfinite(g_st.reqDhwSetpointC)) out["ds"] = g_st.reqDhwSetpointC; else out["ds"] = nullptr;
  if (en && isfinite(g_st.reqMaxModulationPct)) out["mm"] = g_st.reqMaxModulationPct; else out["mm"] = nullptr;

  out["lu"] = en ? g_st.lastUpdateMs : 0;
  out["lc"] = en ? g_st.lastCmdMs : 0;
  out["rs"] = en ? g_st.reason : String("disabled");
  out["cmd"] = en ? g_st.lastCmd : String("");
  out["src"] = en ? g_st.activeSource : String("disabled");
  out["bc"] = g_cfg.boilerControl;
}

String openthermGetStatusJson() {
  DynamicJsonDocument doc(6144);
  doc["ok"] = true;
  JsonObject s = doc.createNestedObject("status");
  JsonObject c = doc.createNestedObject("config");
  fillStatusJson(s);
  JsonObject merge = s.createNestedObject("merge");
  fillSourceRequestJson(merge.createNestedObject("manual"), g_manualReq);
  fillSourceRequestJson(merge.createNestedObject("equitherm"), g_equithermReq);
  fillSourceRequestJson(merge.createNestedObject("dhw"), g_dhwReq);
  JsonObject eff = merge.createNestedObject("effective");
  eff["activeSource"] = g_activeSource;
  eff["chEnable"] = g_reqChEnable;
  eff["dhwEnable"] = g_reqDhwEnable;
  if (isfinite(g_reqChSetpointC)) eff["chSetpointC"] = g_reqChSetpointC; else eff["chSetpointC"] = nullptr;
  if (isfinite(g_reqDhwSetpointC)) eff["dhwSetpointC"] = g_reqDhwSetpointC; else eff["dhwSetpointC"] = nullptr;
  if (isfinite(g_reqMaxModPct)) eff["maxModulationPct"] = g_reqMaxModPct; else eff["maxModulationPct"] = nullptr;
  fillConfigJson(c);
  String out;
  serializeJson(doc, out);
  return out;
}

bool openthermHandleCmdJson(const String& body, String& outErr) {
  outErr = "";
  StaticJsonDocument<512> doc;
  DeserializationError e = deserializeJson(doc, body);
  if (e || !doc.is<JsonObject>()) {
    outErr = "bad json";
    return false;
  }
  JsonObject o = doc.as<JsonObject>();

  OpenThermSourceRequest req = g_manualReq;
  if (!req.active) resetManualReq(req);

  if (o.containsKey("clearManual") && (bool)o["clearManual"]) {
    resetManualReq(req);
  }

  if (o.containsKey("clear")) {
    JsonVariant clr = o["clear"];
    if (clr.is<const char*>()) {
      clearManualField(req, String(clr.as<const char*>()));
    } else if (clr.is<JsonArray>()) {
      for (JsonVariant v : clr.as<JsonArray>()) {
        if (v.is<const char*>()) clearManualField(req, String(v.as<const char*>()));
      }
    }
  }

  if (o.containsKey("chEnable")) {
    if (o["chEnable"].isNull()) clearManualField(req, "chEnable");
    else { req.chEnableSet = true; req.chEnable = (bool)o["chEnable"]; }
  }
  if (o.containsKey("dhwEnable")) {
    if (o["dhwEnable"].isNull()) clearManualField(req, "dhwEnable");
    else { req.dhwEnableSet = true; req.dhwEnable = (bool)o["dhwEnable"]; }
  }
  if (o.containsKey("chSetpointC")) {
    if (o["chSetpointC"].isNull()) {
      clearManualField(req, "chSetpointC");
    } else {
      float v = o["chSetpointC"].as<float>();
      if (isfinite(v)) {
        clampMaybe(v, g_cfg.minChSetpointC, g_cfg.maxChSetpointC);
        req.chSetpointSet = true;
        req.chSetpointC = v;
      }
    }
  }
  if (o.containsKey("dhwSetpointC")) {
    if (o["dhwSetpointC"].isNull()) {
      clearManualField(req, "dhwSetpointC");
    } else {
      float v = o["dhwSetpointC"].as<float>();
      if (isfinite(v)) {
        req.dhwSetpointSet = true;
        req.dhwSetpointC = v;
      }
    }
  }
  if (o.containsKey("maxModulationPct")) {
    if (o["maxModulationPct"].isNull()) {
      clearManualField(req, "maxModulationPct");
    } else {
      float v = o["maxModulationPct"].as<float>();
      if (isfinite(v)) {
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        req.maxModulationSet = true;
        req.maxModulationPct = v;
      }
    }
  }

  g_manualReq = req;
  bool ok = applyEffectiveRequestToBus(outErr);

  if (o.containsKey("resetFault") && (bool)o["resetFault"]) {
    String ctlErr;
    if (!validateReadyForControl(ctlErr)) {
      if (!outErr.length()) outErr = ctlErr;
      ok = false;
    } else {
      otbus::Frame f;
      const bool ro = cfgIsReadOnly();
      const uint16_t reqFlags = otbus::StatusFlags::encodeMaster(ro ? false : g_reqChEnable, ro ? false : g_reqDhwEnable, false, false, false);
      const bool resetOk = g_bus->read(otbus::DataID::Status, f, reqFlags);
      stNoteResponse(g_bus->lastStatus());
      if (!resetOk && !outErr.length()) outErr = "reset (status) failed";
      g_st.lastCmd += "RESET ";
      ok = ok && resetOk;
    }
  }

  return ok;
}

bool openthermSetChSetpointC(float v, String& outErr) {
  OpenThermSourceRequest req = g_manualReq;
  if (!req.active) resetManualReq(req);
  if (!isfinite(v)) { outErr = "bad value"; return false; }
  clampMaybe(v, g_cfg.minChSetpointC, g_cfg.maxChSetpointC);
  req.chSetpointSet = true;
  req.chSetpointC = v;
  g_manualReq = req;
  return applyEffectiveRequestToBus(outErr);
}

bool openthermSetMaxChSetpointC(float v, String& outErr) {
  outErr = "";
  if (!g_cfg.enabled) { outErr = "disabled"; return false; }
  otInitIfNeeded();
  if (!g_bus) { outErr = "init failed"; return false; }
  if (cfgIsReadOnly()) { outErr = "readOnly"; return false; }
  if (!isfinite(v)) { outErr = "bad value"; return false; }

  // If bounds are known, clamp. Otherwise clamp to a safe range.
  float lo = isfinite(g_st.maxChBoundMinC) ? g_st.maxChBoundMinC : 10.0f;
  float hi = isfinite(g_st.maxChBoundMaxC) ? g_st.maxChBoundMaxC : 90.0f;
  clampMaybe(v, lo, hi);

  otbus::Frame f;
  const bool ok = g_bus->write((otbus::DataID)57, otbus::F88::encode(v), f);
  stNoteResponse(g_bus->lastStatus());
  if (!ok) {
    outErr = "write MaxCHSetpoint failed";
    return false;
  }
  g_st.maxChSetpointC = v;
  g_st.lastCmdMs = millis();
  g_st.lastCmd = String("MAXCH=") + String(v, 1);
  return true;
}



bool openthermSetManualRequest(const OpenThermSourceRequest& req, String& outErr) {
  const OpenThermSourceRequest prev = g_manualReq;
  g_manualReq = req;
  if (!g_manualReq.active) resetManualReq(g_manualReq);
  if (applyEffectiveRequestToBus(outErr)) return true;
  g_manualReq = prev;
  String rollbackErr;
  applyEffectiveRequestToBus(rollbackErr);
  return false;
}

bool openthermSetEquithermRequest(const OpenThermSourceRequest& req, String& outErr) {
  const OpenThermSourceRequest prev = g_equithermReq;
  g_equithermReq = req;
  if (applyEffectiveRequestToBus(outErr)) return true;
  g_equithermReq = prev;
  String rollbackErr;
  applyEffectiveRequestToBus(rollbackErr);
  return false;
}

bool openthermClearEquithermRequest() {
  clearReq(g_equithermReq);
  String err;
  return applyEffectiveRequestToBus(err);
}

bool openthermSetDhwRequest(const OpenThermSourceRequest& req, String& outErr) {
  const OpenThermSourceRequest prev = g_dhwReq;
  g_dhwReq = req;
  if (applyEffectiveRequestToBus(outErr)) return true;
  g_dhwReq = prev;
  String rollbackErr;
  applyEffectiveRequestToBus(rollbackErr);
  return false;
}

bool openthermClearDhwRequest() {
  clearReq(g_dhwReq);
  String err;
  return applyEffectiveRequestToBus(err);
}

bool openthermClearLog() {
  // Minimal build: no filesystem logging.
  return false;
}

static bool tempByAge(float value, float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  if (!g_st.present || !g_st.ready || !isfinite(value)) return false;
  const uint32_t now = millis();
  const uint32_t age = (uint32_t)(now - g_st.lastUpdateMs);
  if (outAgeMs) *outAgeMs = age;
  if (age > maxAgeMs) return false;
  outC = value;
  return true;
}

bool openthermGetBoilerTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  return tempByAge(g_st.boilerTempC, outC, maxAgeMs, outAgeMs);
}

bool openthermGetReturnTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  return tempByAge(g_st.returnTempC, outC, maxAgeMs, outAgeMs);
}

bool openthermGetDhwTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  return tempByAge(g_st.dhwTempC, outC, maxAgeMs, outAgeMs);
}

bool openthermGetOutsideTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  return tempByAge(g_st.outsideTempC, outC, maxAgeMs, outAgeMs);
}

bool openthermGetRoomTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  return tempByAge(g_st.roomTempC, outC, maxAgeMs, outAgeMs);
}

bool openthermGetSolarStorageTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  return tempByAge(g_st.solarStorageTempC, outC, maxAgeMs, outAgeMs);
}

bool openthermGetSolarCollectorTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  return tempByAge(g_st.solarCollectorTempC, outC, maxAgeMs, outAgeMs);
}

bool openthermGetCh2FlowTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  return tempByAge(g_st.ch2FlowTempC, outC, maxAgeMs, outAgeMs);
}

bool openthermGetDhw2Temp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  return tempByAge(g_st.dhw2TempC, outC, maxAgeMs, outAgeMs);
}

bool openthermGetExhaustTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  return tempByAge(g_st.exhaustTempC, outC, maxAgeMs, outAgeMs);
}

bool openthermGetHeatExchangerTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) {
  return tempByAge(g_st.heatExchangerTempC, outC, maxAgeMs, outAgeMs);
}

bool openthermScanStart(uint8_t startId, uint8_t endId, uint16_t delayMs, bool includeAll) {
  if (!g_cfg.enabled) return false;
  otInitIfNeeded();
  if (!g_bus) return false;
  if (startId > 127) startId = 127;
  if (endId > 127) endId = 127;
  if (startId > endId) { uint8_t t = startId; startId = endId; endId = t; }
  if (delayMs < 10) delayMs = 10;
  if (delayMs > 2000) delayMs = 2000;

  g_scan = ScanState{};
  g_scan.active = true;
  g_scan.done = false;
  g_scan.includeAll = includeAll;
  g_scan.startId = startId;
  g_scan.endId = endId;
  g_scan.curId = startId;
  g_scan.delayMs = delayMs;
  g_scan.startedMs = millis();
  g_scan.nextStepMs = g_scan.startedMs; // start ASAP
  g_scan.supportedCount = 0;
  return true;
}

void openthermScanStop() {
  g_scan.active = false;
}

static const char* rsToStr(uint8_t rs) {
  switch ((OpenThermResponseStatus)rs) {
    case OpenThermResponseStatus::SUCCESS: return "SUCCESS";
    case OpenThermResponseStatus::INVALID: return "INVALID";
    case OpenThermResponseStatus::TIMEOUT: return "TIMEOUT";
    case OpenThermResponseStatus::NONE: default: return "NONE";
  }
}

static const char* mtToStr(uint8_t mt) {
  switch ((otbus::MessageType)mt) {
    case otbus::MessageType::READ_ACK: return "READ_ACK";
    case otbus::MessageType::WRITE_ACK: return "WRITE_ACK";
    case otbus::MessageType::DATA_INVALID: return "DATA_INVALID";
    case otbus::MessageType::UNKNOWN_DATA_ID: return "UNKNOWN_DATA_ID";
    case otbus::MessageType::READ_DATA: return "READ_DATA";
    case otbus::MessageType::WRITE_DATA: return "WRITE_DATA";
    case otbus::MessageType::INVALID_DATA: return "INVALID_DATA";
    case otbus::MessageType::RESERVED_M2S: return "RESERVED";
    default: return "?";
  }
}

// ---- Advanced Data-ID read/write helpers (web portal) ----

static bool idIsF88(uint8_t id) {
  switch (id) {
    // Many "telemetry" values are encoded as signed f8.8
    case 1:  case 7:  case 8:  case 9:
    case 14: case 16: case 17: case 18: case 19:
    case 23: case 24:
    case 25: case 26: case 27: case 28:
    case 29: case 30: case 31: case 32:
    // NOTE: Texhaust (ID33) is s16, not f8.8.
    case 34:
    case 56: case 57: case 58:
    case 124: case 125:
      return true;
    default:
      return false;
  }
}

static const char* unitForId(uint8_t id) {
  switch (id) {
    case 7:  case 14: case 17: return "%";
    case 18: return "bar";
    case 19: return "L/min";
    case 35: return "rpm";
    case 36: return "uA";
    case 38: return "%";
    case 58: return "K";
    case 79: return "ppm";
    case 111: return "W";
    case 112: return "kWh";
    case 96: case 120: case 121: case 122: case 123: return "h";
    case 97: case 113: case 114: case 116: case 117: case 118: case 119: return "count";
    default: return "";
  }
}

static const char* valueTypeForId(uint8_t id) {
  switch (id) {
    case 0: return "flags/flags";
    case 2: case 3: return "flags/u8";
    case 5: return "flags/u8";
    case 6: return "flags/flags";

    case 10: case 11: case 12: case 13: return "u8/u8";
    case 15: case 35: return "u8/u8";
    case 21: return "u8/u8";
    case 22: return "u16";

    case 48: case 49: case 50: return "s8/s8";

    case 20: return "special";
    case 98: case 99: return "special";

    case 33: return "s16";

    default:
      // Most values are either f8.8 or u16; we distinguish f8.8 by table.
      return idIsF88(id) ? "f8.8" : "u16";
  }
}

static void fillMetaFields(JsonObject& out, uint8_t id) {
  OpenThermDataIdMeta meta{};
  openthermLookupDataIdMeta(id, meta);
  char nameBuf[8];
  const char* nm = meta.name;
  if (!nm) { snprintf(nameBuf, sizeof(nameBuf), "ID%u", (unsigned)id); nm = nameBuf; }
  out["id"] = id;
  out["name"] = nm;
  out["desc"] = meta.desc;
  out["isTemp"] = meta.isTemperature;
  out["type"] = valueTypeForId(id);

  const char* unit = unitForId(id);
  if ((!unit || !unit[0]) && meta.isTemperature) unit = "°C";
  // Only include unit when it is meaningful.
  if (unit && unit[0]) out["unit"] = unit;
}

static void fillDecodedValue(JsonObject& out, uint8_t id, uint16_t raw) {
  out["raw"] = raw;
  out["hb"] = (uint8_t)(raw >> 8);
  out["lb"] = (uint8_t)(raw & 0xFF);
  out["u16"] = (uint16_t)raw;
  out["s16"] = (int16_t)raw;

  OpenThermDataIdMeta meta{};
  openthermLookupDataIdMeta(id, meta);

  if (id == 0) {
    const otbus::StatusFlags f = otbus::StatusFlags::decode(raw);
    JsonObject st = out.createNestedObject("status");
    st["chEnable"] = f.chEnable;
    st["dhwEnable"] = f.dhwEnable;
    st["coolingEnable"] = f.coolingEnable;
    st["otcActive"] = f.otcActive;
    st["ch2Enable"] = f.ch2Enable;
    st["fault"] = f.fault;
    st["chActive"] = f.chActive;
    st["dhwActive"] = f.dhwActive;
    st["flameOn"] = f.flameOn;
    st["coolingActive"] = f.coolingActive;
    st["ch2Active"] = f.ch2Active;
    st["diagnostic"] = f.diagnostic;
  }

  if (id == 5) {
    out["asfFlags"] = (uint8_t)(raw >> 8);
    out["oemFault"] = (uint8_t)(raw & 0xFF);
  }

  if (id == 2) {
    // Master configuration flags (HB) + Master MemberID (LB)
    const uint8_t cfg = (uint8_t)(raw >> 8);
    const uint8_t member = (uint8_t)(raw & 0xFF);
    JsonObject c = out.createNestedObject("masterCfg");
    c["memberId"] = member;
    c["smartPower"] = (bool)((cfg >> 0) & 1);
    c["cfgRaw"] = cfg;
  }

  if (id == 3) {
    // Slave configuration flags (HB) + Slave MemberID (LB)
    // Bit meanings follow common OpenTherm v2.2 tables.
    const uint8_t cfg = (uint8_t)(raw >> 8);
    const uint8_t member = (uint8_t)(raw & 0xFF);
    JsonObject c = out.createNestedObject("slaveCfg");
    c["memberId"] = member;
    c["dhwPresent"] = (bool)((cfg >> 0) & 1);
    c["controlType"] = (bool)((cfg >> 1) & 1);
    c["coolingSupported"] = (bool)((cfg >> 2) & 1);
    c["dhwConfiguration"] = (bool)((cfg >> 3) & 1); // 0=instant, 1=storage
    c["masterLowOffPumpCtrl"] = (bool)((cfg >> 4) & 1);
    c["ch2Present"] = (bool)((cfg >> 5) & 1);
    c["remoteWaterFilling"] = (bool)((cfg >> 6) & 1);
    c["heatCoolModeCtrl"] = (bool)((cfg >> 7) & 1);
    c["cfgRaw"] = cfg;
  }

  if (id == 6) {
    // Remote boiler parameter transfer-enable (HB) and read/write (LB)
    const uint8_t te = (uint8_t)(raw >> 8);
    const uint8_t rw = (uint8_t)(raw & 0xFF);
    JsonObject r = out.createNestedObject("rbp");
    r["teMask"] = te;
    r["rwMask"] = rw;
    // P1..P8 bits (not every boiler uses all)
    for (uint8_t i = 0; i < 8; i++) {
      char k1[6]; // "p1TE" etc
      char k2[6]; // "p1RW" etc
      snprintf(k1, sizeof(k1), "p%uTE", (unsigned)(i + 1));
      snprintf(k2, sizeof(k2), "p%uRW", (unsigned)(i + 1));
      r[k1] = (bool)((te >> i) & 1);
      r[k2] = (bool)((rw >> i) & 1);
    }
  }

  if (id == 10) {
    // Transparent Slave Parameters supported: u8/u8
    out["tspCount"] = (uint8_t)(raw >> 8);
  }

  if (id == 11) {
    // Transparent Slave Parameter entry: u8/u8
    out["tspIndex"] = (uint8_t)(raw >> 8);
    out["tspValue"] = (uint8_t)(raw & 0xFF);
  }

  if (id == 15) {
    // u8/u8: max capacity (kW) / min modulation (%)
    out["maxCapacityKw"] = (uint8_t)(raw >> 8);
    out["minModulationPct"] = (uint8_t)(raw & 0xFF);
  }

  if (id == 20) {
    otbus::DayTime dt = otbus::DayTime::decode(raw);
    JsonObject t = out.createNestedObject("dayTime");
    t["day"] = dt.day;
    t["hour"] = dt.hour;
    t["minute"] = dt.minute;
  }

  if (id == 21) {
    otbus::DateMD d = otbus::DateMD::decode(raw);
    JsonObject t = out.createNestedObject("date");
    t["month"] = d.month;
    t["day"] = d.day;
  }

  if (id == 22) {
    out["year"] = (uint16_t)raw;
  }

  if (id == 48 || id == 49 || id == 50) {
    // s8/s8 bounds: HB=upper, LB=lower
    const int8_t upper = (int8_t)(raw >> 8);
    const int8_t lower = (int8_t)(raw & 0xFF);
    out["upper"] = (int)upper;
    out["lower"] = (int)lower;
  }

  if (id == 33) {
    // Texhaust is s16 (°C)
    out["celsius"] = (int16_t)raw;
    out["unit"] = "°C";
    return;
  }

  const bool asF88 = meta.isTemperature || idIsF88(id);
  if (asF88) {
    const float v = otbus::F88::decode(raw);
    out["f88"] = v;

    const char* unit = unitForId(id);
    if ((!unit || !unit[0]) && meta.isTemperature) unit = "°C";
    if (unit && unit[0]) out["unit"] = unit;
  }
}

String openthermReadDataIdJson(uint8_t id, uint16_t reqValue) {
  DynamicJsonDocument doc(1536);
  doc["ok"] = false;

  JsonObject meta = doc.createNestedObject("meta");
  fillMetaFields(meta, id);

  if (!g_cfg.enabled) { doc["err"] = "disabled"; String out; serializeJson(doc, out); return out; }
  if (g_scan.active) { doc["err"] = "scan_active"; String out; serializeJson(doc, out); return out; }

  otInitIfNeeded();
  if (!g_bus) { doc["err"] = "init_failed"; String out; serializeJson(doc, out); return out; }
  if (!g_bus->isReady()) { doc["err"] = "not_ready"; String out; serializeJson(doc, out); return out; }

  // For ID0 (Status), default request value is current master enable bits.
  uint16_t req = reqValue;
  if (id == 0 && reqValue == 0) {
    const bool ro = cfgIsReadOnly();
    req = otbus::StatusFlags::encodeMaster(ro ? false : g_reqChEnable, ro ? false : g_reqDhwEnable, false, false, false);
  }

  otbus::Frame f;
  f.raw = 0;
  const bool ok = g_bus->read((otbus::DataID)id, f, req);
  const OpenThermResponseStatus rs = g_bus->lastStatus();
  stNoteResponse(rs);

  doc["ok"] = ok;
  doc["rs"] = rsToStr((uint8_t)rs);
  doc["mt"] = mtToStr((uint8_t)f.type());
  doc["req"] = req;

  JsonObject val = doc.createNestedObject("val");
  fillDecodedValue(val, id, f.value());

  // Compatibility helpers for frontend callers that expect common fields at top level.
  if (id == 15) {
    doc["maxCapacityKw"] = (uint8_t)(f.value() >> 8);
    doc["minModulationPct"] = (uint8_t)(f.value() & 0xFF);
  }

  // Avoid immediate extra polling after user operation
  g_lastPollMs = millis();

  String out;
  serializeJson(doc, out);
  return out;
}

String openthermWriteDataIdJson(uint8_t id, uint16_t value) {
  DynamicJsonDocument doc(1536);
  doc["ok"] = false;

  JsonObject meta = doc.createNestedObject("meta");
  fillMetaFields(meta, id);

  if (!g_cfg.enabled) { doc["err"] = "disabled"; String out; serializeJson(doc, out); return out; }
  if (g_scan.active) { doc["err"] = "scan_active"; String out; serializeJson(doc, out); return out; }

  if (cfgIsReadOnly()) { doc["err"] = "readOnly"; String out; serializeJson(doc, out); return out; }
  if (!g_cfg.allowRawWrite) { doc["err"] = "raw_write_disabled"; String out; serializeJson(doc, out); return out; }

  otInitIfNeeded();
  if (!g_bus) { doc["err"] = "init_failed"; String out; serializeJson(doc, out); return out; }
  if (!g_bus->isReady()) { doc["err"] = "not_ready"; String out; serializeJson(doc, out); return out; }

  otbus::Frame f;
  f.raw = 0;
  const bool ok = g_bus->write((otbus::DataID)id, value, f);
  const OpenThermResponseStatus rs = g_bus->lastStatus();
  stNoteResponse(rs);

  doc["ok"] = ok;
  doc["rs"] = rsToStr((uint8_t)rs);
  doc["mt"] = mtToStr((uint8_t)f.type());
  doc["value"] = value;

  JsonObject val = doc.createNestedObject("val");
  fillDecodedValue(val, id, f.value());

  // refresh snapshot ASAP
  g_lastPollMs = 0;

  String out;
  serializeJson(doc, out);
  return out;
}

static void jsonAppendEscaped(String& out, const char* s) {
  if (!s) return;
  for (const char* p = s; *p; ++p) {
    const char c = *p;
    switch (c) {
      case '\\\\': out += "\\\\\\\\"; break;
      case '\"': out += "\\\\\""; break;
      case '\n': out += "\\\\n"; break;
      case '\r': out += "\\\\r"; break;
      case '\t': out += "\\\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          // control -> \\u00XX
          char buf[7];
          snprintf(buf, sizeof(buf), "\\\\u%04x", (unsigned)(uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }
}

static void jsonAppendStrField(String& out, const char* key, const char* val) {
  out += '\"'; out += key; out += "\":\"";
  jsonAppendEscaped(out, val ? val : "");
  out += '\"';
}

static void jsonAppendBoolField(String& out, const char* key, bool v) {
  out += '\"'; out += key; out += "\":"; out += (v ? "true" : "false");
}

static void jsonAppendNumField(String& out, const char* key, uint32_t v) {
  out += '\"'; out += key; out += "\":"; out += String(v);
}

static void jsonAppendIntField(String& out, const char* key, int32_t v) {
  out += '\"'; out += key; out += "\":"; out += String(v);
}

static void jsonAppendFloatField(String& out, const char* key, float v, uint8_t decimals = 2) {
  out += '\"'; out += key; out += "\":"; out += String(v, (unsigned int)decimals);
}

static void jsonAppendMaybeUnit(String& out, uint8_t id) {
  OpenThermDataIdMeta meta{};
  openthermLookupDataIdMeta(id, meta);
  const char* unit = unitForId(id);
  if ((!unit || !unit[0]) && meta.isTemperature) unit = "°C";
  if (unit && unit[0]) {
    out += ',';
    jsonAppendStrField(out, "unit", unit);
  }
}

String openthermScanGetStatusJson(bool includeAll) {
  // Build JSON manually to avoid large DynamicJsonDocument allocations.
  // Note: names/descriptions are short and do not contain special characters, but we still escape for safety.
  String out;
  out.reserve(2048);

  out += "{\"ok\":true,\"scan\":{";
  jsonAppendBoolField(out, "active", g_scan.active); out += ',';
  jsonAppendBoolField(out, "done", g_scan.done); out += ',';
  jsonAppendNumField(out, "startId", g_scan.startId); out += ',';
  jsonAppendNumField(out, "endId", g_scan.endId); out += ',';
  jsonAppendNumField(out, "curId", g_scan.curId); out += ',';
  jsonAppendNumField(out, "delayMs", g_scan.delayMs); out += ',';
  jsonAppendNumField(out, "startedMs", g_scan.startedMs); out += ',';
  jsonAppendNumField(out, "finishedMs", g_scan.finishedMs); out += ',';
  jsonAppendNumField(out, "supportedCount", g_scan.supportedCount); out += ',';
  jsonAppendBoolField(out, "deviceReady", (g_bus && g_bus->isReady())); out += ',';

  const bool wantAll = includeAll || g_scan.includeAll;
  out += "\"items\":[";
  bool first = true;

  for (uint8_t id = g_scan.startId; id <= g_scan.endId; id++) {
    if (!wantAll && !g_scan.supported[id]) continue;
    if (!first) out += ',';
    first = false;

    OpenThermDataIdMeta meta{};
    openthermLookupDataIdMeta(id, meta);

    char nameBuf[8];
    const char* nm = meta.name;
    if (!nm) { snprintf(nameBuf, sizeof(nameBuf), "ID%u", (unsigned)id); nm = nameBuf; }

    out += '{';
    jsonAppendNumField(out, "id", id); out += ',';
    jsonAppendStrField(out, "name", nm); out += ',';
    jsonAppendStrField(out, "desc", meta.desc); out += ',';
    jsonAppendBoolField(out, "isTemp", meta.isTemperature); out += ',';
    jsonAppendBoolField(out, "supported", g_scan.supported[id]); out += ',';
    jsonAppendStrField(out, "rs", rsToStr(g_scan.respStatus[id])); out += ',';
    jsonAppendStrField(out, "mt", mtToStr(g_scan.msgType[id])); out += ',';
    const uint16_t raw = g_scan.value[id];
    out += "\"val\":"; out += String(raw);

    // Lightweight decode hints (kept flat for memory friendliness)
    out += ','; jsonAppendNumField(out, "hb", (uint8_t)(raw >> 8));
    out += ','; jsonAppendNumField(out, "lb", (uint8_t)(raw & 0xFF));

    // f8.8 values
    OpenThermDataIdMeta meta2{};
    openthermLookupDataIdMeta(id, meta2);
    const bool asF88 = (id != 33) && (meta2.isTemperature || idIsF88(id));
    if (asF88) {
      const float v = otbus::F88::decode(raw);
      out += ','; jsonAppendFloatField(out, "f88", v, 2);
      jsonAppendMaybeUnit(out, id);
    }

    // s16 values
    if (id == 33) {
      out += ','; jsonAppendIntField(out, "s16", (int16_t)raw);
      out += ','; jsonAppendStrField(out, "unit", "°C");
    }

    // s8/s8 bounds
    if (id == 48 || id == 49 || id == 50) {
      const int8_t upper = (int8_t)(raw >> 8);
      const int8_t lower = (int8_t)(raw & 0xFF);
      out += ','; jsonAppendIntField(out, "upper", (int)upper);
      out += ','; jsonAppendIntField(out, "lower", (int)lower);
      out += ','; jsonAppendStrField(out, "unit", "°C");
    }

    // Common u8/u8 convenience decodes
    if (id == 15) {
      out += ','; jsonAppendNumField(out, "maxCapacityKw", (uint8_t)(raw >> 8));
      out += ','; jsonAppendNumField(out, "minModulationPct", (uint8_t)(raw & 0xFF));
    }
    if (id == 3) {
      out += ','; jsonAppendNumField(out, "memberId", (uint8_t)(raw & 0xFF));
      out += ','; jsonAppendNumField(out, "cfg", (uint8_t)(raw >> 8));
    }
    if (id == 6) {
      out += ','; jsonAppendNumField(out, "teMask", (uint8_t)(raw >> 8));
      out += ','; jsonAppendNumField(out, "rwMask", (uint8_t)(raw & 0xFF));
    }
    if (id == 10) {
      out += ','; jsonAppendNumField(out, "tspCount", (uint8_t)(raw >> 8));
    }
    if (id == 11) {
      out += ','; jsonAppendNumField(out, "tspIndex", (uint8_t)(raw >> 8));
      out += ','; jsonAppendNumField(out, "tspValue", (uint8_t)(raw & 0xFF));
    }
    out += '}';
  }

  out += "]}}";
  return out;
}

String openthermGetScanProfileJson() {
  String out;
  out.reserve(1536);

  out += "{\"ok\":true,\"profile\":{";
  const bool hasProfile = g_scan.done;
  jsonAppendBoolField(out, "hasProfile", hasProfile); out += ',';
  jsonAppendBoolField(out, "fromScanCache", true); out += ',';
  jsonAppendNumField(out, "startId", g_scan.startId); out += ',';
  jsonAppendNumField(out, "endId", g_scan.endId); out += ',';
  jsonAppendNumField(out, "supportedCount", g_scan.supportedCount); out += ',';
  jsonAppendNumField(out, "startedMs", g_scan.startedMs); out += ',';
  jsonAppendNumField(out, "finishedMs", g_scan.finishedMs); out += ',';
  jsonAppendBoolField(out, "deviceReady", (g_bus && g_bus->isReady())); out += ',';

  out += "\"supportedIds\":[";
  bool first = true;
  for (uint8_t id = g_scan.startId; id <= g_scan.endId; id++) {
    if (!g_scan.supported[id]) continue;
    if (!first) out += ',';
    first = false;
    out += String(id);
  }
  out += "],\"items\":[";

  first = true;
  for (uint8_t id = g_scan.startId; id <= g_scan.endId; id++) {
    if (!g_scan.supported[id]) continue;
    if (!first) out += ',';
    first = false;

    OpenThermDataIdMeta meta{};
    openthermLookupDataIdMeta(id, meta);

    char nameBuf[8];
    const char* nm = meta.name;
    if (!nm) { snprintf(nameBuf, sizeof(nameBuf), "ID%u", (unsigned)id); nm = nameBuf; }

    out += '{';
    jsonAppendNumField(out, "id", id); out += ',';
    jsonAppendStrField(out, "name", nm); out += ',';
    jsonAppendStrField(out, "desc", meta.desc); out += ',';
    jsonAppendBoolField(out, "supported", true); out += ',';
    jsonAppendStrField(out, "rs", rsToStr(g_scan.respStatus[id]));
    out += '}';
  }

  out += "]}}";
  return out;
}

#else

// Feature disabled stubs
void openthermInit() {}
void openthermLoop() {}
void openthermApplyConfig(const String&) {}
OpenThermConfig openthermGetConfig() { return OpenThermConfig{}; }
OpenThermStatusSnapshot openthermGetStatus() { return OpenThermStatusSnapshot{}; }
void openthermFillFastJson(JsonObject&) {}
String openthermGetStatusJson() { return "{}"; }
bool openthermHandleCmdJson(const String&, String& outErr) { outErr="disabled"; return false; }
bool openthermSetManualRequest(const OpenThermSourceRequest&, String& outErr) { outErr="disabled"; return false; }
bool openthermSetEquithermRequest(const OpenThermSourceRequest&, String& outErr) { outErr="disabled"; return false; }
bool openthermClearEquithermRequest() { return false; }
bool openthermSetDhwRequest(const OpenThermSourceRequest&, String& outErr) { outErr="disabled"; return false; }
bool openthermClearDhwRequest() { return false; }

bool openthermClearLog() {
  // Minimal build: no filesystem logging.
  return false;
}
bool openthermGetBoilerTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) { (void)outC; (void)maxAgeMs; (void)outAgeMs; return false; }
bool openthermGetReturnTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) { (void)outC; (void)maxAgeMs; (void)outAgeMs; return false; }
bool openthermGetDhwTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) { (void)outC; (void)maxAgeMs; (void)outAgeMs; return false; }
bool openthermGetOutsideTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) { (void)outC; (void)maxAgeMs; (void)outAgeMs; return false; }
bool openthermGetRoomTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) { (void)outC; (void)maxAgeMs; (void)outAgeMs; return false; }
bool openthermGetSolarStorageTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) { (void)outC; (void)maxAgeMs; (void)outAgeMs; return false; }
bool openthermGetSolarCollectorTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) { (void)outC; (void)maxAgeMs; (void)outAgeMs; return false; }
bool openthermGetCh2FlowTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) { (void)outC; (void)maxAgeMs; (void)outAgeMs; return false; }
bool openthermGetDhw2Temp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) { (void)outC; (void)maxAgeMs; (void)outAgeMs; return false; }
bool openthermGetExhaustTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) { (void)outC; (void)maxAgeMs; (void)outAgeMs; return false; }
bool openthermGetHeatExchangerTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs) { (void)outC; (void)maxAgeMs; (void)outAgeMs; return false; }

bool openthermScanStart(uint8_t, uint8_t, uint16_t, bool) { return false; }
void openthermScanStop() {}
String openthermScanGetStatusJson(bool) { return "{}"; }
String openthermGetScanProfileJson() { return "{\"ok\":true,\"profile\":{\"hasProfile\":false,\"supportedIds\":[],\"items\":[]}}"; }

String openthermReadDataIdJson(uint8_t, uint16_t) { return "{}"; }
String openthermWriteDataIdJson(uint8_t, uint16_t) { return "{}"; }

#endif // FEATURE_OPENTHERM
