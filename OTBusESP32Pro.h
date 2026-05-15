#pragma once
/*
  OTBusESP32Pro - OpenTherm helper library for ESP32

  This library wraps the proven OpenTherm implementation from:
  https://github.com/ihormelnyk/OpenTherm (MIT)

  Additions in OTBusESP32Pro:
  - Strongly typed OpenTherm v2.2 Data-IDs and helpers (encode/decode f8.8, u16, s16, flags, date/time)
  - Generic read/write API for any Data-ID
  - Convenience getters for commonly used boiler values
*/

#include <Arduino.h>
#include <stdint.h>
#include "OpenTherm.h"

namespace otbus {

// OpenTherm v2.2 message types (DataLink layer) (see protocol v2.2, section 4.2.2)
enum class MessageType : uint8_t {
  // Master->Slave
  READ_DATA     = 0b000,
  WRITE_DATA    = 0b001,
  INVALID_DATA  = 0b010,
  RESERVED_M2S  = 0b011,
  // Slave->Master
  READ_ACK      = 0b100,
  WRITE_ACK     = 0b101,
  DATA_INVALID  = 0b110,
  UNKNOWN_DATA_ID = 0b111
};

// OpenTherm v2.2 Data-IDs (Application layer) (see protocol v2.2, section 5.4)
enum class DataID : uint8_t {
  Status                         = 0,
  ControlSetpoint_TSet            = 1,
  MasterConfig_MemberID           = 2,
  SlaveConfig_MemberID            = 3,
  RemoteCommand                   = 4,
  ASFflags_OEMfaultCode           = 5,
  RBPflags                        = 6,
  CoolingControl                  = 7,
  ControlSetpoint_CH2_TsetCH2     = 8,
  RemoteOverrideRoomSetpoint      = 9,
  TransparentSlaveParametersCount = 10,
  TransparentSlaveParameter       = 11,
  FaultHistoryBufferSize          = 12,
  FaultHistoryBufferEntry         = 13,
  MaxRelModLevelSetting           = 14,
  MaxCapacity_MinModLevel         = 15,
  RoomSetpoint_TrSet              = 16,
  RelativeModulationLevel         = 17,
  CHWaterPressure                 = 18,
  DHWFlowRate                     = 19,
  DayOfWeek_TimeOfDay             = 20,
  Date                            = 21,
  Year                            = 22,
  RoomSetpoint_CH2                = 23,
  RoomTemperature_Tr              = 24,
  BoilerWaterTemperature_Tboiler  = 25,
  DHWTemperature_Tdhw             = 26,
  OutsideTemperature_Toutside     = 27,
  ReturnWaterTemperature_Tret     = 28,
  SolarStorageTemperature_Tstorage= 29,
  SolarCollectorTemperature_Tcollector = 30,
  FlowTemperature_CH2             = 31,
  DHW2Temperature_Tdhw2           = 32,
  ExhaustTemperature_Texhaust     = 33,

  DHWSetpoint_UB_LB               = 48,
  MaxCHSetpoint_UB_LB             = 49,

  DHWSetpoint                     = 56,
  MaxCHWaterSetpoint              = 57,

  RemoteOverrideFunction          = 100,
  OEMDiagnosticCode               = 115,
  BurnerStarts                    = 116,
  CHPumpStarts                    = 117,
  DHWPumpValveStarts              = 118,
  DHWBurnerStarts                 = 119,
  BurnerOperationHours            = 120,
  CHPumpOperationHours            = 121,
  DHWPumpValveOperationHours      = 122,
  DHWBurnerOperationHours         = 123,
  OpenThermVersion_Master         = 124,
  OpenThermVersion_Slave          = 125,
  MasterProductVersionType        = 126,
  SlaveProductVersionType         = 127,
};

// Common value codecs (v2.2, section 5.1)
struct F88 {
  static uint16_t encode(float v) {
    // signed f8.8 (two's complement), 1/256
    int32_t raw = (int32_t)lroundf(v * 256.0f);
    if (raw < -32768) raw = -32768;
    if (raw > 32767) raw = 32767;
    return (uint16_t)(raw & 0xFFFF);
  }
  static float decode(uint16_t raw) {
    int16_t s = (int16_t)raw;
    return ((float)s) / 256.0f;
  }
};

inline uint16_t u16(uint16_t v) { return v; }
inline uint16_t s16(int16_t v) { return (uint16_t)v; }
inline uint16_t u8u8(uint8_t hb, uint8_t lb) { return (uint16_t)(((uint16_t)hb << 8) | lb); }
inline uint8_t hb(uint16_t v) { return (uint8_t)(v >> 8); }
inline uint8_t lb(uint16_t v) { return (uint8_t)(v & 0xFF); }

// Parsed frame helper (DataLink layer, section 4.2)
struct Frame {
  uint32_t raw = 0;

  bool parityEven() const { return OpenTherm::parity(raw); }
  MessageType type() const { return (MessageType)OpenTherm::getMessageType(raw); }
  DataID id() const { return (DataID)OpenTherm::getDataID(raw); }
  uint16_t value() const { return (uint16_t)(raw & 0xFFFF); }
};

// OpenTherm status flags for Data-ID 0 (v2.2, section 5.3.1)
struct StatusFlags {
  // master status bits (HB)
  bool chEnable=false;
  bool dhwEnable=false;
  bool coolingEnable=false;
  bool otcActive=false;
  bool ch2Enable=false;

  // slave status bits (LB)
  bool fault=false;
  bool chActive=false;
  bool dhwActive=false;
  bool flameOn=false;
  bool coolingActive=false;
  bool ch2Active=false;
  bool diagnostic=false;

  static StatusFlags decode(uint16_t raw) {
    StatusFlags f;
    uint8_t m = hb(raw);
    uint8_t s = lb(raw);
    f.chEnable = (m >> 0) & 1;
    f.dhwEnable = (m >> 1) & 1;
    f.coolingEnable = (m >> 2) & 1;
    f.otcActive = (m >> 3) & 1;
    f.ch2Enable = (m >> 4) & 1;

    f.fault = (s >> 0) & 1;
    f.chActive = (s >> 1) & 1;
    f.dhwActive = (s >> 2) & 1;
    f.flameOn = (s >> 3) & 1;
    f.coolingActive = (s >> 4) & 1;
    f.ch2Active = (s >> 5) & 1;
    f.diagnostic = (s >> 6) & 1;
    return f;
  }

  static uint16_t encodeMaster(bool chEnable, bool dhwEnable=false, bool coolingEnable=false, bool otcActive=false, bool ch2Enable=false) {
    uint8_t m = 0;
    if (chEnable) m |= (1<<0);
    if (dhwEnable) m |= (1<<1);
    if (coolingEnable) m |= (1<<2);
    if (otcActive) m |= (1<<3);
    if (ch2Enable) m |= (1<<4);
    return u8u8(m, 0);
  }
};

struct DayTime {
  // day: 0=no info, 1=Mon ... 7=Sun
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;

  static DayTime decode(uint16_t raw) {
    DayTime dt;
    uint8_t h = hb(raw);
    dt.day = (h >> 5) & 0x07;
    dt.hour = h & 0x1F;
    dt.minute = lb(raw);
    return dt;
  }

  static uint16_t encode(uint8_t day, uint8_t hour, uint8_t minute) {
    uint8_t h = (uint8_t)(((day & 0x07) << 5) | (hour & 0x1F));
    return u8u8(h, minute);
  }
};

struct DateMD {
  uint8_t month=1;
  uint8_t day=1;

  static DateMD decode(uint16_t raw) { return DateMD{hb(raw), lb(raw)}; }
  static uint16_t encode(uint8_t month, uint8_t day) { return u8u8(month, day); }
};

class OTBusESP32Pro {
public:
  OTBusESP32Pro();

  // roleMaster=true => OpenTherm Master (room controller), roleMaster=false => Slave (boiler)
  void begin(int inPin, int outPin, bool roleMaster = true);

  // Must be called often from loop()
  void loop();

  // Generic API (Master mode)
  bool isReady() const;


  // Control mode: default is read-only. When enabled, library will periodically
  // exchange status (ID0) and optionally write control setpoint (ID1).
  struct ControlConfig {
    bool enabled = false;         // read-only by default
    bool chEnable = true;         // Master CH enable bit (ID0)
    bool dhwEnable = true;        // Master DHW enable bit (ID0)
    bool sendStatus = true;       // send READ-DATA ID0 with master status
    bool sendTSet = true;         // send WRITE-DATA ID1 (Control setpoint)
    float tSetC = 35.0f;          // Control setpoint in °C (ID1, f8.8)
    uint16_t periodMs = 1000;     // must be <= 1000ms per spec recommendation
  };


// Safety watchdogs:
// - Control watchdog: if enabled control mode stops receiving valid responses, control is automatically disabled.
// - Optional hardware watchdog (ESP32): resets MCU if loop() stops running.
struct WatchdogConfig {
  bool enabled = true;                 // enabled by default (only acts when control is enabled)
  uint32_t timeoutMs = 3000;           // no SUCCESS response within this window => watchdog trips
  uint8_t maxConsecutiveFailures = 3;  // consecutive TIMEOUT/INVALID => watchdog trips
  bool disarmOnTrip = true;            // disable control when watchdog trips
  bool sendDisableStatusOnDisarm = true; // best-effort: send status exchange with CH/DHW disabled once
};

void setWatchdogConfig(const WatchdogConfig &cfg);
WatchdogConfig watchdogConfig() const { return _wd; }

// ESP32 hardware watchdog (optional). When enabled, loop() will regularly feed the WDT.
// If the MCU locks up, it will reset (after timeoutSeconds), and since control is read-only by default,
// this is a safe recovery path.
bool enableHardwareWatchdog(uint32_t timeoutSeconds = 5);
void disableHardwareWatchdog();
bool hardwareWatchdogEnabled() const { return _hwWdtEnabled; }

  void setControlConfig(const ControlConfig &cfg);
  ControlConfig controlConfig() const { return _ctrl; }
  bool controlEnabled() const { return _ctrl.enabled; }

  // Convenience setters
  void enableControl(bool enable);
  void setCHEnable(bool enable);
  void setDHWEnable(bool enable);
  void setTSet(float celsius);


  // Read (Master->Slave): returns true if SUCCESS, stores response frame in outFrame
  bool read(DataID id, Frame &outFrame, uint16_t requestValue = 0x0000);

  // Write (Master->Slave): returns true if SUCCESS/WRITE_ACK, stores response frame in outFrame
  bool write(DataID id, uint16_t value, Frame &outFrame);

  // Convenience getters (Master mode)
  bool getStatus(StatusFlags &out);
  bool getBoilerTemperature(float &outCelsius);
  bool getDHWTemperature(float &outCelsius);
  bool getOutsideTemperature(float &outCelsius);
  bool getRelativeModulationLevel(float &outPercent);

  // Convenience setters (Master mode)
  bool setMasterStatus(bool chEnable, bool dhwEnable=false, bool coolingEnable=false, bool otcActive=false, bool ch2Enable=false);
  bool setCHWaterSetpoint(float celsius);
  bool setMaxRelModulationLevel(float percent);


// Response status of the last request (Master/Slave)
OpenThermResponseStatus lastStatus() const;

// Low-level request helper (Master mode). Returns true only when outStatus == SUCCESS.
bool request(MessageType type, DataID id, uint16_t value, Frame &outFrame, OpenThermResponseStatus &outStatus);

// Bulk scan Data-IDs (Master mode):
// Sends READ-DATA for each Data-ID in [startId, endId] and prints a one-line report to 'out'.
// Tip: use a larger delayMs (200..500) for slower boilers.
void scanDataIDs(Stream &out, uint8_t startId = 0, uint8_t endId = 127, uint16_t delayMs = 200, bool printAll = true);

  // After scanDataIDs(), implemented Data-IDs are stored internally.
  bool isImplemented(uint8_t id) const;
  uint8_t implementedCount() const;
  uint8_t implementedAt(uint8_t index) const; // 0..implementedCount()-1

  // Print a compact "status" report for all implemented IDs (reads them live).
  // perIdDelayMs: delay between reads (0 disables delays).
  void printStatusReport(Stream &out, uint16_t perIdDelayMs = 50);

  // Decode + print a single response frame for given Data-ID (human readable when known).
  void printDecoded(Stream &out, uint8_t id, const Frame &f) const;


  // Raw access
  uint32_t lastResponseRaw() const;

private:
  bool roleMaster = true;
  OpenTherm *ot = nullptr;

  // internal
  static void IRAM_ATTR isrRouter();
  void handleInterrupt();
  static OTBusESP32Pro* instance;

  volatile bool _interruptAttached = false;


  // Control mode state (read-only by default)
  ControlConfig _ctrl;
  uint32_t _lastCtrlMs = 0;

// Watchdog state
WatchdogConfig _wd;
uint32_t _lastSuccessMs = 0;
uint8_t _consecutiveFailures = 0;
bool _watchdogTripped = false;

// Hardware watchdog (ESP32)
bool _hwWdtEnabled = false;


  // Implemented IDs discovered by scanDataIDs()
  bool _implemented[128] = {false};
  uint8_t _implementedCount = 0;
};

} // namespace otbus

// Backward compatible global include (example uses OTBusESP32Pro without namespace)
using OTBusESP32Pro = otbus::OTBusESP32Pro;
