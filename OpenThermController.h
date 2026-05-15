#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// OpenTherm module (Ihor Melnyk library backend).
// Provides:
//  - periodic polling of boiler via OpenTherm adapter
//  - REST endpoints (wired via WebServerController)
//  - SSE "fast" snapshot (fast.ot)
//  - temperature sources for LogicController: opentherm_boiler / opentherm_return

struct OpenThermConfig {
  bool enabled = false;
  // Safety gate: when enabled=true but autoStart=false, the module stays idle
  // (no interrupts/timing started). This prevents boot-loops on systems where
  // OpenTherm wiring/adapter is not present or causes low-level faults.
  bool autoStart = false;

  // If autoStart is enabled, delay initialization after boot so UI/network can
  // come up first (gives time to disable OpenTherm if needed).
  uint32_t bootDelayMs = 15000;
  uint32_t pollMs = 2000;

  // "relay" = read-only, "hybrid" / "opentherm" reserved for future mapping.
  String boilerControl = "relay";

  // OTBus mode: "readOnly" or "control"
  String mode = "control";

  // Advanced OT (dangerous): allow raw Data-ID writes via web UI/API.
  // Default false.
  bool allowRawWrite = false;

  // OTBus control watchdog (only active when mode=="control")
  uint32_t watchdogTimeoutMs = 3000;
  uint8_t maxConsecutiveFailures = 3;

  bool mapEquithermChSetpoint = true;
  bool mapDhw = true;
  bool mapNightMode = true;

  float minChSetpointC = 22.0f;
  float maxChSetpointC = 75.0f;
  float dhwSetpointC = 50.0f;
  float dhwBoostChSetpointC = 10.0f;

  // Pressure monitoring (bar). If pressure is below minPressureBar, UI can warn.
  float minPressureBar = 0.8f;
  float maxPressureBar = 3.0f;
  float assumedMaxBoilerKw = 9.0f;

  int txPin = -1; // controller -> adapter IN
  int rxPin = -1; // adapter OUT -> controller (interrupt)
  bool invertTx = false;
  bool invertRx = false;
  bool autoDetectLogic = true;

  // Optional additional poll list (Data-IDs) for future extension.
  uint8_t pollIds[32] = {0};
  uint8_t pollIdsCount = 0;
};

struct OpenThermSourceRequest {
  bool active = false;
  bool chEnableSet = false;
  bool chEnable = false;
  bool dhwEnableSet = false;
  bool dhwEnable = false;
  bool chSetpointSet = false;
  float chSetpointC = NAN;
  bool dhwSetpointSet = false;
  float dhwSetpointC = NAN;
  bool maxModulationSet = false;
  float maxModulationPct = NAN;
};

struct OpenThermStatusSnapshot {
  bool present = false; // module enabled and initialized
  bool ready = false;   // library ready

  bool fault = false;
  bool chEnable = false;
  bool dhwEnable = false;
  bool chActive = false;
  bool dhwActive = false;
  bool flameOn = false;
  bool coolingActive = false;
  bool diagnostic = false;

  uint16_t statusRaw = 0;
  uint8_t masterStatusRaw = 0;
  uint8_t slaveStatusRaw = 0;
  bool otcActive = false;
  bool ch2Enable = false;
  bool ch2Active = false;

  float boilerTempC = NAN;
  float returnTempC = NAN;
  float dhwTempC = NAN;
  // Optional/extended temperatures (read when available)
  float outsideTempC = NAN;
  float roomTempC = NAN;
  float solarStorageTempC = NAN;
  float solarCollectorTempC = NAN;
  float ch2FlowTempC = NAN;
  float dhw2TempC = NAN;
  float exhaustTempC = NAN;
  float heatExchangerTempC = NAN;
  float modulationPct = NAN;
  float pressureBar = NAN;

  // Remote parameter: Max CH setpoint (ID57) and its bounds (ID49)
  float maxChSetpointC = NAN;
  float maxChBoundMinC = NAN;
  float maxChBoundMaxC = NAN;

  // Remote parameter: DHW setpoint bounds (ID48) + DHW setpoint (ID56)
  float dhwSetpointC = NAN;
  float dhwBoundMinC = NAN;
  float dhwBoundMaxC = NAN;

  uint16_t faultFlags = 0;
  uint8_t  oemFaultCode = 0;

  float reqChSetpointC = NAN;
  float reqDhwSetpointC = NAN;
  float reqMaxModulationPct = NAN;

  uint32_t lastUpdateMs = 0;
  uint32_t lastCmdMs = 0;
  uint32_t okCount = 0;
  uint32_t timeoutCount = 0;
  uint32_t invalidCount = 0;

  String reason;
  String lastCmd;
  String activeSource;
};

void openthermInit();
void openthermLoop();
void openthermApplyConfig(const String& configJson);

OpenThermConfig openthermGetConfig();
OpenThermStatusSnapshot openthermGetStatus();

// Helper for /api/fast JSON payload.
void openthermFillFastJson(JsonObject& out);

// REST helpers.
String openthermGetStatusJson();
bool openthermHandleCmdJson(const String& body, String& outErr);

// Centralized request API for internal modules.
// Merge policy: manual = base defaults / service override per field,
// equitherm = heating layer, dhw = highest-priority layer.
bool openthermSetManualRequest(const OpenThermSourceRequest& req, String& outErr);
bool openthermSetEquithermRequest(const OpenThermSourceRequest& req, String& outErr);
bool openthermClearEquithermRequest();
bool openthermSetDhwRequest(const OpenThermSourceRequest& req, String& outErr);
bool openthermClearDhwRequest();

// Programmatic helpers (used by Ekviterm)
bool openthermSetChSetpointC(float v, String& outErr);
bool openthermSetMaxChSetpointC(float v, String& outErr);
bool openthermClearLog();

// Advanced Data-ID access (used by "OT Advanced" web panel)
String openthermReadDataIdJson(uint8_t id, uint16_t reqValue = 0);
String openthermWriteDataIdJson(uint8_t id, uint16_t value);

// Temperature source helpers.
bool openthermGetBoilerTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs);
bool openthermGetReturnTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs);
bool openthermGetDhwTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs);

// Extended temperature sources (may be NAN / unavailable on some boilers)
bool openthermGetOutsideTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs);
bool openthermGetRoomTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs);
bool openthermGetSolarStorageTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs);
bool openthermGetSolarCollectorTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs);
bool openthermGetCh2FlowTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs);
bool openthermGetDhw2Temp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs);
bool openthermGetExhaustTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs);
bool openthermGetHeatExchangerTemp(float& outC, uint32_t maxAgeMs, uint32_t* outAgeMs);

// Data-ID discovery (scan) helpers.
// Non-blocking scan; results are cached in RAM until next start.
bool openthermScanStart(uint8_t startId, uint8_t endId, uint16_t delayMs, bool includeAll);
void openthermScanStop();
String openthermScanGetStatusJson(bool includeAll);
String openthermGetScanProfileJson();