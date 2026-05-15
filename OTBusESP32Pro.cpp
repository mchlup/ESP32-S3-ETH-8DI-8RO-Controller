#include "OTBusESP32Pro.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <string.h>
#endif

using namespace otbus;

OTBusESP32Pro* OTBusESP32Pro::instance = nullptr;

OTBusESP32Pro::OTBusESP32Pro() {}

void OTBusESP32Pro::begin(int inPin, int outPin, bool roleMaster)
{
  this->roleMaster = roleMaster;
  if (ot) {
    delete ot;
    ot = nullptr;
  }
  // OpenTherm constructor expects isSlave flag
  ot = new OpenTherm(inPin, outPin, !roleMaster);
  instance = this;
  _lastSuccessMs = millis();
  _consecutiveFailures = 0;
  _watchdogTripped = false;

  // Use internal interrupt handler
  ot->begin([](){
    if (OTBusESP32Pro::instance) OTBusESP32Pro::instance->handleInterrupt();
  });
}

void OTBusESP32Pro::loop()
{
  if (!ot) return;
  ot->process();

  // Feed ESP32 task watchdog (if enabled)
#if defined(ARDUINO_ARCH_ESP32)
  if (_hwWdtEnabled) {
    (void)esp_task_wdt_reset();
  }
#endif

  // Control watchdog: if enabled control mode is not getting valid responses, disable control.
  if (roleMaster && _ctrl.enabled && _wd.enabled && ot->isReady()) {
    const uint32_t nowWd = millis();
    const bool timedOut = (_lastSuccessMs != 0) && ((uint32_t)(nowWd - _lastSuccessMs) > _wd.timeoutMs);
    const bool tooManyFails = (_consecutiveFailures >= _wd.maxConsecutiveFailures) && (_wd.maxConsecutiveFailures > 0);
    if (!_watchdogTripped && (timedOut || tooManyFails)) {
      _watchdogTripped = true;
      if (_wd.sendDisableStatusOnDisarm) {
        // Best-effort: send status exchange with CH/DHW disabled (some boilers detect regulator presence via ID0/ID1 activity)
        Frame st;
        OpenThermResponseStatus s;
        (void)request(MessageType::READ_DATA, (DataID)0, StatusFlags::encodeMaster(false, false), st, s);
      }
      if (_wd.disarmOnTrip) {
        _ctrl.enabled = false; // back to read-only
      }
    }
  }


  // Optional control mode (default: disabled / read-only)
  if (!roleMaster) return;
  if (!_ctrl.enabled) return;
  if (!ot->isReady()) return;

  const uint32_t now = millis();
  const uint16_t period = (_ctrl.periodMs < 200) ? 200 : _ctrl.periodMs;
  if ((uint32_t)(now - _lastCtrlMs) < period) return;
  _lastCtrlMs = now;

  // 1) Status exchange (ID 0)
  if (_ctrl.sendStatus) {
    Frame st;
    OpenThermResponseStatus s;
    (void)request(MessageType::READ_DATA, (DataID)0, StatusFlags::encodeMaster(_ctrl.chEnable, _ctrl.dhwEnable), st, s);
  }

  // 2) Control setpoint (ID 1)
  if (_ctrl.sendTSet) {
    Frame ack;
    OpenThermResponseStatus s;
    const uint16_t v = F88::encode(_ctrl.tSetC);
    (void)request(MessageType::WRITE_DATA, (DataID)1, v, ack, s);
  }
}

bool OTBusESP32Pro::isReady() const
{
  return ot && ot->isReady();
}


void OTBusESP32Pro::setControlConfig(const ControlConfig &cfg)
{
  _ctrl = cfg;
  // Safety defaults
  if (_ctrl.periodMs < 200) _ctrl.periodMs = 200;
  // When enabling control, force an immediate tick
  _lastCtrlMs = 0;
  // Reset watchdog state
  _consecutiveFailures = 0;
  _watchdogTripped = false;
  _lastSuccessMs = millis();
}

void OTBusESP32Pro::enableControl(bool enable)
{
  _ctrl.enabled = enable;
  _lastCtrlMs = 0;
  // Reset watchdog state
  _consecutiveFailures = 0;
  _watchdogTripped = false;
  _lastSuccessMs = millis();
}

void OTBusESP32Pro::setCHEnable(bool enable)
{
  _ctrl.chEnable = enable;
}

void OTBusESP32Pro::setDHWEnable(bool enable)
{
  _ctrl.dhwEnable = enable;
}

void OTBusESP32Pro::setTSet(float celsius)
{
  _ctrl.tSetC = celsius;
}


void OTBusESP32Pro::setWatchdogConfig(const WatchdogConfig &cfg)
{
  _wd = cfg;
  if (_wd.timeoutMs < 500) _wd.timeoutMs = 500;
}

bool OTBusESP32Pro::enableHardwareWatchdog(uint32_t timeoutSeconds)
{
#if defined(ARDUINO_ARCH_ESP32)
  if (timeoutSeconds < 2) timeoutSeconds = 2;
  // Init or re-init WDT with panic reset enabled.
  esp_err_t e = ESP_FAIL;
#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
  // ESP-IDF v5+: esp_task_wdt_init takes a config struct.
  esp_task_wdt_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.timeout_ms = timeoutSeconds * 1000UL;
  cfg.trigger_panic = true;
  cfg.idle_core_mask = 0;
  e = esp_task_wdt_init(&cfg);
#else
  // ESP-IDF v4 and older: (timeout_seconds, trigger_panic)
  e = esp_task_wdt_init(timeoutSeconds, true);
#endif
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
    _hwWdtEnabled = false;
    return false;
  }
  // Add current task (loopTask)
  e = esp_task_wdt_add(NULL);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
    _hwWdtEnabled = false;
    return false;
  }
  _hwWdtEnabled = true;
  return true;
#else
  (void)timeoutSeconds;
  _hwWdtEnabled = false;
  return false;
#endif
}

void OTBusESP32Pro::disableHardwareWatchdog()
{
#if defined(ARDUINO_ARCH_ESP32)
  (void)esp_task_wdt_delete(NULL);
#endif
  _hwWdtEnabled = false;
}


uint32_t OTBusESP32Pro::lastResponseRaw() const
{
  if (!ot) return 0;
  return ot->getLastResponse();
}


OpenThermResponseStatus OTBusESP32Pro::lastStatus() const
{
  if (!ot) return OpenThermResponseStatus::NONE;
  return ot->getLastResponseStatus();
}

bool OTBusESP32Pro::request(MessageType type, DataID id, uint16_t value, Frame &outFrame, OpenThermResponseStatus &outStatus)
{
  if (!ot || !roleMaster) { outStatus = OpenThermResponseStatus::NONE; outFrame.raw = 0; return false; }

  unsigned long req = OpenTherm::buildRequest((OpenThermMessageType)((uint8_t)type),
                                              (OpenThermMessageID)((uint8_t)id),
                                              value);
  unsigned long resp = ot->sendRequest(req);
  outFrame.raw = (uint32_t)resp;
  outStatus = ot->getLastResponseStatus();
  const bool ok = (outStatus == OpenThermResponseStatus::SUCCESS);
  if (ok) {
    _lastSuccessMs = millis();
    _consecutiveFailures = 0;
    _watchdogTripped = false;
  } else {
    // Count failures for watchdog (TIMEOUT / INVALID). Ignore NONE.
    if (outStatus != OpenThermResponseStatus::NONE) {
      if (_consecutiveFailures < 255) _consecutiveFailures++;
    }
  }
  return ok;
}

static const __FlashStringHelper* _respStatusStr(OpenThermResponseStatus st)
{
  switch (st) {
    case OpenThermResponseStatus::SUCCESS: return F("SUCCESS");
    case OpenThermResponseStatus::INVALID: return F("INVALID");
    case OpenThermResponseStatus::TIMEOUT: return F("TIMEOUT");
    case OpenThermResponseStatus::NONE: default: return F("NONE");
  }
}

static const __FlashStringHelper* _msgTypeStr(MessageType mt)
{
  switch (mt) {
    case MessageType::READ_DATA: return F("READ_DATA");
    case MessageType::WRITE_DATA: return F("WRITE_DATA");
    case MessageType::INVALID_DATA: return F("INVALID_DATA");
    case MessageType::RESERVED_M2S: return F("RESERVED");
    case MessageType::READ_ACK: return F("READ_ACK");
    case MessageType::WRITE_ACK: return F("WRITE_ACK");
    case MessageType::DATA_INVALID: return F("DATA_INVALID");
    case MessageType::UNKNOWN_DATA_ID: return F("UNKNOWN_DATA_ID");
    default: return F("?");
  }
}

void OTBusESP32Pro::scanDataIDs(Stream &out, uint8_t startId, uint8_t endId, uint16_t delayMs, bool printAll)
{
  if (!ot || !roleMaster) {
    out.println(F("[OT] scanDataIDs: not in Master mode"));
    return;
  }
  if (startId > endId) {
    uint8_t tmp = startId; startId = endId; endId = tmp;
  }

  out.println(F("OpenTherm Data-ID scan (READ-DATA)"));
  out.print(F("Range: ")); out.print(startId); out.print(F("..")); out.println(endId);
  out.println(F("Format: ID | status | msgType | value(hex) | u16 | s16 | f8.8"));
  out.println(F("---------------------------------------------------------------"));

  // reset implemented list
  for (uint16_t i=0;i<128;i++) _implemented[i]=false;
  _implementedCount = 0;

  for (uint16_t id = startId; id <= endId; id++) {
    Frame f;
    OpenThermResponseStatus st;
    request(MessageType::READ_DATA, (DataID)((uint8_t)id), 0x0000, f, st);

    MessageType rmt = (MessageType)OpenTherm::getMessageType(f.raw);
    uint16_t v = (uint16_t)(f.raw & 0xFFFF);
    int16_t vs = (int16_t)v;
    float vf = F88::decode(v);

    bool supported = (st == OpenThermResponseStatus::SUCCESS) &&
                     (rmt == MessageType::READ_ACK || rmt == MessageType::WRITE_ACK || rmt == MessageType::DATA_INVALID);

    if (supported && id < 128) {
      if (!_implemented[id]) { _implemented[id] = true; _implementedCount++; }
    }

    if (printAll || supported) {
      out.print(id);
      out.print(F(" | "));
      out.print(_respStatusStr(st));
      out.print(F(" | "));
      if (st == OpenThermResponseStatus::SUCCESS) out.print(_msgTypeStr(rmt));
      else out.print(F("-"));
      out.print(F(" | 0x"));
      if (v < 0x1000) out.print('0');
      if (v < 0x100) out.print('0');
      if (v < 0x10) out.print('0');
      out.print(v, HEX);
      out.print(F(" | "));
      out.print((uint32_t)v);
      out.print(F(" | "));
      out.print((int32_t)vs);
      out.print(F(" | "));
      out.println(vf, 2);
    }

    delay(delayMs);

    if (id == 255) break; // safety for uint8_t wrap, though endId <= 127 by default
  }

  out.println(F("---------------------------------------------------------------"));
  out.println(F("Legend: READ_ACK=implemented, DATA_INVALID=implemented but no valid data now, UNKNOWN_DATA_ID=not supported"));
}

bool OTBusESP32Pro::read(DataID id, Frame &outFrame, uint16_t requestValue)
{
  OpenThermResponseStatus st;
  return request(MessageType::READ_DATA, id, requestValue, outFrame, st);
}

bool OTBusESP32Pro::write(DataID id, uint16_t value, Frame &outFrame)
{
  OpenThermResponseStatus st;
  return request(MessageType::WRITE_DATA, id, value, outFrame, st);
}

bool OTBusESP32Pro::getStatus(StatusFlags &out)
{
  Frame f;
  if (!read(DataID::Status, f, StatusFlags::encodeMaster(true))) return false;
  out = StatusFlags::decode(f.value());
  return true;
}

bool OTBusESP32Pro::getBoilerTemperature(float &outCelsius)
{
  Frame f;
  if (!read(DataID::BoilerWaterTemperature_Tboiler, f)) return false;
  outCelsius = F88::decode(f.value());
  return true;
}

bool OTBusESP32Pro::getDHWTemperature(float &outCelsius)
{
  Frame f;
  if (!read(DataID::DHWTemperature_Tdhw, f)) return false;
  outCelsius = F88::decode(f.value());
  return true;
}

bool OTBusESP32Pro::getOutsideTemperature(float &outCelsius)
{
  Frame f;
  if (!read(DataID::OutsideTemperature_Toutside, f)) return false;
  outCelsius = F88::decode(f.value());
  return true;
}

bool OTBusESP32Pro::getRelativeModulationLevel(float &outPercent)
{
  Frame f;
  if (!read(DataID::RelativeModulationLevel, f)) return false;
  outPercent = F88::decode(f.value());
  return true;
}

bool OTBusESP32Pro::setMasterStatus(bool chEnable, bool dhwEnable, bool coolingEnable, bool otcActive, bool ch2Enable)
{
  if (!ot || !roleMaster) return false;
  // v2.2: special status exchange is done by READ-DATA id=0 carrying MasterStatus in HB (slave responds with READ-ACK incl. SlaveStatus in LB)
  Frame f;
  return read(DataID::Status, f, StatusFlags::encodeMaster(chEnable, dhwEnable, coolingEnable, otcActive, ch2Enable));
}

bool OTBusESP32Pro::setCHWaterSetpoint(float celsius)
{
  Frame f;
  return write(DataID::ControlSetpoint_TSet, F88::encode(celsius), f);
}

bool OTBusESP32Pro::setMaxRelModulationLevel(float percent)
{
  Frame f;
  return write(DataID::MaxRelModLevelSetting, F88::encode(percent), f);
}

void OTBusESP32Pro::handleInterrupt()
{
  if (!ot) return;
  ot->handleInterrupt();
}


bool OTBusESP32Pro::isImplemented(uint8_t id) const
{
  if (id >= 128) return false;
  return _implemented[id];
}

uint8_t OTBusESP32Pro::implementedCount() const
{
  return _implementedCount;
}

uint8_t OTBusESP32Pro::implementedAt(uint8_t index) const
{
  if (index >= _implementedCount) return 255;
  uint8_t n = 0;
  for (uint16_t id = 0; id < 128; id++) {
    if (_implemented[id]) {
      if (n == index) return (uint8_t)id;
      n++;
    }
  }
  return 255;
}


// Human-friendly Data-ID names (subset). Unknown IDs return "?".
// Kept small on purpose; extend as needed.
static const __FlashStringHelper* _dataIdName(DataID id)
{
  switch ((uint8_t)id) {
    case 0: return F("Status");
    case 1: return F("TSet");
    case 2: return F("MasterConfig");
    case 3: return F("SlaveConfig");
    case 4: return F("Command");
    case 5: return F("ASFflags/OEMfault");
    case 6: return F("RBPflags");
    case 10: return F("TSPcount");
    case 11: return F("TSP");
    case 14: return F("MaxRelModLevelSet");
    case 15: return F("MaxCapacity/MinMod");
    case 16: return F("RoomSetpoint");
    case 17: return F("RelModLevel");
    case 18: return F("CHpressure");
    case 19: return F("DHWflow");
    case 20: return F("DayTime");
    case 21: return F("Date");
    case 22: return F("Year");
    case 24: return F("RoomTemp");
    case 25: return F("Tboiler");
    case 26: return F("Tdhw");
    case 27: return F("Toutside");
    case 28: return F("Treturn");
    case 30: return F("Tcollector");
    case 31: return F("TflowCH2");
    case 33: return F("Texhaust");
    case 48: return F("DHWsetp bounds");
    case 49: return F("MaxCHsetp bounds");
    case 56: return F("DHW setpoint");
    case 57: return F("Max CH setpoint");
    case 115: return F("OEM diagnostic");
    case 116: return F("Burner starts");
    case 117: return F("CH pump starts");
    case 118: return F("DHW pump starts");
    case 119: return F("DHW burner starts");
    case 120: return F("Burner hours");
    case 121: return F("CH pump hours");
    case 122: return F("DHW pump hours");
    case 123: return F("DHW burner hours");
    case 124: return F("OT ver master");
    case 125: return F("OT ver slave");
    case 126: return F("Master version");
    case 127: return F("Slave version");
    default: return F("?");
  }
}

static void _printBool(Stream &out, const __FlashStringHelper *name, bool v)
{
  out.print(name);
  out.print(v ? F("1") : F("0"));
}

void OTBusESP32Pro::printDecoded(Stream &out, uint8_t id, const Frame &f) const
{
  const uint16_t v = (uint16_t)(f.raw & 0xFFFF);
  const int16_t vs = (int16_t)v;
  const float vf = F88::decode(v);

  out.print(F("ID"));
  out.print(id);
  out.print(F(" "));
  out.print(_dataIdName((DataID)id));
  out.print(F(" = "));

  switch (id) {
    case 0: { // Status flags
      uint8_t m = (uint8_t)(v >> 8);
      uint8_t s = (uint8_t)(v & 0xFF);
      out.print(F("M["));
      _printBool(out, F("CH="), m & (1 << 0));
      out.print(F(" "));
      _printBool(out, F("DHW="), m & (1 << 1));
      out.print(F(" "));
      _printBool(out, F("COOL="), m & (1 << 2));
      out.print(F(" "));
      _printBool(out, F("OTC="), m & (1 << 3));
      out.print(F(" "));
      _printBool(out, F("CH2="), m & (1 << 4));
      out.print(F("] S["));
      _printBool(out, F("CH="), s & (1 << 1));
      out.print(F(" "));
      _printBool(out, F("DHW="), s & (1 << 2));
      out.print(F(" "));
      _printBool(out, F("FL="), s & (1 << 3));
      out.print(F(" "));
      _printBool(out, F("COOL="), s & (1 << 4));
      out.print(F(" "));
      _printBool(out, F("CH2="), s & (1 << 5));
      out.print(F(" "));
      _printBool(out, F("DIAG="), s & (1 << 6));
      out.print(F("]"));
      break;
    }

    case 3: { // Slave configuration / member id
      uint8_t cfg = (uint8_t)(v >> 8);
      uint8_t member = (uint8_t)(v & 0xFF);
      out.print(F("cfg=0x"));
      if (cfg < 0x10) out.print('0');
      out.print(cfg, HEX);
      out.print(F(" member="));
      out.print(member);
      out.print(F(" [DHW_present="));
      out.print((cfg & (1 << 0)) ? 1 : 0);
      out.print(F(" DHW_type="));
      if (cfg & (1 << 3)) out.print(F("storage"));
      else out.print(F("instant"));
      out.print(F(" pumpCtrl="));
      out.print((cfg & (1 << 4)) ? F("allowed") : F("no"));
      out.print(F("]"));
      break;
    }

    case 5: { // Fault flags + OEM code
      uint8_t fl = (uint8_t)(v >> 8);
      uint8_t oem = (uint8_t)(v & 0xFF);
      out.print(F("flags=0x"));
      if (fl < 0x10) out.print('0');
      out.print(fl, HEX);
      out.print(F(" oem="));
      out.print(oem);
      break;
    }

    case 6: { // Remote boiler parameter flags
      uint8_t te = (uint8_t)(v >> 8);
      uint8_t rw = (uint8_t)(v & 0xFF);
      out.print(F("TE=0x"));
      if (te < 0x10) out.print('0');
      out.print(te, HEX);
      out.print(F(" RW=0x"));
      if (rw < 0x10) out.print('0');
      out.print(rw, HEX);
      out.print(F(" (P1..P8)"));
      break;
    }

    // These are typically f8.8 temps/percents; show with unit
    case 10: // TSet (control setpoint) often used as CH setpoint
    case 17: // CH water setpoint
    case 18: // relative modulation
    case 25: // boiler water temperature
    case 26: // DHW temperature
    case 56: // DHW setpoint
    case 57: // max CH setpoint
      out.print(vf, 2);
      if (id == 18) out.print(F("%"));
      else out.print(F("C"));
      break;

    case 27: // outside temperature often signed
      out.print(vf, 2);
      out.print(F("C"));
      break;

    case 48: // bounds: upper/lower in s8
    case 49: {
      int8_t upper = (int8_t)(v >> 8);
      int8_t lower = (int8_t)(v & 0xFF);
      out.print((int)lower);
      out.print(F(".."));
      out.print((int)upper);
      out.print(F(" (s8)"));
      break;
    }

    default:
      // generic fallback
      out.print(F("0x"));
      if (v < 0x1000) out.print('0');
      if (v < 0x100) out.print('0');
      if (v < 0x10) out.print('0');
      out.print(v, HEX);
      out.print(F(" u16="));
      out.print((uint32_t)v);
      out.print(F(" s16="));
      out.print((int32_t)vs);
      out.print(F(" f8.8="));
      out.print(vf, 2);
      break;
  }
}

void OTBusESP32Pro::printStatusReport(Stream &out, uint16_t perIdDelayMs)
{
  if (!ot || !roleMaster) {
    out.println(F("[OT] printStatusReport: not in Master mode"));
    return;
  }
  if (_implementedCount == 0) {
    out.println(F("[OT] No implemented IDs (run scanDataIDs first)."));
    return;
  }

  out.println(F("OT status (implemented Data-IDs):"));
  for (uint8_t i = 0; i < _implementedCount; i++) {
    uint8_t id = implementedAt(i);
    if (id == 255) break;

    Frame f;
    OpenThermResponseStatus st;
    request(MessageType::READ_DATA, (DataID)id, 0x0000, f, st);

    if (st == OpenThermResponseStatus::SUCCESS) {
      MessageType rmt = (MessageType)OpenTherm::getMessageType(f.raw);
      if (rmt == MessageType::READ_ACK || rmt == MessageType::DATA_INVALID || rmt == MessageType::WRITE_ACK) {
        out.print(F(" - "));
        printDecoded(out, id, f);
        out.print(F(" ("));
        out.print(_msgTypeStr(rmt));
        out.println(F(")"));
      } else {
        out.print(F(" - ID"));
        out.print(id);
        out.print(F(" "));
        out.print(_dataIdName((DataID)id));
        out.print(F(": unexpected msgType "));
        out.println(_msgTypeStr(rmt));
      }
    } else {
      out.print(F(" - ID"));
      out.print(id);
      out.print(F(" "));
      out.print(_dataIdName((DataID)id));
      out.print(F(": "));
      out.println(_respStatusStr(st));
    }

    if (perIdDelayMs) delay(perIdDelayMs);
  }
  out.println();
}
