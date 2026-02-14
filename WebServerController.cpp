// IMPORTANT: include Features.h first so FEATURE_WEBSERVER is visible before
// including the header (which provides stubs when the feature is disabled).
#include "Features.h"
#include "WebServerController.h"

#if defined(FEATURE_WEBSERVER)

#include "FsController.h"
#include <Update.h>
#include "Log.h"

#include "LogicController.h"
#include "OpenThermController.h"
#include "DallasController.h"
#include "NetworkController.h"
#include "RelayController.h"
#include "InputController.h"
#include "BleController.h"
#include "ThermometerController.h"

#include <LittleFS.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// Fast UI goals (without additional libs):
//  - Serve static files with caching (+ optional .gz)
//  - Provide SSE stream for low-latency updates: GET /api/events
//  - Keep legacy polling endpoint: GET /api/status

namespace {
  WebServer g_server(80);
  bool g_inited = false;

  // ---- SSE (Server-Sent Events) ----
  struct SseClient {
    WiFiClient client;
    uint32_t lastSendMs = 0;
    uint32_t lastStateHash = 0;
    bool alive = false;
  };

  // Small fixed pool (UI usually only 1 browser tab)
  constexpr uint8_t kMaxSseClients = 4;
  SseClient g_sse[kMaxSseClients];

  // Set by webserverNotifyStateChanged().
  volatile bool g_stateDirty = true;

  static uint32_t fnv1a32(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
      h ^= data[i];
      h *= 16777619u;
    }
    return h;
  }

  static uint32_t fnv1a32_str(const String& s) {
    return fnv1a32(reinterpret_cast<const uint8_t*>(s.c_str()), s.length());
  }

  static void sendJson(int code, const JsonDocument& doc, const char* etag = nullptr) {
    String out;
    serializeJson(doc, out);
    if (etag) g_server.sendHeader("ETag", etag);
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.send(code, "application/json", out);
  }

  static String argOrEmpty(const char* key) {
    if (!g_server.hasArg(key)) return "";
    return g_server.arg(key);
  }

  // UI layout storage (LittleFS)
  static bool isSafeProfileChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  }

  static String uiProfileSanitize(const String& in) {
    String p = in;
    p.trim();
    if (!p.length()) return "default";
    String out;
    out.reserve(p.length());
    for (size_t i = 0; i < p.length(); i++) {
      char c = p[i];
      if (isSafeProfileChar(c)) out += c;
    }
    if (!out.length()) out = "default";
    if (out.length() > 24) out = out.substring(0, 24);
    return out;
  }

  static String uiLayoutPathForProfile(const String& profile) {
    const String p = uiProfileSanitize(profile);
    return String("/ui_layout_") + p + String(".json");
  }

  // Build a compact snapshot intended for frequent refresh.
  // Keep it small: bitmasks + a few floats.
  static void buildFastState(JsonDocument& doc) {
    doc["ts"] = (uint32_t)millis();
    doc["mode"] = logicModeToString(logicGetMode());
    doc["ctrl"] = (logicGetControlMode() == ControlMode::AUTO) ? "A" : "M";

    // Bitmasks (faster for JS and smaller JSON)
    uint16_t rMask = 0;
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
      if (relayGetState((RelayId)i)) rMask |= (1u << i);
    }
    doc["r"] = rMask;

    uint16_t iMask = 0;
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
      if (inputGetState((InputId)i)) iMask |= (1u << i);
    }
    doc["i"] = iMask;

    // Temps 1..8 (keep array, UI already expects it)
    JsonArray temps = doc.createNestedArray("t");
    JsonArray valid = doc.createNestedArray("tv");
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
      temps.add(logicGetTempC(i));
      valid.add(logicIsTempValid(i));
    }

    // Key status fields for instant UI feedback
    EquithermStatus eq = logicGetEquithermStatus();
    JsonObject je = doc.createNestedObject("e");
    je["en"] = eq.enabled;
    je["ac"] = eq.active;
    je["ni"] = eq.night;
    je["to"] = eq.outdoorC;
    je["rs"] = eq.outdoorReason;
    je["tf"] = eq.flowC;
    je["tg"] = eq.targetFlowC;
    je["vp"] = eq.valvePosPct;
    je["vt"] = eq.valveTargetPct;
    je["mv"] = eq.valveMoving;
    je["vm"] = eq.valveMaster; // 1..8, 0=none

    TuvStatus tu = logicGetTuvStatus();
    JsonObject jt = doc.createNestedObject("d");
    jt["en"] = tu.enabled;
    jt["ac"] = tu.active;
    jt["br"] = tu.boilerRelayOn;

    RecircStatus rc = logicGetRecircStatus();
    JsonObject jr = doc.createNestedObject("c");
    jr["en"] = rc.enabled;
    jr["ac"] = rc.active;
    jr["rm"] = (uint32_t)rc.remainingMs;
    // Extra fields for UI widgets/pages
    jr["mo"] = rc.mode;
    jr["rt"] = rc.returnTempC;
    jr["rv"] = rc.returnTempValid;

    AkuHeaterStatus ak = logicGetAkuHeaterStatus();
    JsonObject ja = doc.createNestedObject("a");
    ja["en"] = ak.enabled;
    ja["ac"] = ak.active;
    ja["tp"] = ak.topC;
    ja["tv"] = ak.topValid;
    ja["mo"] = ak.mode;
    ja["rs"] = ak.reason;

    JsonObject jn = doc.createNestedObject("n");
    jn["up"] = networkIsConnected();
    jn["ip"] = networkGetIp();

    // BLE meteo snapshot (used by UI widget + SSE fast updates)
    JsonObject jb = doc.createNestedObject("b");
    bleFillFastJson(jb);

    // OpenTherm snapshot (optional; header provides stubs when disabled)
    OpenThermStatus ot = openthermGetStatus();
    JsonObject jot = doc.createNestedObject("ot");
    jot["en"] = (bool)ot.present; // compiled-in
    jot["rd"] = (bool)ot.ready;
    jot["fl"] = (bool)ot.fault;
    jot["ce"] = (bool)ot.chEnable;
    jot["de"] = (bool)ot.dhwEnable;
    jot["ca"] = (bool)ot.chActive;
    jot["da"] = (bool)ot.dhwActive;
    jot["fo"] = (bool)ot.flameOn;
    jot["bt"] = ot.boilerTempC;
    jot["rt"] = ot.returnTempC;
    jot["dt"] = ot.dhwTempC;
    jot["mt"] = ot.modulationPct;
    jot["pr"] = ot.pressureBar;
    jot["ff"] = ot.faultFlags;
    jot["oc"] = ot.oemFaultCode;
    jot["cs"] = ot.reqChSetpointC;
    jot["ds"] = ot.reqDhwSetpointC;
    jot["mm"] = ot.reqMaxModulationPct;
    jot["lu"] = (uint32_t)ot.lastUpdateMs;
    jot["lc"] = (uint32_t)ot.lastCmdMs;
    jot["rs"] = ot.reason;
    jot["cmd"] = ot.lastCmd;
  }

  static String buildFastStateString(uint32_t* outHash = nullptr) {
    StaticJsonDocument<3584> doc;
    buildFastState(doc);
    String out;
    serializeJson(doc, out);
    if (outHash) *outHash = fnv1a32_str(out);
    return out;
  }

  // ---- API handlers ----
  static void handleFast() {
    uint32_t h = 0;
    const String payload = buildFastStateString(&h);
    char etag[16];
    snprintf(etag, sizeof(etag), "%08lx", (unsigned long)h);

    const String inm = g_server.header("If-None-Match");
    if (inm.length() && inm == etag) {
      g_server.sendHeader("ETag", etag);
      g_server.send(304, "text/plain", "");
      return;
    }

    g_server.sendHeader("ETag", etag);
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.send(200, "application/json", payload);
  }

  // Backward compatible (full-ish) endpoint; keep for older UI.
  static void handleStatus() {
    StaticJsonDocument<8192> doc;
    doc["mode"] = logicModeToString(logicGetMode());
    doc["controlMode"] = (logicGetControlMode() == ControlMode::AUTO) ? "AUTO" : "MANUAL";

    JsonArray rel = doc.createNestedArray("relays");
    for (uint8_t i = 0; i < RELAY_COUNT; i++) rel.add(relayGetState((RelayId)i));

    JsonArray ins = doc.createNestedArray("inputs");
    for (uint8_t i = 0; i < INPUT_COUNT; i++) ins.add(inputGetState((InputId)i));

    JsonArray temps = doc.createNestedArray("temps");
    JsonArray valid = doc.createNestedArray("tempValid");
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
      temps.add(logicGetTempC(i));
      valid.add(logicIsTempValid(i));
    }

    EquithermStatus eq = logicGetEquithermStatus();
    JsonObject jeq = doc.createNestedObject("equitherm");
    jeq["enabled"] = eq.enabled;
    jeq["active"] = eq.active;
    jeq["night"] = eq.night;
    jeq["configOk"] = eq.configOk;
    if (!eq.configReason.isEmpty()) jeq["configReason"] = eq.configReason;
    if (!eq.configWarning.isEmpty()) jeq["configWarning"] = eq.configWarning;
    jeq["outdoorC"] = eq.outdoorC;
    jeq["outdoorValid"] = eq.outdoorValid;
    if (!eq.outdoorReason.isEmpty()) jeq["outdoorReason"] = eq.outdoorReason;
    jeq["flowC"] = eq.flowC;
    jeq["boilerInC"] = eq.boilerInC;
    jeq["boilerInValid"] = eq.boilerInValid;
    jeq["targetFlowC"] = eq.targetFlowC;
    jeq["valvePosPct"] = eq.valvePosPct;
    jeq["valveTargetPct"] = eq.valveTargetPct;
    jeq["valveMoving"] = eq.valveMoving;
    jeq["reason"] = eq.reason;

    TuvStatus tu = logicGetTuvStatus();
    JsonObject jt = doc.createNestedObject("tuv");
    jt["enabled"] = tu.enabled;
    jt["active"] = tu.active;
    jt["reason"] = tu.reason;
    jt["boilerRelayOn"] = tu.boilerRelayOn;

    RecircStatus rc = logicGetRecircStatus();
    JsonObject jr = doc.createNestedObject("recirc");
    jr["enabled"] = rc.enabled;
    jr["active"] = rc.active;
    jr["mode"] = rc.mode;
    jr["remainingMs"] = rc.remainingMs;
    jr["returnTempC"] = rc.returnTempC;

    AkuHeaterStatus ak = logicGetAkuHeaterStatus();
    JsonObject ja = doc.createNestedObject("aku");
    ja["enabled"] = ak.enabled;
    ja["active"] = ak.active;
    ja["reason"] = ak.reason;
    ja["topC"] = ak.topC;

    JsonObject jn = doc.createNestedObject("network");
    jn["connected"] = networkIsConnected();
    jn["ip"] = networkGetIp();
    jn["timeValid"] = networkIsTimeValid();
    jn["timeIso"] = networkGetTimeIso();
    jn["timeSource"] = networkGetTimeSource();

    sendJson(200, doc);
  }

  static const char* dallasStatusToStr(TempInputStatus s) {
    switch (s) {
      case TEMP_STATUS_DISABLED:  return "DISABLED";
      case TEMP_STATUS_OK:        return "OK";
      case TEMP_STATUS_NO_SENSOR: return "NO_SENSOR";
      case TEMP_STATUS_BUS_ERROR: return "BUS_ERROR";
      case TEMP_STATUS_ERROR:     return "ERROR";
      default:                    return "UNKNOWN";
    }
  }

  static void handleDallasStatus() {
    // Rich diagnostics for DS18B20 buses on GPIO0..GPIO3
    StaticJsonDocument<8192> doc;
    JsonArray gpios = doc.createNestedArray("gpios");

    for (uint8_t gpio = 0; gpio <= 3; gpio++) {
      const DallasGpioStatus* st = DallasController::getStatus(gpio);
      JsonObject g = gpios.createNestedObject();
      g["gpio"] = gpio;
      if (!st) {
        g["status"] = "UNSUPPORTED";
        g["deviceCount"] = 0;
        continue;
      }

      g["status"] = dallasStatusToStr(st->status);
      g["lastReadMs"] = st->lastReadMs;
      g["deviceCount"] = (uint32_t)st->devices.size();

      JsonArray devs = g.createNestedArray("devices");
      for (auto &d : st->devices) {
        JsonObject jd = devs.createNestedObject();
        // Convert ROM to 16-hex (same helper used in DallasController.cpp)
        // We do it inline to avoid exposing internal helper.
        uint64_t rom = d.rom;
        char buf[17];
        for (int i = 15; i >= 0; i--) {
          const uint8_t nib = (uint8_t)(rom & 0x0F);
          buf[i] = (nib < 10) ? char('0' + nib) : char('A' + (nib - 10));
          rom >>= 4;
        }
        buf[16] = 0;
        jd["romHex"] = buf;
        jd["tempC"] = d.temperature;
        jd["valid"] = d.valid;
      }
    }

    // Also provide the TEMP1..TEMP8 view used by Logic/UI
    JsonArray slots = doc.createNestedArray("slots");
    for (uint8_t i = 0; i < 8; i++) {
      JsonObject s = slots.createNestedObject();
      s["idx"] = i;
      s["gpio"] = dallasGetSlotGpio(i);
      if (dallasSlotHasFixedRom(i)) {
        const String fr = dallasGetSlotFixedRomHex(i);
        if (fr.length()) s["fixedRomHex"] = fr;
      }
      s["tempC"] = logicGetTempC(i);
      s["valid"] = logicIsTempValid(i);
      const String rom = dallasGetSlotRomHex(i);
      if (rom.length()) s["romHex"] = rom;
    }

    sendJson(200, doc);
  }

  static void handleOpenThermStatus() {
    StaticJsonDocument<2048> doc;

    OpenThermStatus ot = openthermGetStatus();
    OpenThermConfig cfg = openthermGetConfig();

    JsonObject s = doc.createNestedObject("status");
    s["present"] = ot.present;
    s["ready"] = ot.ready;
    s["fault"] = ot.fault;
    s["chEnable"] = ot.chEnable;
    s["dhwEnable"] = ot.dhwEnable;
    s["chActive"] = ot.chActive;
    s["dhwActive"] = ot.dhwActive;
    s["flameOn"] = ot.flameOn;
    s["boilerTempC"] = ot.boilerTempC;
    s["returnTempC"] = ot.returnTempC;
    s["dhwTempC"] = ot.dhwTempC;
    s["roomTempC"] = ot.roomTempC;
    s["outdoorTempC"] = ot.outdoorTempC;
    s["roomSetpointC"] = ot.roomSetpointC;
    s["maxChWaterSetpointC"] = ot.maxChWaterSetpointC;
    s["dhwSetpointC"] = ot.dhwSetpointC;
    s["modulationPct"] = ot.modulationPct;
    s["pressureBar"] = ot.pressureBar;
    s["flowRateLpm"] = ot.flowRateLpm;
    s["powerKw"] = ot.powerKw;
    s["faultFlags"] = ot.faultFlags;
    s["oemFaultCode"] = ot.oemFaultCode;
    s["reqChSetpointC"] = ot.reqChSetpointC;
    s["reqDhwSetpointC"] = ot.reqDhwSetpointC;
    s["reqMaxModulationPct"] = ot.reqMaxModulationPct;
    s["reqRoomSetpointC"] = ot.reqRoomSetpointC;
    s["reqMaxChWaterSetpointC"] = ot.reqMaxChWaterSetpointC;
    s["lastUpdateMs"] = (uint32_t)ot.lastUpdateMs;
    s["lastCmdMs"] = (uint32_t)ot.lastCmdMs;
    s["reason"] = ot.reason;
    s["lastCmd"] = ot.lastCmd;

    s["totalCount"] = (uint32_t)ot.totalCount;
    s["okCount"] = (uint32_t)ot.okCount;
    s["timeoutCount"] = (uint32_t)ot.timeoutCount;
    s["frameErrorCount"] = (uint32_t)ot.frameErrorCount;
    s["parityErrorCount"] = (uint32_t)ot.parityErrorCount;
    s["badResponseCount"] = (uint32_t)ot.badResponseCount;
    s["notInitializedCount"] = (uint32_t)ot.notInitializedCount;
    s["isrOverflowCount"] = (uint32_t)ot.isrOverflowCount;


    // Raw polled values (subset of protocol IDs)
    JsonArray raw = doc.createNestedArray("raw");
    OpenThermRawValue rv[24];
    const uint8_t rn = openthermGetRaw(rv, 24);
    for (uint8_t i = 0; i < rn; i++) {
      JsonObject r = raw.createNestedObject();
      r["id"] = rv[i].id;
      r["msgType"] = rv[i].msgType;
      r["u16"] = rv[i].u16;
      r["f88"] = rv[i].f88;
      r["valid"] = rv[i].valid;
      r["tsMs"] = (uint32_t)rv[i].tsMs;
    }

    JsonObject c = doc.createNestedObject("config");
    c["enabled"] = cfg.enabled;
    c["pollMs"] = (uint32_t)cfg.pollMs;
    c["minChSetpointC"] = cfg.minChSetpointC;
    c["maxChSetpointC"] = cfg.maxChSetpointC;
    c["boilerControl"] = (cfg.boilerControl == OpenThermBoilerControl::OPENTHERM)
                            ? "opentherm"
                            : (cfg.boilerControl == OpenThermBoilerControl::HYBRID ? "hybrid" : "relay");
    c["mapEquithermChSetpoint"] = cfg.mapEquithermChSetpoint;
    c["mapDhw"] = cfg.mapDhw;
    c["mapNightMode"] = cfg.mapNightMode;
    c["dhwSetpointC"] = cfg.dhwSetpointC;
    c["dhwBoostChSetpointC"] = cfg.dhwBoostChSetpointC;
    c["roomSetpointC"] = cfg.roomSetpointC;
    c["maxChWaterSetpointC"] = cfg.maxChWaterSetpointC;
    c["txPin"] = cfg.txPin;
    c["rxPin"] = cfg.rxPin;
    c["assumedMaxBoilerKw"] = cfg.assumedMaxBoilerKw;
    c["invertTx"] = cfg.invertTx;
    c["invertRx"] = cfg.invertRx;
    c["autoDetectLogic"] = cfg.autoDetectLogic;
    c["logEnabled"] = cfg.logEnabled;
    c["logIntervalMs"] = (uint32_t)cfg.logIntervalMs;
    JsonArray pids = c.createNestedArray("pollIds");
    for (uint8_t i = 0; i < cfg.pollIdCount; i++) pids.add(cfg.pollIds[i]);

    sendJson(200, doc);
  }

  static void handleOpenThermCmd() {
    const String body = g_server.arg("plain");
    if (!body.length()) {
      g_server.send(400, "text/plain", "empty");
      return;
    }
    DynamicJsonDocument doc(1024);
    DeserializationError e = deserializeJson(doc, body);
    if (e || !doc.is<JsonObject>()) {
      g_server.send(400, "text/plain", "bad_json");
      return;
    }
    JsonObject o = doc.as<JsonObject>();

    if (!o["chEnable"].isNull() || !o["dhwEnable"].isNull()) {
      const bool ch = (bool)(o["chEnable"] | false);
      const bool dhw = (bool)(o["dhwEnable"] | false);
      openthermCmdSetEnable(ch, dhw);
    }
    if (!o["chSetpointC"].isNull()) {
      openthermCmdSetChSetpoint((float)(o["chSetpointC"] | NAN));
    }
    if (!o["dhwSetpointC"].isNull()) {
      openthermCmdSetDhwSetpoint((float)(o["dhwSetpointC"] | NAN));
    }
    if (!o["maxModulationPct"].isNull()) {
      openthermCmdSetMaxModulation((float)(o["maxModulationPct"] | NAN));
    }
    if (!o["roomSetpointC"].isNull()) {
      openthermCmdSetRoomSetpoint((float)(o["roomSetpointC"] | NAN));
    }
    if (!o["maxChWaterSetpointC"].isNull()) {
      openthermCmdSetMaxChWaterSetpoint((float)(o["maxChWaterSetpointC"] | NAN));
    }
    if ((bool)(o["resetFault"] | false)) {
      openthermCmdResetFault();
    }

    webserverNotifyStateChanged();
    g_server.send(200, "text/plain", "ok");
  }

  static void handleRelay() {
    int id = argOrEmpty("id").toInt(); // 1..8
    String cmd = argOrEmpty("cmd");
    cmd.toLowerCase();
    if (id < 1 || id > 8) {
      g_server.send(400, "text/plain", "bad id");
      return;
    }
    const uint8_t r1 = (uint8_t)id;
    if (cmd == "on") logicSetRelayOutput(r1, true);
    else if (cmd == "off") logicSetRelayOutput(r1, false);
    else if (cmd == "toggle") logicSetRelayOutput(r1, !relayGetState((RelayId)(id - 1)));
    else if (cmd == "raw_on") logicSetRelayRaw(r1, true);
    else if (cmd == "raw_off") logicSetRelayRaw(r1, false);
    else {
      g_server.send(400, "text/plain", "bad cmd");
      return;
    }
    webserverNotifyStateChanged();
    g_server.send(200, "text/plain", "ok");
  }

  // --- Valve commissioning / troubleshooting helpers ---
  // NOTE: These endpoints intentionally use query parameters to keep the firmware small.
  //   POST /api/valve/pulse?master=1&dir=1    (dir: +1=open / -1=close)
  //   POST /api/valve/goto?master=1&pct=50   (pct: 0..100)
  //   POST /api/valve/stop?master=1
  static void handleValvePulse() {
    const int master = argOrEmpty("master").toInt();
    const int dir    = argOrEmpty("dir").toInt();
    StaticJsonDocument<256> doc;
    if (master < 1 || master > 8 || (dir != 1 && dir != -1)) {
      doc["ok"] = false;
      doc["err"] = "bad_args";
      sendJson(400, doc);
      return;
    }
    const bool ok = logicValvePulse((uint8_t)master, (int8_t)dir);
    doc["ok"] = ok;
    sendJson(ok ? 200 : 409, doc);
  }

  static void handleValveGoto() {
    const int master = argOrEmpty("master").toInt();
    const int pct    = argOrEmpty("pct").toInt();
    StaticJsonDocument<256> doc;
    if (master < 1 || master > 8 || pct < 0 || pct > 100) {
      doc["ok"] = false;
      doc["err"] = "bad_args";
      sendJson(400, doc);
      return;
    }
    const bool ok = logicValveGotoPct((uint8_t)master, (uint8_t)pct);
    doc["ok"] = ok;
    sendJson(ok ? 200 : 409, doc);
  }

  static void handleValveStop() {
    const int master = argOrEmpty("master").toInt();
    StaticJsonDocument<256> doc;
    if (master < 1 || master > 8) {
      doc["ok"] = false;
      doc["err"] = "bad_args";
      sendJson(400, doc);
      return;
    }
    const bool ok = logicValveStop((uint8_t)master);
    doc["ok"] = ok;
    sendJson(ok ? 200 : 409, doc);
  }

  // --- Equitherm: mixing valve calibration (HOME) ---
  // POST /api/equitherm/calibrate
  // Optional query params:
  //   master=1..8   (default: equitherm mixing valve)
  //   home=0|100    (default: equitherm.homing.positionPct)
  static void handleEquithermCalibrate() {
    const int master = argOrEmpty("master").toInt();
    const int home   = argOrEmpty("home").toInt();
    StaticJsonDocument<256> doc;

    bool ok = false;
    if (master >= 1 && master <= 8) {
      const uint8_t hp = (home >= 50) ? 100 : 0;
      ok = logicValveCalibrateHome((uint8_t)master, hp);
    } else {
      ok = logicEquithermCalibrateHome();
    }

    doc["ok"] = ok;
    sendJson(ok ? 200 : 409, doc);
  }

  static void handleConfigGet() {
  String json;
  if (!fsReadTextFile("/config.json", json)) {
    g_server.send(404, "text/plain", "config not found");
    return;
  }
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.send(200, "application/json", json);
}

static void handleConfigApply() {
  String body = g_server.arg("plain");
  if (!body.length()) {
    g_server.send(400, "text/plain", "empty");
    return;
  }

  // Merge PATCH-like updates: if body is a partial JSON (e.g. only "equitherm"),
  // keep the rest of the existing /config.json.
  String current;
  bool hasCurrent = fsReadTextFile("/config.json", current);

  // Fast path: no existing config -> just write
  if (!hasCurrent || !current.length()) {
    fsWriteTextFile("/config.json", body);
    webserverLoadConfigFromFS();
    webserverNotifyStateChanged();
    g_server.send(200, "text/plain", "ok");
    return;
  }

  // Try merge. If anything fails (OOM/parse), fall back to overwrite (legacy behavior).
  DynamicJsonDocument docOld(16384);
  DynamicJsonDocument docNew(16384);

  DeserializationError eOld = deserializeJson(docOld, current);
  DeserializationError eNew = deserializeJson(docNew, body);

  if (!eOld && !eNew && docOld.is<JsonObject>() && docNew.is<JsonObject>()) {
    JsonObject oOld = docOld.as<JsonObject>();
    JsonObject oNew = docNew.as<JsonObject>();

    for (JsonPair kv : oNew) {
      // Overwrite/insert top-level key
      oOld[kv.key()] = kv.value();
    }

    String merged;
    serializeJson(docOld, merged);
    fsWriteTextFile("/config.json", merged);
  } else {
    // Legacy: overwrite
    fsWriteTextFile("/config.json", body);
  }

  webserverLoadConfigFromFS();
  webserverNotifyStateChanged();
  g_server.send(200, "text/plain", "ok");
}

static void handleNetworkPortalStart() {
  // Starts WiFiManager config portal on next boot, then reboots.
  // Use-case: device reachable via Ethernet but Wi-Fi credentials need to be changed.
  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["rebooting"] = true;
  sendJson(200, doc);
  // Give TCP stack a moment to flush.
  delay(120);
  networkRequestConfigPortal();
}

  // ---- Device helpers (UI tests) ----
  static void handleDeviceBeep() {
    // Optional JSON body:
    // {"mode":"beep"|"gong"|"sound", "sound":"chime"|"gong"|"alert"|"off", "freqHz":2000, "durationMs":80, "repeats":1}
    const String body = g_server.arg("plain");
    if (body.length()) {
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, body)) {
        const String mode = doc["mode"] | "beep";
        if (mode == "gong") {
          const int rep = (int)(doc["repeats"] | 1);
          buzzerGong((uint8_t)constrain(rep, 1, 10));
        } else if (mode == "sound") {
          const String sid = doc["sound"] | "chime";
          const int rep = (int)(doc["repeats"] | 1);
          buzzerPlaySound(sid.c_str(), (uint8_t)constrain(rep, 1, 10));
        } else {
          // legacy beep
          const int f = (int)(doc["freqHz"] | 0);
          const int d = (int)(doc["durationMs"] | 0);
          buzzerBeep((uint16_t)constrain(f, 0, 10000), (uint16_t)constrain(d, 0, 2000));
        }
      } else {
        // bad json -> fall back to default beep
        buzzerBeep(0, 0);
      }
    } else {
      // uses configured defaults (0,0 => defaults)
      buzzerBeep(0, 0);
    }
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.send(200, "text/plain", "ok");
  }

  static void handleDeviceLed() {
    const String body = g_server.arg("plain");
    if (!body.length()) {
      g_server.send(400, "text/plain", "empty");
      return;
    }
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, body)) {
      g_server.send(400, "text/plain", "bad_json");
      return;
    }
    // ArduinoJson supports the `|` default operator on JsonVariant.
    // Do NOT cast to const char* before applying `|`, otherwise the operator
    // becomes a C/C++ bitwise OR and compilation fails.
    const String mode = doc["mode"] | "";
    if (mode == "off") {
      rgbLedOff();
    } else if (mode == "solid") {
      const int r = (int)(doc["r"] | 0);
      const int g = (int)(doc["g"] | 0);
      const int b = (int)(doc["b"] | 0);
      rgbLedSetColor((uint8_t)constrain(r,0,255), (uint8_t)constrain(g,0,255), (uint8_t)constrain(b,0,255));
    } else if (mode == "blink") {
      const int r = (int)(doc["r"] | 0);
      const int g = (int)(doc["g"] | 0);
      const int b = (int)(doc["b"] | 0);
      const int p = (int)(doc["periodMs"] | 500);
      rgbLedBlink((uint8_t)constrain(r,0,255), (uint8_t)constrain(g,0,255), (uint8_t)constrain(b,0,255), (uint16_t)constrain(p,50,5000));
    } else {
      g_server.send(400, "text/plain", "bad_mode");
      return;
    }
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.send(200, "text/plain", "ok");
  }

  // ---- UI layout (Dashboard) ----
  // GET  /api/ui/layout?profile=default
  // POST /api/ui/layout?profile=default   (body: JSON)
  // GET  /api/ui/layout/profiles
  // POST /api/ui/layout/delete?profile=...
  static void handleUiLayoutGet() {
    const String profile = uiProfileSanitize(argOrEmpty("profile"));
    const String path = uiLayoutPathForProfile(profile);
    String json;
    if (!fsReadTextFile(path.c_str(), json)) {
      // No layout saved yet -> return empty layout, so UI can fall back without console noise.
      g_server.sendHeader("Cache-Control", "no-store");
      g_server.send(200, "application/json", "[]");
      return;
    }
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.send(200, "application/json", json);
  }

  static void handleUiLayoutSet() {
    const String profile = uiProfileSanitize(argOrEmpty("profile"));
    const String path = uiLayoutPathForProfile(profile);
    const String body = g_server.arg("plain");
    if (!body.length()) {
      g_server.send(400, "text/plain", "empty");
      return;
    }

    // Validate JSON briefly (prevent writing junk)
    DynamicJsonDocument doc(8192);
    DeserializationError e = deserializeJson(doc, body);
    if (e) {
      g_server.send(400, "text/plain", "bad_json");
      return;
    }
    fsWriteTextFile(path.c_str(), body);
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.send(200, "text/plain", "ok");
  }

  static void handleUiLayoutProfiles() {
    StaticJsonDocument<4096> doc;
    JsonArray arr = doc.createNestedArray("profiles");
    fsLock();
    File root = LittleFS.open("/", "r");
    File f = root.openNextFile();
    while (f) {
      const String name = String(f.name());
      if (name.startsWith("/ui_layout_") && name.endsWith(".json")) {
        String p = name.substring(strlen("/ui_layout_"));
        p = p.substring(0, p.length() - strlen(".json"));
        arr.add(p);
      }
      f = root.openNextFile();
    }
    root.close();
    fsUnlock();
    sendJson(200, doc);
  }

  static void handleUiLayoutDelete() {
    const String profile = uiProfileSanitize(argOrEmpty("profile"));
    const String path = uiLayoutPathForProfile(profile);
    fsLock();
    const bool ok = LittleFS.remove(path.c_str());
    fsUnlock();
    g_server.send(ok ? 200 : 404, "text/plain", ok ? "ok" : "not found");
  }

  static void handleFsList() {
    StaticJsonDocument<4096> doc;
    JsonArray arr = doc.createNestedArray("files");
    fsLock();
    File root = LittleFS.open("/", "r");
    File f = root.openNextFile();
    while (f) {
      JsonObject o = arr.createNestedObject();
      o["name"] = String(f.name());
      o["size"] = (uint32_t)f.size();
      f = root.openNextFile();
    }
    root.close();
    fsUnlock();
    sendJson(200, doc);
  }

  // ---------------- File manager ----------------
  static bool isSafePath(const String& in) {
    if (!in.length()) return false;
    if (in[0] != '/') return false;
    if (in.indexOf("..") >= 0) return false;
    if (in.length() > 96) return false;
    return true;
  }

  static void handleFsInfo() {
    StaticJsonDocument<512> doc;
    fsLock();
    bool ok = LittleFS.begin(true);
    if (!ok) { fsUnlock(); doc["ok"] = false; sendJson(500, doc); return; }
    doc["ok"] = true;
    doc["totalBytes"] = (uint32_t)LittleFS.totalBytes();
    doc["usedBytes"] = (uint32_t)LittleFS.usedBytes();
    fsUnlock();
    sendJson(200, doc);
  }

  static void handleFsRead() {
    const String path = g_server.arg("path");
    if (!isSafePath(path)) { g_server.send(400, "text/plain", "bad path"); return; }
    fsLock();
    if (!LittleFS.begin(true)) { fsUnlock(); g_server.send(500, "text/plain", "LittleFS error"); return; }
    File f = LittleFS.open(path, FILE_READ);
    if (!f) { fsUnlock(); g_server.send(404, "text/plain", "not found"); return; }
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.streamFile(f, "text/plain");
    f.close();
    fsUnlock();
  }

  static void handleFsWrite() {
    const String path = g_server.arg("path");
    if (!isSafePath(path)) { g_server.send(400, "text/plain", "bad path"); return; }
    const String body = g_server.arg("plain");
    fsLock();
    if (!LittleFS.begin(true)) { fsUnlock(); g_server.send(500, "text/plain", "LittleFS error"); return; }
    File f = LittleFS.open(path, "w");
    if (!f) { fsUnlock(); g_server.send(500, "text/plain", "open failed"); return; }
    f.print(body);
    f.close();
    fsUnlock();
    g_server.send(200, "text/plain", "OK");
  }

  static void handleFsDelete() {
    const String path = g_server.arg("path");
    if (!isSafePath(path)) { g_server.send(400, "text/plain", "bad path"); return; }
    fsLock();
    if (!LittleFS.begin(true)) { fsUnlock(); g_server.send(500, "text/plain", "LittleFS error"); return; }
    bool ok = LittleFS.remove(path);
    fsUnlock();
    g_server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "ERR");
  }

  // Forward declaration (used before definition further in this file)
  static void streamDownload(const char* path, const char* filename, const char* ctype);

  static void handleFsDownload() {
    const String path = g_server.arg("path");
    if (!isSafePath(path)) { g_server.send(400, "text/plain", "bad path"); return; }
    String fn = path;
    int slash = fn.lastIndexOf('/');
    if (slash >= 0) fn = fn.substring(slash + 1);
    streamDownload(path.c_str(), fn.c_str(), "application/octet-stream");
  }

  static String g_fsUploadPath;
  static void handleFsUpload() {
    // final response after upload
    StaticJsonDocument<256> doc;
    doc["ok"] = true;
    sendJson(200, doc);
  }

  static void handleFsUploadBody() {
    HTTPUpload& up = g_server.upload();
    if (up.status == UPLOAD_FILE_START) {
      g_fsUploadPath = g_server.arg("path");
      if (!isSafePath(g_fsUploadPath)) {
        g_fsUploadPath = "";
        return;
      }
      fsLock();
      LittleFS.begin(true);
      File f = LittleFS.open(g_fsUploadPath, "w");
      if (f) f.close();
      fsUnlock();
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (!g_fsUploadPath.length()) return;
      fsLock();
      if (!LittleFS.begin(true)) { fsUnlock(); return; }
      File f = LittleFS.open(g_fsUploadPath, FILE_APPEND);
      if (f) {
        f.write(up.buf, up.currentSize);
        f.close();
      }
      fsUnlock();
    } else if (up.status == UPLOAD_FILE_END) {
      // nothing
    }
  }

  // ---------------- OTA (web) ----------------
  struct OtaState {
    bool inProgress = false;
    uint32_t written = 0;
    uint32_t total = 0;
    String lastError = "";
    bool rebootPending = false;
    uint32_t rebootAtMs = 0;
  };
  static OtaState g_ota;

  static void handleOtaStatus() {
    StaticJsonDocument<384> doc;
    doc["inProgress"] = g_ota.inProgress;
    doc["written"] = g_ota.written;
    doc["total"] = g_ota.total;
    doc["progress"] = (g_ota.total ? (float)g_ota.written / (float)g_ota.total : 0.0f);
    doc["lastError"] = g_ota.lastError;
    doc["rebootPending"] = g_ota.rebootPending;
    sendJson(200, doc);
  }

  static void handleOtaUpdateDone() {
    // response after upload end
    StaticJsonDocument<256> doc;
    const bool ok = (!g_ota.lastError.length());
    doc["ok"] = ok;
    doc["error"] = g_ota.lastError;
    sendJson(ok ? 200 : 500, doc);
  }

  static void handleOtaUpdateBody() {
    HTTPUpload& up = g_server.upload();
    if (up.status == UPLOAD_FILE_START) {
      g_ota.inProgress = true;
      g_ota.written = 0;
      g_ota.total = up.totalSize;
      g_ota.lastError = "";
      g_ota.rebootPending = false;

      if (!Update.begin(up.totalSize, U_FLASH)) {
        g_ota.lastError = String("Update.begin failed: ") + Update.errorString();
      }
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (g_ota.lastError.length()) return;
      const size_t w = Update.write(up.buf, up.currentSize);
      g_ota.written += (uint32_t)w;
      if (w != up.currentSize) {
        g_ota.lastError = String("Update.write failed: ") + Update.errorString();
      }
    } else if (up.status == UPLOAD_FILE_END) {
      if (!g_ota.lastError.length()) {
        if (!Update.end(true)) {
          g_ota.lastError = String("Update.end failed: ") + Update.errorString();
        } else {
          g_ota.rebootPending = true;
          g_ota.rebootAtMs = millis() + 1500;
        }
      }
      g_ota.inProgress = false;
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      g_ota.inProgress = false;
      g_ota.lastError = "upload aborted";
      Update.abort();
    }
  }

  static void handleEvents() {
    // Start SSE stream.
    // IMPORTANT:
    // Do NOT use WebServer::send() with CONTENT_LENGTH_UNKNOWN here.
    // That makes the response "Transfer-Encoding: chunked", but we then write
    // raw SSE frames via WiFiClient::print(), which breaks chunk framing and
    // browsers report: net::ERR_INVALID_CHUNKED_ENCODING.
    //
    // Instead, write a plain HTTP/1.1 response without Content-Length and
    // without chunked encoding, and keep the connection open.

    WiFiClient c = g_server.client();
    c.setNoDelay(true);

    c.print("HTTP/1.1 200 OK\r\n");
    c.print("Content-Type: text/event-stream\r\n");
    c.print("Cache-Control: no-cache\r\n");
    c.print("Connection: keep-alive\r\n");
    c.print("X-Accel-Buffering: no\r\n");
    c.print("\r\n");

    // Optional: tell the browser how long to wait before reconnect.
    c.print("retry: 2000\n\n");

    // Find slot
    int slot = -1;
    for (uint8_t i = 0; i < kMaxSseClients; i++) {
      if (!g_sse[i].alive || !g_sse[i].client.connected()) {
        slot = (int)i;
        break;
      }
    }
    if (slot < 0) {
      // No slot available; close.
      c.stop();
      return;
    }

    g_sse[slot].client = c;
    g_sse[slot].alive = true;
    g_sse[slot].lastSendMs = 0;
    g_sse[slot].lastStateHash = 0;

    // Send initial state immediately.
    uint32_t h = 0;
    const String payload = buildFastStateString(&h);
    g_sse[slot].client.print("data: ");
    g_sse[slot].client.print(payload);
    g_sse[slot].client.print("\n\n");
    g_sse[slot].lastSendMs = millis();
    g_sse[slot].lastStateHash = h;
  }

  static void sseBroadcastIfNeeded() {
    // Rate limit: don't spam faster than ~4 Hz unless explicitly dirtied.
    const uint32_t now = millis();
    const bool dirty = g_stateDirty;
    if (!dirty) {
      // Still refresh occasionally so UI can recover from missed packets.
      static uint32_t lastPeriodicMs = 0;
      if ((uint32_t)(now - lastPeriodicMs) < 1000) return;
      lastPeriodicMs = now;
    }
    g_stateDirty = false;

    uint32_t h = 0;
    const String payload = buildFastStateString(&h);

    for (uint8_t i = 0; i < kMaxSseClients; i++) {
      if (!g_sse[i].alive) continue;
      if (!g_sse[i].client.connected()) {
        g_sse[i].client.stop();
        g_sse[i].alive = false;
        continue;
      }

      // Per-client throttle and change detection
      if (!dirty && h == g_sse[i].lastStateHash) continue;
      if (dirty && (uint32_t)(now - g_sse[i].lastSendMs) < 250) continue;

      g_sse[i].client.print("data: ");
      g_sse[i].client.print(payload);
      g_sse[i].client.print("\n\n");
      g_sse[i].lastSendMs = now;
      g_sse[i].lastStateHash = h;
    }
  }

  // ---- Static files ----
  static bool clientAcceptsGzip() {
    const String enc = g_server.header("Accept-Encoding");
    return enc.indexOf("gzip") >= 0;
  }

  static String guessContentType(const String& path) {
    if (path.endsWith(".html")) return "text/html";
    if (path.endsWith(".css")) return "text/css";
    if (path.endsWith(".js")) return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".svg")) return "image/svg+xml";
    if (path.endsWith(".ico")) return "image/x-icon";
    return "application/octet-stream";
  }

  static bool isCacheableAsset(const String& path) {
    // For UI assets we can cache aggressively. If you use hashed filenames,
    // this becomes perfectly safe.
    return path.endsWith(".css") || path.endsWith(".js") || path.endsWith(".png") ||
           path.endsWith(".jpg") || path.endsWith(".jpeg") || path.endsWith(".svg") ||
           path.endsWith(".ico");
  }

  static void serveStaticFile(const String& uriPath) {
    String path = uriPath;
    if (path == "/") path = "/index.html";

    const bool wantGz = clientAcceptsGzip();
    const String gzPath = path + ".gz";
    const bool hasGz = wantGz && LittleFS.exists(gzPath);

    const String servePath = hasGz ? gzPath : path;
    if (!LittleFS.exists(servePath)) {
      g_server.send(404, "text/plain", "not found");
      return;
    }

    fsLock();
    File f = LittleFS.open(servePath, "r");
    const String ctype = guessContentType(path); // note: original path, not .gz
    if (hasGz) g_server.sendHeader("Content-Encoding", "gzip");

    if (isCacheableAsset(path)) {
      g_server.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
    } else {
      // index.html: short cache so updates propagate.
      g_server.sendHeader("Cache-Control", "public, max-age=60");
    }

    g_server.streamFile(f, ctype);
    f.close();
    fsUnlock();
  }

  
  static void streamDownload(const char* path, const char* filename, const char* ctype) {
    fsLock();
    if (!LittleFS.begin(true)) { fsUnlock(); g_server.send(500,"text/plain","LittleFS error"); return; }
    File f = LittleFS.open(path, FILE_READ);
    if (!f) { fsUnlock(); g_server.send(404,"text/plain","Not found"); return; }
    g_server.sendHeader("Cache-Control","no-store");
    g_server.sendHeader("Content-Disposition", String("attachment; filename=\"") + filename + "\"");
    g_server.streamFile(f, ctype);
    f.close();
    fsUnlock();
  }

static void handleNotFound() {
    serveStaticFile(g_server.uri());
  }
}

void webserverNotifyStateChanged() {
  g_stateDirty = true;
}

void webserverLoadConfigFromFS() {
  String json;
  if (!fsReadTextFile("/config.json", json)) return;

  networkApplyConfig(json);
  rgbLedApplyConfig(json);
  buzzerApplyConfig(json);
  dallasApplyConfig(json);
  bleApplyConfig(json);
  openthermApplyConfig(json);
#if FEATURE_HEATLOSS
  heatlossApplyConfig(json);
#endif
  // Roles/thermometer sources are used by logicApplyConfig() role fallback.
  // Apply them first so Equitherm (and other functions) can use BLE/MQTT roles immediately.
  thermometersApplyConfig(json);
  logicApplyConfig(json);
}

void webserverInit() {
  if (g_inited) return;
  g_inited = true;

  // API
  g_server.on("/api/fast", HTTP_GET, handleFast);
  g_server.on("/api/status", HTTP_GET, handleStatus);
  g_server.on("/api/dallas", HTTP_GET, handleDallasStatus);
  g_server.on("/api/ble/status", HTTP_GET, []() {
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.send(200, "application/json", bleGetStatusJson());
  });
  g_server.on("/api/opentherm/status", HTTP_GET, handleOpenThermStatus);
  g_server.on("/api/opentherm/protocol", HTTP_GET, [](){ g_server.sendHeader("Cache-Control","no-store"); g_server.send(200,"application/json", openthermGetProtocolJson()); });
  g_server.on("/api/opentherm/cmd", HTTP_POST, handleOpenThermCmd);
  g_server.on("/api/opentherm/log", HTTP_GET, [](){
    fsLock();
    if (!LittleFS.begin(true)) { fsUnlock(); g_server.send(500,"text/plain","LittleFS error"); return; }
    File f = LittleFS.open("/opentherm.csv", FILE_READ);
    if (!f) { fsUnlock(); g_server.send(404,"text/plain","No log"); return; }
    g_server.sendHeader("Cache-Control","no-store");
    g_server.streamFile(f, "text/csv");
    f.close();
    fsUnlock();
  });
  g_server.on("/api/opentherm/clear", HTTP_POST, [](){
    fsLock();
    if (!LittleFS.begin(true)) { fsUnlock(); g_server.send(500,"text/plain","LittleFS error"); return; }
    bool ok = LittleFS.remove("/opentherm.csv");
    fsUnlock();
    g_server.send(ok?200:500,"text/plain", ok?"OK":"ERR");
  });

  // Simple downloads (CSV)
  g_server.on("/download/opentherm.csv", HTTP_GET, [](){ streamDownload("/opentherm.csv","opentherm.csv","text/csv"); });
#if FEATURE_HEATLOSS
  g_server.on("/download/heatloss.csv", HTTP_GET, [](){ streamDownload("/heatloss.csv","heatloss.csv","text/csv"); });
#endif


#if FEATURE_HEATLOSS
  g_server.on("/api/heatloss/status", HTTP_GET, [](){
    StaticJsonDocument<2048> doc;
    HeatLossStatus st = heatlossGetStatus();
    HeatLossConfig cfg = heatlossGetConfig();

    JsonObject s = doc.createNestedObject("status");
    s["enabled"] = st.enabled;
    s["haveSample"] = st.haveSample;
    s["indoorC"] = st.indoorC;
    s["outdoorC"] = st.outdoorC;
    s["powerKw"] = st.powerKw;
    s["ua_W_per_K"] = st.ua_W_per_K;
    s["projectedLossKw"] = st.projectedLossKw;
    s["samples"] = st.samples;
    s["lastLogMs"] = st.lastLogMs;
    s["reason"] = st.reason;

    JsonObject c = doc.createNestedObject("config");
    c["enabled"] = cfg.enabled;
    c["logIntervalMs"] = cfg.logIntervalMs;
    c["windowSec"] = cfg.windowSec;
    c["designOutdoorC"] = cfg.designOutdoorC;
    c["indoorTargetC"] = cfg.indoorTargetC;
    c["indoorSource"] = cfg.indoorSource;
    c["outdoorSource"] = cfg.outdoorSource;
    c["assumedMaxBoilerKw"] = cfg.assumedMaxBoilerKw;


    sendJson(200, doc);
  });

  g_server.on("/api/heatloss/clear", HTTP_POST, [](){
    bool ok = heatlossClearLog();
    StaticJsonDocument<256> doc;
    doc["ok"] = ok;
    sendJson(ok ? 200 : 500, doc);
  });

  g_server.on("/api/heatloss/log", HTTP_GET, [](){
    if (!LittleFS.begin(true)) { g_server.send(500, "text/plain", "LittleFS not available"); return; }
    const String path = heatlossGetLogPath();
    if (!LittleFS.exists(path)) { g_server.send(404, "text/plain", "log not found"); return; }
    File f = LittleFS.open(path, FILE_READ);
    if (!f) { g_server.send(500, "text/plain", "open failed"); return; }
    g_server.streamFile(f, "text/csv");
    f.close();
  });
#endif
  g_server.on("/api/relay", HTTP_GET, handleRelay);
  g_server.on("/api/valve/pulse", HTTP_POST, handleValvePulse);
  g_server.on("/api/valve/goto", HTTP_POST, handleValveGoto);
  g_server.on("/api/valve/stop", HTTP_POST, handleValveStop);
  g_server.on("/api/equitherm/calibrate", HTTP_POST, handleEquithermCalibrate);
  g_server.on("/api/config", HTTP_GET, handleConfigGet);
  g_server.on("/api/config/apply", HTTP_POST, handleConfigApply);
  g_server.on("/api/network/portal", HTTP_POST, handleNetworkPortalStart);
  g_server.on("/api/device/beep", HTTP_POST, handleDeviceBeep);
  g_server.on("/api/device/led", HTTP_POST, handleDeviceLed);
  g_server.on("/api/ui/layout", HTTP_GET, handleUiLayoutGet);
  g_server.on("/api/ui/layout", HTTP_POST, handleUiLayoutSet);
  g_server.on("/api/ui/layout/profiles", HTTP_GET, handleUiLayoutProfiles);
  g_server.on("/api/ui/layout/delete", HTTP_POST, handleUiLayoutDelete);
  g_server.on("/api/fs/list", HTTP_GET, handleFsList);
  g_server.on("/api/fs/info", HTTP_GET, handleFsInfo);
  g_server.on("/api/fs/read", HTTP_GET, handleFsRead);
  g_server.on("/api/fs/write", HTTP_POST, handleFsWrite);
  g_server.on("/api/fs/delete", HTTP_POST, handleFsDelete);
  g_server.on("/api/fs/download", HTTP_GET, handleFsDownload);
  g_server.on("/api/fs/upload", HTTP_POST, handleFsUpload, handleFsUploadBody);

  g_server.on("/api/ota/status", HTTP_GET, handleOtaStatus);
  g_server.on("/api/ota/update", HTTP_POST, handleOtaUpdateDone, handleOtaUpdateBody);
  g_server.on("/api/events", HTTP_GET, handleEvents);

  // Static
  g_server.onNotFound(handleNotFound);
  g_server.begin();
  LOGI("WebServer started on :80 (SSE: /api/events)");
}

void webserverLoop() {
  if (!g_inited) return;
  g_server.handleClient();
  sseBroadcastIfNeeded();

  // Perform reboot after successful OTA update.
  if (g_ota.rebootPending) {
    const uint32_t now = millis();
    if ((int32_t)(now - g_ota.rebootAtMs) >= 0) {
      ESP.restart();
    }
  }
}

#endif // FEATURE_WEBSERVER
