#pragma once
#include <Arduino.h>

enum class OpenThermBoilerControl : uint8_t {
  RELAY = 0,     // keep boiler relay control, OT is read-only
  HYBRID = 1,    // OT setpoints + relay enable (for legacy boilers)
  OPENTHERM = 2  // full OT control (CH/DHW enable via OT status)
};

struct OpenThermConfig {
  bool enabled = false;

  // Adapter wiring:
  // - txPin -> adapter IN (ESP32 drives opto LED)
  // - rxPin -> adapter OUT (ESP32 reads opto output)
  int8_t txPin = 47;   // default per Waveshare suggestion (GPIO47)
  int8_t rxPin = 48;   // default per Waveshare suggestion (GPIO48)

  bool invertTx = false;
  bool invertRx = false;
  bool autoDetectLogic = true;

  uint16_t pollMs = 1000;         // overall poll period
  uint16_t interMessageGapMs = 110;
  uint16_t slaveTimeoutMs = 900;
  uint8_t retries = 2;

  // High-level integration
  OpenThermBoilerControl boilerControl = OpenThermBoilerControl::RELAY;
  bool mapEquithermChSetpoint = true;
  bool mapDhw = true;
  bool mapNightMode = true;

  float minChSetpointC = 20.0f;
  float maxChSetpointC = 60.0f;

  float dhwSetpointC = 50.0f;
  float dhwBoostChSetpointC = 0.0f;

  // Optional: a "remote room setpoint" (ID16) for boilers that support it.
  // If NAN, controller won't write it.
  float roomSetpointC = NAN;

  // Optional: cap for CH max water setpoint (ID57) for boilers that support it.
  // If NAN, controller won't write it.
  float maxChWaterSetpointC = NAN;

  // Heat/power estimation fallback
  float assumedMaxBoilerKw = 9.0f;

  // Poll list (Data-IDs)
  uint8_t pollIds[32] = {0, 17, 18, 19, 25, 28, 56, 57};
  uint8_t pollIdCount = 8;

  // Optional CSV log to LittleFS (/opentherm.csv)
  bool logEnabled = false;
  uint32_t logIntervalMs = 60000;
};

struct OpenThermRawValue {
  uint8_t id = 0;
  uint8_t msgType = 0;
  uint16_t u16 = 0;
  float f88 = NAN;
  bool valid = false;
  uint32_t tsMs = 0;
};

struct OpenThermStatus {
  bool present = true; // compiled in
  bool ready = false;

  bool fault = false;

  // From ID0 status exchange
  bool chEnable = false;
  bool dhwEnable = false;
  bool coolingEnable = false;
  bool otcActive = false;
  bool ch2Enable = false;

  bool chActive = false;
  bool dhwActive = false;
  bool flameOn = false;
  bool coolingActive = false;
  bool ch2Active = false;
  bool diagnostic = false;

  // Common numeric values
  float boilerTempC = NAN;      // ID25
  float returnTempC = NAN;      // ID28
  float dhwTempC = NAN;         // ID26
  float roomTempC = NAN;        // ID24
  float outdoorTempC = NAN;     // ID27

  float roomSetpointC = NAN;        // ID16 (if polled)
  float maxChWaterSetpointC = NAN;  // ID57 (if polled)
  float dhwSetpointC = NAN;         // ID56 (if polled)
  float modulationPct = NAN;    // ID17
  float pressureBar = NAN;      // ID18
  float flowRateLpm = NAN;      // ID19

  float powerKw = NAN;          // derived
  float maxCapacityKw = NAN;    // ID15 HB
  float minModulationPct = NAN; // ID15 LB

  // Fault detail (ID5)
  uint8_t faultFlags = 0;
  uint8_t oemFaultCode = 0;

  // Pending requests / last command state
  float reqChSetpointC = NAN;
  float reqDhwSetpointC = NAN;
  float reqMaxModulationPct = NAN;

  float reqRoomSetpointC = NAN;
  float reqMaxChWaterSetpointC = NAN;

  uint32_t lastUpdateMs = 0;
  uint32_t lastCmdMs = 0;

  // Link quality counters (since boot)
  uint32_t totalCount = 0;
  uint32_t okCount = 0;
  uint32_t timeoutCount = 0;
  uint32_t frameErrorCount = 0;
  uint32_t parityErrorCount = 0;
  uint32_t badResponseCount = 0;
  uint32_t notInitializedCount = 0;
  uint32_t isrOverflowCount = 0; // edge buffer overflow in OT library

  String reason = "";
  String lastCmd = "";
};

void openthermInit();
void openthermLoop();

OpenThermConfig openthermGetConfig();
OpenThermStatus openthermGetStatus();

void openthermApplyConfig(const String& json);

uint8_t openthermGetRaw(OpenThermRawValue* out, uint8_t maxOut);

String openthermGetProtocolJson();

// Commands (queued, sent in loop)
void openthermCmdSetEnable(bool chEnable, bool dhwEnable);
void openthermCmdSetChSetpoint(float tC);
void openthermCmdSetDhwSetpoint(float tC);
void openthermCmdSetMaxModulation(float pct);
void openthermCmdSetRoomSetpoint(float tC);
void openthermCmdSetMaxChWaterSetpoint(float tC);
void openthermCmdResetFault();

// Direct transactions (immediate; mainly for diagnostics)
bool openthermRead(uint8_t dataId, uint16_t* outU16, uint8_t* outMsgType);
bool openthermWrite(uint8_t dataId, uint16_t value, uint8_t* outMsgType);

// Maintenance helpers
bool openthermClearLog();
String openthermGetLogPath();
