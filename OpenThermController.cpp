#include "OpenThermController.h"
#include "Log.h"
#include "FsController.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string.h>

#include "OpenThermPlusESP32.h"
#include "OpenThermProtocolItems.h"

namespace {
  OpenThermConfig g_cfg;
  OpenThermStatus g_st;
  bool g_inited = false;

  OpenThermPlusESP32 g_ot;

  struct RawSlot {
    OpenThermRawValue v;
  };
  RawSlot g_raw[48];
  uint8_t g_rawN = 0;

  // command queue
  bool g_cmdEnableDirty = false;
  bool g_cmdChDirty = false;
  bool g_cmdDhwDirty = false;
  bool g_cmdMaxModDirty = false;
  bool g_cmdRoomSpDirty = false;
  bool g_cmdMaxChWaterSpDirty = false;
  bool g_cmdResetFaultDirty = false;

  bool g_reqChEn = false;
  bool g_reqDhwEn = false;
  float g_reqChC = NAN;
  float g_reqDhwC = NAN;
  float g_reqMaxModPct = NAN;
  float g_reqRoomSpC = NAN;
  float g_reqMaxChWaterSpC = NAN;

  uint32_t g_lastPollMs = 0;
  uint32_t g_lastInitTryMs = 0;

  uint32_t g_lastLogMs = 0;

  static inline float clampf(float v, float lo, float hi) {
    if (!isfinite(v)) return v;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
  }

  static void bumpResult(OpenThermPlusESP32::Result r) {
    g_st.totalCount++;
    switch (r) {
      case OpenThermPlusESP32::Result::OK: g_st.okCount++; break;
      case OpenThermPlusESP32::Result::TIMEOUT: g_st.timeoutCount++; break;
      case OpenThermPlusESP32::Result::FRAME_ERROR: g_st.frameErrorCount++; break;
      case OpenThermPlusESP32::Result::PARITY_ERROR: g_st.parityErrorCount++; break;
      case OpenThermPlusESP32::Result::BAD_RESPONSE: g_st.badResponseCount++; break;
      case OpenThermPlusESP32::Result::NOT_INITIALIZED: g_st.notInitializedCount++; break;
      default: break;
    }
    // keep a live view of ISR overflow counter from the library
    g_st.isrOverflowCount = g_ot.edgeOverflowCount();
  }

  static void rawUpsert(uint8_t id, uint8_t msgType, uint16_t u16, bool valid) {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < g_rawN; i++) {
      if (g_raw[i].v.id == id) {
        g_raw[i].v.msgType = msgType;
        g_raw[i].v.u16 = u16;
        g_raw[i].v.f88 = valid ? OpenThermPlusESP32::decodeF8_8(u16) : NAN;
        g_raw[i].v.valid = valid;
        g_raw[i].v.tsMs = now;
        return;
      }
    }
    if (g_rawN < (uint8_t)(sizeof(g_raw)/sizeof(g_raw[0]))) {
      g_raw[g_rawN].v.id = id;
      g_raw[g_rawN].v.msgType = msgType;
      g_raw[g_rawN].v.u16 = u16;
      g_raw[g_rawN].v.f88 = valid ? OpenThermPlusESP32::decodeF8_8(u16) : NAN;
      g_raw[g_rawN].v.valid = valid;
      g_raw[g_rawN].v.tsMs = now;
      g_rawN++;
    }
  }

  static void applyStatusId0(const OpenThermPlusESP32::Frame& f) {
    const uint8_t master = OpenThermPlusESP32::hiByte(f.value);
    const uint8_t slave  = OpenThermPlusESP32::loByte(f.value);

    g_st.chEnable      = (master & (1 << 0)) != 0;
    g_st.dhwEnable     = (master & (1 << 1)) != 0;
    g_st.coolingEnable = (master & (1 << 2)) != 0;
    g_st.otcActive     = (master & (1 << 3)) != 0;
    g_st.ch2Enable     = (master & (1 << 4)) != 0;

    g_st.fault         = (slave & (1 << 0)) != 0;
    g_st.chActive      = (slave & (1 << 1)) != 0;
    g_st.dhwActive     = (slave & (1 << 2)) != 0;
    g_st.flameOn       = (slave & (1 << 3)) != 0;
    g_st.coolingActive = (slave & (1 << 4)) != 0;
    g_st.ch2Active     = (slave & (1 << 5)) != 0;
    g_st.diagnostic    = (slave & (1 << 6)) != 0;
  }

  static void recomputeDerived() {
    // Prefer direct power if we already have it.
    if (!isfinite(g_st.powerKw)) {
      if (isfinite(g_st.modulationPct)) {
        const float maxKw = isfinite(g_st.maxCapacityKw) ? g_st.maxCapacityKw : g_cfg.assumedMaxBoilerKw;
        g_st.powerKw = (g_st.modulationPct / 100.0f) * maxKw;
      }
    }
  }

  static void ensureLogHeader(File& f) {
    if (f.size() > 0) return;
    f.print("ts_ms,ready,fault,ch_en,dhw_en,ch_sp_c,dhw_sp_c,tboiler_c,treturn_c,tdhw_c,troom_c,tout_c,mod_pct,press_bar,flow_lpm,p_kw\n");
  }

  static void appendLog() {
    if (!g_cfg.logEnabled) return;
    const uint32_t now = millis();
    if (g_lastLogMs != 0 && (uint32_t)(now - g_lastLogMs) < g_cfg.logIntervalMs) return;

    if (!LittleFS.begin(true)) return;
    const char* path = "/opentherm.csv";
    File f = LittleFS.open(path, FILE_APPEND);
    if (!f) return;
    ensureLogHeader(f);

    f.printf("%lu,%u,%u,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f,%.3f\n",
             (unsigned long)now,
             (unsigned)g_st.ready,
             (unsigned)g_st.fault,
             (unsigned)g_st.chEnable,
             (unsigned)g_st.dhwEnable,
             (double)g_st.reqChSetpointC,
             (double)g_st.reqDhwSetpointC,
             (double)g_st.boilerTempC,
             (double)g_st.returnTempC,
             (double)g_st.dhwTempC,
             (double)g_st.roomTempC,
             (double)g_st.outdoorTempC,
             (double)g_st.modulationPct,
             (double)g_st.pressureBar,
             (double)g_st.flowRateLpm,
             (double)g_st.powerKw);
    f.close();
    g_lastLogMs = now;
  }

  static bool doRead(uint8_t id, OpenThermPlusESP32::Frame& fr) {
    OpenThermPlusESP32::Result r = g_ot.readData(id, fr);
    bumpResult(r);
    if (r != OpenThermPlusESP32::Result::OK) return false;
    return (fr.type == OpenThermPlusESP32::MsgType::READ_ACK || fr.type == OpenThermPlusESP32::MsgType::WRITE_ACK);
  }

  static bool doWrite(uint8_t id, uint16_t v, OpenThermPlusESP32::Frame& fr) {
    OpenThermPlusESP32::Result r = g_ot.writeData(id, v, fr);
    bumpResult(r);
    if (r != OpenThermPlusESP32::Result::OK) return false;
    return (fr.type == OpenThermPlusESP32::MsgType::WRITE_ACK);
  }

  static void pollOnce() {
    g_st.reason = "";
    g_st.lastUpdateMs = millis();

    // Status exchange (ID0): also carries master flags -> slave flags
    OpenThermPlusESP32::Frame stf{};
    OpenThermPlusESP32::Result sr = g_ot.readStatus(stf);
    bumpResult(sr);
    if (sr != OpenThermPlusESP32::Result::OK) {
      g_st.ready = false;
      g_st.reason = "status timeout";
      return;
    }
    g_st.ready = true;
    rawUpsert(0, (uint8_t)stf.type, stf.value, true);
    applyStatusId0(stf);

    // Poll additional IDs
    for (uint8_t i = 0; i < g_cfg.pollIdCount; i++) {
      const uint8_t id = g_cfg.pollIds[i];
      if (id == 0) continue;

      OpenThermPlusESP32::Frame fr{};
      bool ok = doRead(id, fr);
      rawUpsert(id, (uint8_t)fr.type, fr.value, ok);

      if (!ok) continue;

      const float f88 = OpenThermPlusESP32::decodeF8_8(fr.value);

      switch (id) {
        case 5: { // fault flags + oem fault code
          g_st.faultFlags = OpenThermPlusESP32::hiByte(fr.value);
          g_st.oemFaultCode = OpenThermPlusESP32::loByte(fr.value);
        } break;
        case 15: { // boiler capacity (HB kW) + min modulation (LB %)
          g_st.maxCapacityKw = (float)OpenThermPlusESP32::hiByte(fr.value);
          g_st.minModulationPct = (float)OpenThermPlusESP32::loByte(fr.value);
        } break;
        case 17: g_st.modulationPct = f88; break;
        case 18: g_st.pressureBar = f88; break;
        case 19: g_st.flowRateLpm = f88; break;
        case 24: g_st.roomTempC = f88; break;
        case 16: g_st.roomSetpointC = f88; break;
        case 25: g_st.boilerTempC = f88; break;
        case 26: g_st.dhwTempC = f88; break;
        case 27: g_st.outdoorTempC = f88; break;
        case 28: g_st.returnTempC = f88; break;
        case 56: g_st.dhwSetpointC = f88; break;
        case 57: g_st.maxChWaterSetpointC = f88; break;
        default: break;
      }
    }

    // Derive power
    g_st.powerKw = NAN;
    recomputeDerived();

    appendLog();
  }

  static bool tryInit() {
    if (!g_cfg.enabled) return false;
    if (g_cfg.txPin < 0 || g_cfg.rxPin < 0) return false;

    OpenThermPlusESP32::Config cfg{};
    cfg.txPin = (uint8_t)g_cfg.txPin;
    cfg.rxPin = (uint8_t)g_cfg.rxPin;
    cfg.invertTx = g_cfg.invertTx;
    cfg.invertRx = g_cfg.invertRx;
    cfg.autoDetectLogic = g_cfg.autoDetectLogic;
    cfg.retries = g_cfg.retries;
    cfg.slaveResponseTimeoutMs = g_cfg.slaveTimeoutMs;
    cfg.interMessageGapMs = g_cfg.interMessageGapMs;
    cfg.enablePolling = false; // we drive polling from controller

    const bool ok = g_ot.begin(cfg);
    g_st.ready = ok;
    g_st.reason = ok ? "" : "init failed";
    return ok;
  }

  static void applyMasterFlagsToLib() {
    // push desired flags into library master status
    g_ot.setCHEnable(g_reqChEn);
    g_ot.setDHWEnable(g_reqDhwEn);
    // keep others false for now
    g_ot.setCoolingEnable(false);
    g_ot.setOTCActive(false);
    g_ot.setCH2Enable(false);
  }

  static void processCmdQueue() {
    if (!g_st.ready) return;

    OpenThermPlusESP32::Frame fr{};

    // Fault reset (one-shot): send ID0 with master bit7 set, then clear.
    if (g_cmdResetFaultDirty) {
      applyMasterFlagsToLib();
      g_ot.setFaultReset(true);
      OpenThermPlusESP32::Frame stf{};
      OpenThermPlusESP32::Result sr = g_ot.readStatus(stf);
      bumpResult(sr);
      g_ot.setFaultReset(false);
      if (sr == OpenThermPlusESP32::Result::OK) {
        rawUpsert(0, (uint8_t)stf.type, stf.value, true);
        applyStatusId0(stf);
        g_cmdResetFaultDirty = false;
        g_st.lastCmdMs = millis();
        g_st.lastCmd = "ID0 fault reset";
      } else {
        g_st.reason = "fault reset timeout";
      }
    }

    // status flags (sent by readStatus as master HB)
    if (g_cmdEnableDirty) {
      applyMasterFlagsToLib();
      // force a status exchange now to apply flags quickly
      OpenThermPlusESP32::Frame stf{};
      OpenThermPlusESP32::Result sr = g_ot.readStatus(stf);
      bumpResult(sr);
      if (sr == OpenThermPlusESP32::Result::OK) {
        rawUpsert(0, (uint8_t)stf.type, stf.value, true);
        applyStatusId0(stf);
        g_cmdEnableDirty = false;
        g_st.lastCmdMs = millis();
        g_st.lastCmd = "ID0 flags";
      } else {
        g_st.reason = "flags timeout";
      }
    }

    if (g_cmdChDirty && isfinite(g_reqChC)) {
      const float t = clampf(g_reqChC, g_cfg.minChSetpointC, g_cfg.maxChSetpointC);
      OpenThermPlusESP32::Result r = g_ot.writeTset(t, fr);
      bumpResult(r);
      if (r == OpenThermPlusESP32::Result::OK) {
        rawUpsert(1, (uint8_t)fr.type, fr.value, true);
        g_cmdChDirty = false;
        g_st.lastCmdMs = millis();
        g_st.lastCmd = "ID1 Tset";
      } else {
        g_st.reason = "ID1 fail";
      }
    }

    if (g_cmdDhwDirty && isfinite(g_reqDhwC)) {
      const float t = clampf(g_reqDhwC, 10.0f, 80.0f);
      if (doWrite(56, OpenThermPlusESP32::encodeF8_8(t), fr)) {
        rawUpsert(56, (uint8_t)fr.type, fr.value, true);
        g_cmdDhwDirty = false;
        g_st.lastCmdMs = millis();
        g_st.lastCmd = "ID56 DHWsp";
      } else {
        g_st.reason = "ID56 fail";
      }
    }

    if (g_cmdMaxModDirty && isfinite(g_reqMaxModPct)) {
      const float p = clampf(g_reqMaxModPct, 0.0f, 100.0f);
      if (doWrite(14, OpenThermPlusESP32::encodeF8_8(p), fr)) {
        rawUpsert(14, (uint8_t)fr.type, fr.value, true);
        g_cmdMaxModDirty = false;
        g_st.lastCmdMs = millis();
        g_st.lastCmd = "ID14 MaxMod";
      } else {
        g_st.reason = "ID14 fail";
      }
    }

    if (g_cmdRoomSpDirty && isfinite(g_reqRoomSpC)) {
      const float t = clampf(g_reqRoomSpC, 5.0f, 30.0f);
      if (doWrite(16, OpenThermPlusESP32::encodeF8_8(t), fr)) {
        rawUpsert(16, (uint8_t)fr.type, fr.value, true);
        g_cmdRoomSpDirty = false;
        g_st.lastCmdMs = millis();
        g_st.lastCmd = "ID16 RoomSp";
      } else {
        g_st.reason = "ID16 fail";
      }
    }

    if (g_cmdMaxChWaterSpDirty && isfinite(g_reqMaxChWaterSpC)) {
      const float t = clampf(g_reqMaxChWaterSpC, 20.0f, 90.0f);
      if (doWrite(57, OpenThermPlusESP32::encodeF8_8(t), fr)) {
        rawUpsert(57, (uint8_t)fr.type, fr.value, true);
        g_cmdMaxChWaterSpDirty = false;
        g_st.lastCmdMs = millis();
        g_st.lastCmd = "ID57 MaxCHsp";
      } else {
        g_st.reason = "ID57 fail";
      }
    }

    // Update public requested fields
    g_st.reqChSetpointC = g_reqChC;
    g_st.reqDhwSetpointC = g_reqDhwC;
    g_st.reqMaxModulationPct = g_reqMaxModPct;

    g_st.reqRoomSetpointC = g_reqRoomSpC;
    g_st.reqMaxChWaterSetpointC = g_reqMaxChWaterSpC;
  }
}

void openthermInit() {
  if (g_inited) return;
  g_inited = true;
  g_st = OpenThermStatus();
  g_cfg = OpenThermConfig();
}

void openthermApplyConfig(const String& json) {
  DynamicJsonDocument doc(4096);
  DeserializationError e = deserializeJson(doc, json);
  if (e) return;

  JsonObject root = doc.as<JsonObject>();
  JsonObject ot = root["opentherm"];
  if (ot.isNull()) return;

  g_cfg.enabled = (bool)(ot["enabled"] | g_cfg.enabled);

  g_cfg.txPin = (int8_t)(ot["txPin"] | ot["inPin"] | g_cfg.txPin);
  g_cfg.rxPin = (int8_t)(ot["rxPin"] | ot["outPin"] | g_cfg.rxPin);

  g_cfg.invertTx = (bool)(ot["invertTx"] | g_cfg.invertTx);
  g_cfg.invertRx = (bool)(ot["invertRx"] | g_cfg.invertRx);
  g_cfg.autoDetectLogic = (bool)(ot["autoDetectLogic"] | g_cfg.autoDetectLogic);

  g_cfg.pollMs = (uint16_t)(ot["pollMs"] | g_cfg.pollMs);
  g_cfg.interMessageGapMs = (uint16_t)(ot["interMessageGapMs"] | g_cfg.interMessageGapMs);
  g_cfg.slaveTimeoutMs = (uint16_t)(ot["slaveTimeoutMs"] | g_cfg.slaveTimeoutMs);
  g_cfg.retries = (uint8_t)(ot["retries"] | g_cfg.retries);

  const char* bc = ot["boilerControl"] | nullptr;
  if (bc) {
    String s = String(bc); s.toLowerCase();
    if (s == "opentherm") g_cfg.boilerControl = OpenThermBoilerControl::OPENTHERM;
    else if (s == "hybrid") g_cfg.boilerControl = OpenThermBoilerControl::HYBRID;
    else g_cfg.boilerControl = OpenThermBoilerControl::RELAY;
  }

  g_cfg.mapEquithermChSetpoint = (bool)(ot["mapEquithermChSetpoint"] | g_cfg.mapEquithermChSetpoint);
  g_cfg.mapDhw = (bool)(ot["mapDhw"] | g_cfg.mapDhw);
  g_cfg.mapNightMode = (bool)(ot["mapNightMode"] | g_cfg.mapNightMode);

  g_cfg.minChSetpointC = (float)(ot["minChSetpointC"] | g_cfg.minChSetpointC);
  g_cfg.maxChSetpointC = (float)(ot["maxChSetpointC"] | g_cfg.maxChSetpointC);

  g_cfg.dhwSetpointC = (float)(ot["dhwSetpointC"] | g_cfg.dhwSetpointC);
  g_cfg.dhwBoostChSetpointC = (float)(ot["dhwBoostChSetpointC"] | g_cfg.dhwBoostChSetpointC);

  // Optional setpoints
  if (!ot["roomSetpointC"].isNull()) g_cfg.roomSetpointC = (float)(ot["roomSetpointC"] | NAN);
  if (!ot["maxChWaterSetpointC"].isNull()) g_cfg.maxChWaterSetpointC = (float)(ot["maxChWaterSetpointC"] | NAN);

  g_cfg.assumedMaxBoilerKw = (float)(ot["assumedMaxBoilerKw"] | g_cfg.assumedMaxBoilerKw);

  // polling list
  if (ot["pollIds"].is<JsonArray>()) {
    JsonArray a = ot["pollIds"].as<JsonArray>();
    uint8_t n = 0;
    for (JsonVariant v : a) {
      if (!v.is<int>()) continue;
      int id = v.as<int>();
      if (id < 0 || id > 127) continue;
      if (n < (uint8_t)sizeof(g_cfg.pollIds)) g_cfg.pollIds[n++] = (uint8_t)id;
    }
    if (n > 0) g_cfg.pollIdCount = n;
  } else if (!ot["pollIds"].isNull()) {
    // allow string "0,25,17"
    String s = String((const char*)(ot["pollIds"] | ""));
    uint8_t n = 0;
    int start = 0;
    while (start < (int)s.length()) {
      int comma = s.indexOf(',', start);
      if (comma < 0) comma = s.length();
      String tok = s.substring(start, comma);
      tok.trim();
      int id = tok.toInt();
      if (id >= 0 && id <= 127) {
        if (n < (uint8_t)sizeof(g_cfg.pollIds)) g_cfg.pollIds[n++] = (uint8_t)id;
      }
      start = comma + 1;
    }
    if (n > 0) g_cfg.pollIdCount = n;
  }

  g_cfg.logEnabled = (bool)(ot["logEnabled"] | g_cfg.logEnabled);
  g_cfg.logIntervalMs = (uint32_t)(ot["logIntervalMs"] | g_cfg.logIntervalMs);

  // If config changed, re-init on next loop
  g_st.ready = false;
  g_st.reason = "cfg changed";
}

OpenThermConfig openthermGetConfig() { return g_cfg; }
OpenThermStatus openthermGetStatus() { return g_st; }

uint8_t openthermGetRaw(OpenThermRawValue* out, uint8_t maxOut) {
  if (!out || maxOut == 0) return 0;
  const uint8_t n = (g_rawN < maxOut) ? g_rawN : maxOut;
  for (uint8_t i = 0; i < n; i++) out[i] = g_raw[i].v;
  return n;
}

String openthermGetProtocolJson() {
  StaticJsonDocument<8192> doc;
  JsonArray a = doc.createNestedArray("items");

  for (uint8_t i = 0; i < OpenThermProtocol::DATA_ID_COUNT; i++) {
    OpenThermProtocol::DataIdInfo tmp;
    memcpy_P(&tmp, &OpenThermProtocol::DATA_IDS[i], sizeof(tmp));
    JsonObject o = a.createNestedObject();
    o["id"] = tmp.id;
    o["name"] = tmp.name;
    o["type"] = tmp.type;
    o["unit"] = tmp.unit;
    o["rw"] = tmp.rw;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void openthermCmdSetEnable(bool chEnable, bool dhwEnable) {
  g_reqChEn = chEnable;
  g_reqDhwEn = dhwEnable;
  g_cmdEnableDirty = true;
}

void openthermCmdSetChSetpoint(float tC) {
  g_reqChC = tC;
  g_cmdChDirty = true;
}

void openthermCmdSetDhwSetpoint(float tC) {
  g_reqDhwC = tC;
  g_cmdDhwDirty = true;
}

void openthermCmdSetMaxModulation(float pct) {
  g_reqMaxModPct = pct;
  g_cmdMaxModDirty = true;
}

void openthermCmdSetRoomSetpoint(float tC) {
  g_reqRoomSpC = tC;
  g_cmdRoomSpDirty = true;
}

void openthermCmdSetMaxChWaterSetpoint(float tC) {
  g_reqMaxChWaterSpC = tC;
  g_cmdMaxChWaterSpDirty = true;
}

void openthermCmdResetFault() {
  // OpenTherm: master status bit7 triggers a fault reset in the slave.
  // We will send one status exchange with the bit set and then clear it.
  g_cmdResetFaultDirty = true;
}

bool openthermRead(uint8_t dataId, uint16_t* outU16, uint8_t* outMsgType) {
  if (!g_st.ready) return false;
  OpenThermPlusESP32::Frame fr{};
  bool ok = doRead(dataId, fr);
  if (outMsgType) *outMsgType = (uint8_t)fr.type;
  if (outU16) *outU16 = fr.value;
  rawUpsert(dataId, (uint8_t)fr.type, fr.value, ok);
  return ok;
}

bool openthermWrite(uint8_t dataId, uint16_t value, uint8_t* outMsgType) {
  if (!g_st.ready) return false;
  OpenThermPlusESP32::Frame fr{};
  bool ok = doWrite(dataId, value, fr);
  if (outMsgType) *outMsgType = (uint8_t)fr.type;
  rawUpsert(dataId, (uint8_t)fr.type, fr.value, ok);
  return ok;
}

void openthermLoop() {
  if (!g_inited) openthermInit();

  const uint32_t now = millis();

  if (!g_cfg.enabled) {
    g_st.ready = false;
    g_st.reason = "disabled";
    return;
  }

  // (Re)initialize if not ready, retry slowly.
  if (!g_st.ready) {
    if (g_lastInitTryMs == 0 || (uint32_t)(now - g_lastInitTryMs) > 2000) {
      g_lastInitTryMs = now;
      tryInit();
      applyMasterFlagsToLib();
    }
    return;
  }

  // Process queued commands first (so flags/setpoints react faster than poll).
  processCmdQueue();

  // Apply optional steady-state configuration setpoints (only when configured).
  // These are "soft" writes: sent opportunistically and only when the requested value differs.
  if (isfinite(g_cfg.roomSetpointC) && (!isfinite(g_reqRoomSpC) || fabsf(g_reqRoomSpC - g_cfg.roomSetpointC) > 0.05f)) {
    openthermCmdSetRoomSetpoint(g_cfg.roomSetpointC);
  }
  if (isfinite(g_cfg.maxChWaterSetpointC) && (!isfinite(g_reqMaxChWaterSpC) || fabsf(g_reqMaxChWaterSpC - g_cfg.maxChWaterSetpointC) > 0.05f)) {
    openthermCmdSetMaxChWaterSetpoint(g_cfg.maxChWaterSetpointC);
  }

  if (g_lastPollMs == 0 || (uint32_t)(now - g_lastPollMs) >= g_cfg.pollMs) {
    g_lastPollMs = now;
    pollOnce();
  }
}

bool openthermClearLog() {
  if (!LittleFS.begin(true)) return false;
  return LittleFS.remove("/opentherm.csv");
}

String openthermGetLogPath() { return "/opentherm.csv"; }
