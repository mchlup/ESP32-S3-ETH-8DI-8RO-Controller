#include "WebServerController.h"

#include <Arduino.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include "FsController.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <math.h>
#include "config_pins.h"
#include <ctype.h>

#include "RelayController.h"
#include "BuzzerController.h"
#include "InputController.h"
#include "LogicController.h"
#include "NetworkController.h"
#include "MqttController.h"
#include "BleController.h"
#include "ThermometerController.h"
#include "TempParse.h"
#include "OpenThermController.h"
#include "RuleEngine.h"
#include "NtcController.h"
#include "DallasController.h"

static WebServer server(80);
static File uploadFile;

// Ne blokující restart (naplánovaný)
static bool     g_rebootPending = false;
static uint32_t g_rebootAtMs    = 0;

// Globální JSON konfigurace (popisy + mapování relé + režimy)
static String g_configJson;

static void applyAllConfig(const String& json){
    networkApplyConfig(json);
    ntcApplyConfig(json);
    dallasApplyConfig(json);
    thermometersApplyConfig(json);
    openthermApplyConfig(json);
    // Logika může doplnit/změnit seznam MQTT topiců (např. ekviterm + MQTT teploměry).
    // Proto aplikuj logiku dříve a MQTT až poté (aby subscribe odpovídal nové konfiguraci).
    logicApplyConfig(json);
    mqttApplyConfig(json);
}


// Rule engine JSON (uložené v LittleFS jako /rules.json)
#if FEATURE_RULE_ENGINE
    static String g_rulesJson;
    static bool   g_rulesEnabled = false;
#endif

// ===== JSON helper =====

static String jsonEscape(const String& in) {
    String out;
    out.reserve(in.length() + 16);
    for (size_t i = 0; i < in.length(); i++) {
        const char c = in[i];
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((uint8_t)c < 0x20) {
                    // control chars -> \u00XX
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04X", (unsigned int)(uint8_t)c);
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

// ===== Pomocné funkce pro FS =====

static bool handleFileRead(const String& path) {
    String p = path;
    if (p.endsWith("/")) p += "index.html";

    String contentType = "text/plain";
    if (p.endsWith(".html")) contentType = "text/html";
    else if (p.endsWith(".css")) contentType = "text/css";
    else if (p.endsWith(".js")) contentType = "application/javascript";
    else if (p.endsWith(".json")) contentType = "application/json";
    else if (p.endsWith(".png")) contentType = "image/png";
    else if (p.endsWith(".jpg") || p.endsWith(".jpeg")) contentType = "image/jpeg";
    else if (p.endsWith(".svg")) contentType = "image/svg+xml";
    else if (p.endsWith(".ico")) contentType = "image/x-icon";

    if (!LittleFS.exists(p)) return false;

    File f = LittleFS.open(p, "r");
    if (!f) return false;

    // UI soubory nechceme cachovat – typicky řeší "změny se neprojevily".
    // (index.html / app.js / styles.css / případně json configy)
    if (p.endsWith(".html") || p.endsWith(".js") || p.endsWith(".css") || p.endsWith(".json")) {
        server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        server.sendHeader("Pragma", "no-cache");
        server.sendHeader("Expires", "0");
    } else {
        // Statické assety (obrázky) klidně cachuj – šetří to výkon.
        server.sendHeader("Cache-Control", "public, max-age=86400");
    }

    server.streamFile(f, contentType);
    f.close();
    return true;
}

static bool handleFileUploadWrite() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        String filename = up.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        uploadFile = LittleFS.open(filename, "w");
        if (!uploadFile) return false;
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        if (uploadFile) uploadFile.close();
    }
    return true;
}

// ===== Konfigurace (config.json) =====

static bool loadFileToString(const char* path, String& out) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;

    const size_t sz = (size_t)f.size();
    out = "";
    out.reserve(sz + 1);

    char buf[512];
    while (f.available()) {
        const size_t n = f.readBytes(buf, sizeof(buf));
        if (n == 0) break;
        out.concat(buf, (unsigned int)n);
    }

    f.close();
    return true;
}

static bool isValidJsonObject(const String& json) {
    // Validace konfigurace – musí to být JSON objekt.
    // Pozor: pro jistotu necháváme větší buffer, aby validní config nebyl odmítnut jen kvůli velikosti.
    DynamicJsonDocument doc(32768);
    DeserializationError err = deserializeJson(doc, json);
    return (!err) && doc.is<JsonObject>();
}

static void loadConfigFromFS() {
    const char* cfgPath = "/config.json";
    const char* bakPath = "/config.json.bak";
    bool restoredFromBak = false;

    if (!LittleFS.exists(cfgPath)) {
        g_configJson = "{}";
        Serial.println(F("[CFG] No config.json, using {}"));
        applyAllConfig(g_configJson);
        return;
    }

    String cfg;
    if (!loadFileToString(cfgPath, cfg)) {
        g_configJson = "{}";
        Serial.println(F("[CFG] config.json open failed, using {}"));
        applyAllConfig(g_configJson);
        return;
    }

    cfg.trim();
    if (!cfg.length()) cfg = "{}";

    // Pokud je config poškozený, zkusíme obnovu ze zálohy.
    if (!isValidJsonObject(cfg)) {
        Serial.println(F("[CFG] config.json invalid, trying .bak"));
        if (LittleFS.exists(bakPath)) {
            String bak;
            if (loadFileToString(bakPath, bak)) {
                bak.trim();
                if (bak.length() && isValidJsonObject(bak)) {
                    cfg = bak;
                    restoredFromBak = true;
                    Serial.println(F("[CFG] Restored config from /config.json.bak"));
                } else {
                    Serial.println(F("[CFG] config.json.bak invalid, using {}"));
                    cfg = "{}";
                }
            } else {
                Serial.println(F("[CFG] config.json.bak read failed, using {}"));
                cfg = "{}";
            }
        } else {
            Serial.println(F("[CFG] No config.json.bak, using {}"));
            cfg = "{}";
        }
    }

    g_configJson = cfg;

    // Pokud jsme obnovili z .bak, je lepší zapsat opravený config zpět do /config.json,
    // aby se zařízení po každém restartu neopíralo o .bak.
    // Zároveň necháváme .bak beze změny.
    if (restoredFromBak) {
        const char* tmpPath = "/config.json.restore.tmp";
        File wf = LittleFS.open(tmpPath, "w");
        if (wf) {
            wf.print(g_configJson);
            wf.flush();
            wf.close();

            LittleFS.remove(cfgPath); // může to být poškozené
            if (!LittleFS.rename(tmpPath, cfgPath)) {
                LittleFS.remove(tmpPath);
                Serial.println(F("[CFG] Restore write-back failed (rename)."));
            } else {
                Serial.println(F("[CFG] Restored config written back to /config.json"));
            }
        } else {
            Serial.println(F("[CFG] Restore write-back failed (open tmp)."));
        }
    }

    applyAllConfig(g_configJson);

    Serial.println(F("[CFG] config.json loaded & applied."));
}


// ===== API dash (Dashboard V2) =====
static void handleApiDash() {
    DynamicJsonDocument doc(32768);

    JsonArray temps = doc.createNestedArray("temps");
    JsonArray tempsValid = doc.createNestedArray("tempsValid");
    for (uint8_t i=0;i<8;i++){
        bool v = logicIsTempValid(i);
        tempsValid.add(v);
        float t = logicGetTempC(i);
        if (isfinite(t)) temps.add(t);
        else temps.add(nullptr);
    }
    // --- Dallas diagnostics (GPIO0-3) ---
    JsonArray dallasArr = doc.createNestedArray("dallas");
    for (uint8_t gpio=0; gpio<=3; gpio++){
        const DallasGpioStatus* ds = DallasController::getStatus(gpio);
        JsonObject go = dallasArr.createNestedObject();
        go["gpio"] = gpio;
        if (!ds){ go["status"] = "disabled"; continue; }
        switch(ds->status){
          case TEMP_STATUS_OK: go["status"]="ok"; break;
          case TEMP_STATUS_NO_SENSOR: go["status"]="no_sensor"; break;
          case TEMP_STATUS_ERROR: go["status"]="error"; break;
          case TEMP_STATUS_DISABLED: go["status"]="disabled"; break;
          default: go["status"]="unknown"; break;
        }
        go["lastReadMs"] = ds->lastReadMs;
        JsonArray devs = go.createNestedArray("devices");
        for (const auto &dv : ds->devices){
            JsonObject d = devs.createNestedObject();
            char buf[17];
            snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)dv.rom);
            d["rom"] = buf;
            d["valid"] = dv.valid;
            if (isfinite(dv.temperature)) d["tempC"] = dv.temperature; else d["tempC"] = nullptr;
        }
    }

    // --- BLE temps (např. meteostanice) ---
    {
        JsonArray bleTemps = doc.createNestedArray("bleTemps");
        const BleThermometerCfg& bc = thermometersGetBle();
        float tC = NAN;
        bool ok = bleGetTempCById(bc.id, tC);

        {
            JsonObject o = bleTemps.createNestedObject();
            o["id"] = bc.id;
            o["label"] = bc.name.length() ? bc.name : "BLE";
            o["valid"] = ok && isfinite(tC);
            if (ok && isfinite(tC)) o["tempC"] = tC; else o["tempC"] = nullptr;
        }

        // stručný stav (pro rychlé kontroly)
        JsonObject ble = doc.createNestedObject("ble");
        ble["meteoFix"] = bleHasMeteoFix();
        float mt = NAN;
        if (bleGetMeteoTempC(mt) && isfinite(mt)) ble["meteoTempC"] = mt; else ble["meteoTempC"] = nullptr;
    }

    // --- MQTT teploměry (konfigurace "Teploměry") ---
    {
        JsonArray mqttTemps = doc.createNestedArray("mqttTemps");
        for (uint8_t i = 0; i < 2; i++) {
            const MqttThermometerCfg& mc = thermometersGetMqtt(i);
            JsonObject o = mqttTemps.createNestedObject();
            o["idx"] = i + 1;
            o["name"] = mc.name;
            o["topic"] = mc.topic;
            o["jsonKey"] = mc.jsonKey;

            float tC = NAN;
            bool ok = false;
            uint32_t lastMs = 0;
            String payload;
            bool have = mc.topic.length() && mqttGetLastValueInfo(mc.topic, &payload, &lastMs);
            if (have) ok = tempParseFromPayload(payload, mc.jsonKey, tC);

            o["valid"] = ok && isfinite(tC);
            if (ok && isfinite(tC)) o["tempC"] = tC; else o["tempC"] = nullptr;
            if (have) {
                o["lastMs"] = lastMs;
                o["ageMs"] = (uint32_t)(millis() - lastMs);
            } else {
                o["lastMs"] = nullptr;
                o["ageMs"] = nullptr;
            }
        }
    }

    JsonArray valves = doc.createNestedArray("valves");
    for (uint8_t r=1;r<=8;r++){
        ValveUiStatus vs;
        if (!logicGetValveUiStatus(r, vs)) continue;
        JsonObject o = valves.createNestedObject();
        o["master"] = vs.master;
        o["peer"] = vs.peer;
        o["posPct"] = vs.posPct;
        o["moving"] = vs.moving;
        o["targetB"] = vs.targetB;
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static bool saveConfigToFS() {
    const char* tmpPath = "/config.json.tmp";
    const char* bakPath = "/config.json.bak";

    File f = LittleFS.open(tmpPath, "w");
    if (!f) return false;

    const size_t written = f.print(g_configJson);
    f.flush();
    f.close();

    if (written != g_configJson.length()) {
        LittleFS.remove(tmpPath);
        return false;
    }

    if (LittleFS.exists(bakPath)) LittleFS.remove(bakPath);
    if (LittleFS.exists("/config.json")) {
        if (!LittleFS.rename("/config.json", bakPath)) {
            LittleFS.remove(tmpPath);
            return false;
        }
    }

    if (!LittleFS.rename(tmpPath, "/config.json")) {
        if (LittleFS.exists(bakPath)) LittleFS.rename(bakPath, "/config.json");
        LittleFS.remove(tmpPath);
        return false;
    }

    if (LittleFS.exists(bakPath)) LittleFS.remove(bakPath);
    return true;
}

// ===== Rules (rules.json) =====
#if FEATURE_RULE_ENGINE
// forward declaration (používá se v loadRulesFromFS)
static bool saveRulesToFS();

static void loadRulesFromFS() {
    if (!LittleFS.exists("/rules.json")) {
        g_rulesEnabled = false;
        g_rulesJson = "{\"enabled\":false,\"defaultOffControlled\":true,\"rules\":[]}";
        File f = LittleFS.open("/rules.json", "w");
        if (f) { f.print(g_rulesJson); f.close(); }
        Serial.println(F("[RULES] No rules.json, default created."));
        return;
    }
    File f = LittleFS.open("/rules.json", "r");
    if (!f) {
        g_rulesEnabled = false;
        g_rulesJson = "{\"enabled\":false,\"defaultOffControlled\":true,\"rules\":[]}";
        Serial.println(F("[RULES] rules.json open failed, using default."));
        return;
    }
    g_rulesJson = "";
    while (f.available()) g_rulesJson += (char)f.read();
    f.close();
    if (!g_rulesJson.length()) {
        g_rulesEnabled = false;
        g_rulesJson = "{\"enabled\":false,\"defaultOffControlled\":true,\"rules\":[]}";
        Serial.println(F("[RULES] rules.json empty, using default."));
        return;
    }
    // pokusíme se vytáhnout enabled (podporujeme i legacy formát: JSON array => enabled=true)
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, g_rulesJson) == DeserializationError::Ok) {
        if (doc.is<JsonArray>()) {
            g_rulesEnabled = true;
        } else {
            g_rulesEnabled = doc["enabled"] | false;
        }
    } else {
        g_rulesEnabled = false;
    }
    Serial.println(F("[RULES] rules.json loaded."));

    // Aplikuj do runtime Rule Engine
    String errMsg;
    if (!ruleEngineLoadFromJson(g_rulesJson, &errMsg)) {
        Serial.print(F("[RULES] ruleEngineLoadFromJson failed: "));
        Serial.println(errMsg);
        ruleEngineSetEnabled(false);
        g_rulesEnabled = false;
    } else {
        // sjednocení: stav v RAM odpovídá pravidlům
        // (loadFromJson nastavuje enabled podle JSONu, ale držíme i kopii pro UI)
        g_rulesEnabled = ruleEngineIsEnabled();
        // normalizace exportu (řeší legacy array formát, doplní defaultOffControlled, atd.)
        g_rulesJson = ruleEngineExportJson();
        // volitelně migrace do FS (ať se UI příště vždy načte stejně)
        saveRulesToFS();
    }
}

static bool saveRulesToFS() {
    File f = LittleFS.open("/rules.json", "w");
    if (!f) {
        Serial.println(F("[RULES] Failed to open rules.json for write."));
        return false;
    }
    f.print(g_rulesJson);
    f.close();
    Serial.println(F("[RULES] rules.json saved."));
    return true;
}
#endif

// ===== API status =====

void handleApiStatus() {
    // Stabilní status přes ArduinoJson (bez ručního skládání Stringů)
    DynamicJsonDocument doc(12288);

    // --- mode/control ---
    doc["systemMode"]  = logicModeToString(logicGetMode());
    doc["controlMode"] = (logicGetControlMode() == ControlMode::AUTO) ? "auto" : "manual";

    // --- AUTO diagnostika ---
    AutoStatus as = logicGetAutoStatus();
    JsonObject autoObj = doc.createNestedObject("auto");
    autoObj["triggered"] = as.triggered;
    autoObj["triggerInput"] = as.triggerInput;
    autoObj["triggerMode"] = logicModeToString(as.triggerMode);
    autoObj["usingRelayMap"] = as.usingRelayMap;
    autoObj["blockedByRules"] = as.blockedByRules;
    autoObj["defaultOffUnmapped"] = logicGetAutoDefaultOffUnmapped();

    // --- relays ---
    JsonArray relays = doc.createNestedArray("relays");
    for (int i = 0; i < RELAY_COUNT; i++) {
        relays.add(relayGetState(static_cast<RelayId>(i)) ? 1 : 0);
    }

    // --- inputs (RAW) ---
    JsonArray inputs = doc.createNestedArray("inputs");
    for (int i = 0; i < INPUT_COUNT; i++) {
        inputs.add(inputGetRaw(static_cast<InputId>(i)) ? 1 : 0);
    }

    // --- temperatures (TEMP1..TEMP8) ---
    JsonArray temps = doc.createNestedArray("temps");
    for (int i = 0; i < INPUT_COUNT; i++) {
        if (logicIsTempValid((uint8_t)i)) temps.add(logicGetTempC((uint8_t)i));
        else temps.add(nullptr);
    }

    // --- valves (UI stav pro trojcestné ventily) ---
    JsonArray valves = doc.createNestedArray("valves");
    for (uint8_t r = 1; r <= RELAY_COUNT; r++) {
        ValveUiStatus vs;
        if (!logicGetValveUiStatus(r, vs)) continue;
        JsonObject o = valves.createNestedObject();
        o["master"] = vs.master;
        o["peer"] = vs.peer;
        o["posPct"] = vs.posPct;
        o["moving"] = vs.moving;
        o["targetB"] = vs.targetB;
    }

    // --- uptime ---
    doc["uptimeMs"] = (uint32_t)millis();

    // --- wifi ---
    const bool wifiConn = networkIsConnected();
    JsonObject wifi = doc.createNestedObject("wifi");
    wifi["connected"] = wifiConn;
    wifi["ip"] = networkGetIp();
    if (wifiConn) wifi["rssi"] = WiFi.RSSI();

    // --- MQTT ---
    JsonObject mqtt = doc.createNestedObject("mqtt");
    mqtt["configured"] = mqttIsConfigured();
    mqtt["connected"] = mqttIsConnected();

    // --- time ---
    JsonObject t = doc.createNestedObject("time");
    t["valid"] = networkIsTimeValid();
    t["epoch"] = (uint32_t)networkGetTimeEpoch();
    t["iso"] = networkGetTimeIso();
    t["source"] = networkGetTimeSource();
    t["rtcPresent"] = networkIsRtcPresent();

    // --- TUV status ---
    JsonObject tuv = doc.createNestedObject("tuv");
    TuvStatus ts = logicGetTuvStatus();
    tuv["enabled"] = ts.enabled;
    tuv["scheduleEnabled"] = ts.scheduleEnabled;
    tuv["demandActive"] = ts.demandActive;
    tuv["modeActive"] = ts.modeActive;
    tuv["eqValveMaster"] = ts.eqValveMaster;
    tuv["eqValveTargetPct"] = ts.eqValveTargetPct;
    tuv["valveMaster"] = ts.valveMaster;
    tuv["valveTargetPct"] = ts.valveTargetPct;
    tuv["nightMode"] = logicGetNightMode();

    // --- equitherm ---
    {
        EquithermStatus es = logicGetEquithermStatus();
        JsonObject eq = doc.createNestedObject("equitherm");
        eq["enabled"] = es.enabled;
        eq["active"]  = es.active;
        eq["night"]   = es.night;
        eq["reason"]  = logicGetEquithermReason();

        if (isfinite(es.outdoorC)) eq["outdoorC"] = es.outdoorC; else eq["outdoorC"] = nullptr;
        if (isfinite(es.flowC))    eq["flowC"]    = es.flowC;    else eq["flowC"]    = nullptr;
        if (isfinite(es.targetFlowC)) eq["targetFlowC"] = es.targetFlowC; else eq["targetFlowC"] = nullptr;

        eq["valveMaster"]   = es.valveMaster;     // 0 = none
        eq["valvePosPct"]   = es.valvePosPct;
        eq["valveTargetPct"]= es.valveTargetPct;
        eq["valveMoving"]   = es.valveMoving;
        eq["lastAdjustMs"]  = es.lastAdjustMs;
    }

    // --- opentherm ---
    {
        JsonObject ot = doc.createNestedObject("opentherm");
        openthermFillJson(ot);
    }

    // UI kompatibilita: top-level alias
    doc["ruleEngineEnabled"] = ruleEngineIsEnabled();
    #if FEATURE_RULE_ENGINE

    // Rule Engine status (jako objekt)
    {
        DynamicJsonDocument rulesDoc(3072);
        DeserializationError err = deserializeJson(rulesDoc, ruleEngineGetStatusJson());
        if (!err) {
            JsonObject ro = doc.createNestedObject("rules");
            ro.set(rulesDoc.as<JsonObject>());
        } else {
            JsonObject ro = doc.createNestedObject("rules");
            ro["error"] = err.c_str();
        }
    }
    #endif

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// ===== API relé =====

void handleApiRelay() {
    if (!server.hasArg("id") || !server.hasArg("cmd")) {
        server.send(400, "application/json", "{\"error\":\"missing id or cmd\"}");
        return;
    }

    int id = server.arg("id").toInt();
    String cmd = server.arg("cmd");
    cmd.toLowerCase();

    if (id < 1 || id > RELAY_COUNT) {
        server.send(400, "application/json", "{\"error\":\"invalid id\"}");
        return;
    }

    // Bezpečné chování: ruční zásah => MANUAL
    if (logicGetControlMode() == ControlMode::AUTO) {
        logicSetControlMode(ControlMode::MANUAL);
    }

    if (cmd == "on") {
        relaySet(static_cast<RelayId>(id - 1), true);
    } else if (cmd == "off") {
        relaySet(static_cast<RelayId>(id - 1), false);
    } else if (cmd == "toggle") {
        relayToggle(static_cast<RelayId>(id - 1));
    } else {
        server.send(400, "application/json", "{\"error\":\"invalid cmd\"}");
        return;
    }

    String json = "{\"status\":\"ok\",\"id\":";
    json += String(id);
    json += ",\"state\":";
    json += relayGetState(static_cast<RelayId>(id - 1)) ? "true" : "false";
    json += "}";
    server.send(200, "application/json", json);
}

// ===== API config =====

void handleApiConfigGet() {
    server.send(200, "application/json", g_configJson);
}

void handleApiConfigPost() {
    String body = server.arg("plain");
    body.trim();
    if (!body.length()) {
        server.send(400, "application/json", "{\"error\":\"empty body\"}");
        return;
    }

    int first = 0;
    while (first < (int)body.length() && isspace(static_cast<unsigned char>(body[first]))) first++;
    if (first >= (int)body.length() || body[first] != '{') {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    StaticJsonDocument<512> filter;
    filter["iofunc"] = true;
    filter["equitherm"] = true;
    filter["tuv"] = true;
    filter["schedules"] = true;
    filter["mqtt"] = true;
    filter["time"] = true;
    filter["thermometers"] = true;
    filter["opentherm"] = true;
    filter["relayNames"] = true;
    filter["inputNames"] = true;
    filter["inputActiveLevels"] = true;
    filter["inputs"] = true;
    filter["relayMap"] = true;
    filter["modes"] = true;
    filter["autoDefaultOffUnmapped"] = true;
    filter["auto_default_off_unmapped"] = true;

    DynamicJsonDocument doc(32768);
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }
    if (!doc.is<JsonObject>()) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    String previous = g_configJson;
    g_configJson = body;
    if (!saveConfigToFS()) {
        g_configJson = previous;
        server.send(500, "application/json", "{\"error\":\"save failed\"}");
        return;
    }

    applyAllConfig(g_configJson);

    server.send(200, "application/json", "{\"status\":\"ok\"}");
}


// ===== API BLE =====

void handleApiBleStatus() {
    server.send(200, "application/json", bleGetStatusJson());
}

void handleApiOpenThermStatus() {
    DynamicJsonDocument doc(512);
    JsonObject obj = doc.to<JsonObject>();
    openthermFillJson(obj);
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void handleApiBleConfigGet() {
    server.send(200, "application/json", bleGetConfigJson());
}

void handleApiBleConfigPost() {
    String body = server.arg("plain");
    body.trim();
    if (!body.length()) {
        server.send(400, "application/json", "{\"error\":\"empty body\"}");
        return;
    }
    if (!bleSetConfigJson(body)) {
        server.send(500, "application/json", "{\"error\":\"save/apply failed\"}");
        return;
    }
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleApiBlePairedGet() {
    server.send(200, "application/json", bleGetPairedJson());
}

void handleApiBlePairPost() {
    String body = server.arg("plain");
    body.trim();

    DynamicJsonDocument doc(512);
    deserializeJson(doc, body);

    uint32_t seconds = doc["seconds"] | 120;
    const char* role = doc["role"] | "";

    if (!bleStartPairing(seconds, String(role))) {
        server.send(500, "application/json", "{\"error\":\"pairing start failed\"}");
        return;
    }
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleApiBleStopPairPost() {
    bleStopPairing();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleApiBleRemovePost() {
    String body = server.arg("plain");
    body.trim();
    DynamicJsonDocument doc(512);
    deserializeJson(doc, body);
    const char* mac = doc["mac"] | "";
    if (!strlen(mac)) {
        server.send(400, "application/json", "{\"error\":\"missing mac\"}");
        return;
    }
    if (!bleRemoveDevice(String(mac))) {
        server.send(500, "application/json", "{\"error\":\"remove failed\"}");
        return;
    }
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// ===== API mode_ctrl =====

void handleApiModeCtrlGet() {
    String json = "{";
    json += "\"controlMode\":\"";
    json += (logicGetControlMode() == ControlMode::AUTO) ? "auto" : "manual";
    json += "\",\"systemMode\":\"";
    json += logicModeToString(logicGetMode());
    json += "\"}";
    server.send(200, "application/json", json);
}

void handleApiModeCtrlPost() {
    // 1) JSON body (nové UI)
    String body = server.arg("plain");
    body.trim();
    if (body.length() && body[0] == '{') {
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, body);
        if (err) {
            server.send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }

        String action = (const char*)(doc["action"] | "");
        action.toLowerCase();

        if (action == "control_mode") {
            String v = (const char*)(doc["value"] | "");
            v.toLowerCase();
            if (v == "auto") logicSetControlMode(ControlMode::AUTO);
            if (v == "manual") logicSetControlMode(ControlMode::MANUAL);
            handleApiModeCtrlGet();
            return;
        }

        if (action == "manual_mode") {
            String v = (const char*)(doc["value"] | "");
            if (!logicSetManualModeByName(v)) {
                server.send(400, "application/json", "{\"error\":\"invalid mode\"}");
                return;
            }
            logicSetControlMode(ControlMode::MANUAL);
            handleApiModeCtrlGet();
            return;
        }

        if (action == "relay") {
            int rid = doc["relay"] | 0; // 1..8
            bool val = doc["value"] | false;
            if (rid < 1 || rid > RELAY_COUNT) {
                server.send(400, "application/json", "{\"error\":\"relay must be 1-8\"}");
                return;
            }

            // Bezpečné chování: ruční zásah => MANUAL
            if (logicGetControlMode() == ControlMode::AUTO) {
                logicSetControlMode(ControlMode::MANUAL);
            }
            logicSetRelayOutput((uint8_t)rid, val);
            String json = "{\"status\":\"ok\",\"relay\":";
            json += String(rid);
            json += ",\"value\":";
            json += val ? "true" : "false";
            json += "}";
            server.send(200, "application/json", json);
            return;
        }

        if (action == "relay_raw") {
            const uint8_t relay = (uint8_t)(doc["relay"] | 0); // 1..8
            const bool on = (bool)(doc["on"] | false);
            if (relay < 1 || relay > RELAY_COUNT) {
                server.send(400, "application/json", "{\"error\":\"relay must be 1-8\"}");
                return;
            }
            logicSetRelayRaw(relay, on);
            server.send(200, "application/json", "{\"ok\":true}");
            return;
        }

        if (action == "mqtt_discovery") {
            mqttRepublishDiscovery();
            server.send(200, "application/json", "{\"status\":\"ok\"}");
            return;
        }

        if (action == "auto_recompute") {
            logicRecomputeFromInputs();
            handleApiModeCtrlGet();
            return;
        }

        server.send(400, "application/json", "{\"error\":\"unknown action\"}");
        return;
    }

    // 2) Legacy form-data kompatibilita
    if (server.hasArg("control")) {
        String control = server.arg("control");
        control.toLowerCase();
        if (control == "auto") {
            logicSetControlMode(ControlMode::AUTO);
        } else if (control == "manual") {
            logicSetControlMode(ControlMode::MANUAL);
        }
    }

    if (server.hasArg("mode")) {
        String mode = server.arg("mode");
        if (!logicSetManualModeByName(mode)) {
            server.send(400, "application/json", "{\"error\":\"invalid mode\"}");
            return;
        }
        logicSetControlMode(ControlMode::MANUAL);
    }

    handleApiModeCtrlGet();
}

// ===== RULES API =====
#if FEATURE_RULE_ENGINE
void handleApiRulesGet() {
    // vždy vrať to, co drží runtime engine (nejspolehlivější zdroj)
    // fallback: lokální kopie / default
    String cur = ruleEngineExportJson();
    if (cur.length()) {
        g_rulesJson = cur;
        g_rulesEnabled = ruleEngineIsEnabled();
    } else if (!g_rulesJson.length()) {
        g_rulesEnabled = false;
        g_rulesJson = "{\"enabled\":false,\"rules\":[]}";
    }
    server.send(200, "application/json", g_rulesJson);
}

void handleApiRulesPost() {
    String body = server.arg("plain");
    body.trim();
    if (!body.length()) {
        server.send(400, "application/json", "{\"error\":\"empty body\"}");
        return;
    }

    // validace, že je to JSON + načtení do runtime (tím zároveň ověříme formát pravidel)
    String errMsg;
    if (!ruleEngineLoadFromJson(body, &errMsg)) {
        String out = String("{\"error\":\"rule parse failed\",\"detail\":\"") + jsonEscape(errMsg) + "\"}";
        server.send(400, "application/json", out);
        return;
    }

    // Když UI pošle enabled=false, engine se vypne (loadFromJson nastavuje enabled podle JSONu)
    // ale pro jistotu držíme konzistentní kopii
    g_rulesEnabled = ruleEngineIsEnabled();
    g_rulesJson = ruleEngineExportJson();

    if (!saveRulesToFS()) {
        server.send(500, "application/json", "{\"error\":\"save failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleApiRulesStatus() {
    server.send(200, "application/json", ruleEngineGetStatusJson());
}
#endif
// ===== REBOOT API =====

void handleApiRebootPost() {
    String body = server.arg("plain");
    body.trim();
    bool doReboot = false;
    if (body.length() && body[0] == '{') {
        StaticJsonDocument<128> doc;
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            doReboot = doc["reboot"] | false;
        }
    }
    if (!doReboot) {
        server.send(400, "application/json", "{\"error\":\"reboot=false\"}");
        return;
    }
    server.send(200, "application/json", "{\"status\":\"ok\",\"msg\":\"Rebooting...\",\"whenMs\":250}");
    g_rebootPending = true;
    g_rebootAtMs = millis() + 250;
}

// ===== File manager =====

void handleApiFsList() {
    if (!fsIsReady()) {
        server.send(500, "application/json", "{\"error\":\"fs not mounted\"}");
        return;
    }
    File root = LittleFS.open("/");
    if (!root) {
        server.send(500, "application/json", "{\"error\":\"fs open failed\"}");
        return;
    }

    // Pozn.: ArduinoJson minimalizuje fragmentaci heapu oproti ručnímu lepení Stringů
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.to<JsonArray>();

    File file = root.openNextFile();
    while (file) {
        JsonObject o = arr.createNestedObject();
        o["name"] = file.name();
        o["size"] = (uint32_t)file.size();
        file = root.openNextFile();
        // pokud je FS větší a dojdeme na limit, raději vrať částečný výpis než spadnout
        if (doc.memoryUsage() > 1900) break;
    }

    String out;
    serializeJson(arr, out);
    server.send(200, "application/json", out);
}

void handleApiFsDelete() {
    if (!server.hasArg("path")) {
        server.send(400, "application/json", "{\"error\":\"missing path\"}");
        return;
    }
    String path = server.arg("path");
    if (!path.startsWith("/")) path = "/" + path;

    if (!LittleFS.exists(path)) {
        server.send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    if (!LittleFS.remove(path)) {
        server.send(500, "application/json", "{\"error\":\"remove failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleApiFsUpload() {
    if (!handleFileUploadWrite()) {
        server.send(500, "application/json", "{\"error\":\"upload failed\"}");
    }
}

void handleApiFsUploadDone() {
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// ===== API buzzer =====
static void handleApiBuzzerGet() {
    String json;
    buzzerToJson(json);
    server.send(200, "application/json", json);
}

static void handleApiBuzzerPost() {
    String body = server.arg("plain");
    body.trim();
    if (!body.length() || body[0] != '{') {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    StaticJsonDocument<768> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    String action = (const char*)(doc["action"] | "");
    action.toLowerCase();

    if (action == "test") {
        String pattern = (const char*)(doc["pattern"] | "short");
        buzzerPlayPatternByName(pattern);
        handleApiBuzzerGet();
        return;
    }
    if (action == "stop") {
        buzzerStop();
        handleApiBuzzerGet();
        return;
    }
    if (action == "set_config") {
        // očekává {"action":"set_config","config":{...}}
        JsonObject cfg = doc["config"].as<JsonObject>();
        if (cfg.isNull()) {
            server.send(400, "application/json", "{\"error\":\"missing config\"}");
            return;
        }
        buzzerUpdateFromJson(cfg);
        buzzerSaveToFS();
        handleApiBuzzerGet();
        return;
    }

    server.send(400, "application/json", "{\"error\":\"unknown action\"}");
}



// ===== API time/caps =====

static void handleApiTime() {
    StaticJsonDocument<512> doc;
    doc["valid"] = networkIsTimeValid();
    doc["epoch"] = (uint32_t)networkGetTimeEpoch();
    doc["iso"] = networkGetTimeIso();
    doc["source"] = networkGetTimeSource();
    doc["rtcPresent"] = networkIsRtcPresent();
    doc["ip"] = networkGetIp();

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleApiCaps() {
    StaticJsonDocument<512> doc;
    // UI can hide/disable features based on these flags
    doc["time"] = true;
    doc["rtc"] = networkIsRtcPresent();
    doc["schedules"] = true;
    doc["iofunc"] = true;
    #if FEATURE_RULE_ENGINE
        doc["ruleEngine"] = true;
    #else
        doc["ruleEngine"] = false;
    #endif
    doc["mqtt"] = true;
    doc["ble"] = true;
    doc["opentherm"] = true;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleApiTemps() {
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.createNestedArray("ntc");

  for (uint8_t i = 0; i < 3; i++) {
    JsonObject o = arr.createNestedObject();
    o["idx"] = i + 1;
    o["en"] = ntcIsEnabled(i);
    o["gpio"] = ntcGetGpio(i);
    o["valid"] = ntcIsValid(i);
    o["raw"] = ntcGetRaw(i);
    if (ntcIsValid(i)) o["c"] = ntcGetTempC(i);
    else o["c"] = nullptr;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ===== Inicializace webserveru =====
void webserverInit() {
    // FS je mountnutý v setup() přes fsInit(); tady jen načti konfiguraci, pokud je dostupná
    if (fsIsReady()) {
        loadConfigFromFS();
        #if FEATURE_RULE_ENGINE
            loadRulesFromFS();
        #endif
    }

    server.on("/", HTTP_GET, []() {
        if (!handleFileRead("/index.html")) {
            server.send(404, "text/plain", "index.html not found");
        }
    });

    // BUZZER API
    server.on("/api/buzzer", HTTP_GET, []() {
        handleApiBuzzerGet();
    });
    server.on("/api/buzzer", HTTP_POST, []() {
        handleApiBuzzerPost();
    });

    // favicon explicitně (ať to nechodí přes onNotFound a je to čitelnější v logu)
    server.on("/favicon.ico", HTTP_GET, []() {
        if (!handleFileRead("/favicon.ico")) {
            server.send(404, "text/plain", "favicon.ico not found");
        }
    });

    server.onNotFound([]() {
        if (!handleFileRead(server.uri())) {
            server.send(404, "text/plain", "Not found");
        }
    });

    // API
    server.on("/api/dash", HTTP_GET, handleApiDash);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/time", HTTP_GET, handleApiTime);
    server.on("/api/caps", HTTP_GET, handleApiCaps);

    server.on("/api/relay", HTTP_GET, handleApiRelay);

    server.on("/api/config", HTTP_GET, handleApiConfigGet);
    server.on("/api/config", HTTP_POST, handleApiConfigPost);

    // BLE
    server.on("/api/ble/status", HTTP_GET, handleApiBleStatus);

    // OpenTherm (boiler)
    server.on("/api/opentherm/status", HTTP_GET, handleApiOpenThermStatus);
    server.on("/api/ble/config", HTTP_GET, handleApiBleConfigGet);
    server.on("/api/ble/config", HTTP_POST, handleApiBleConfigPost);
    server.on("/api/ble/paired", HTTP_GET, handleApiBlePairedGet);
    server.on("/api/ble/pair", HTTP_POST, handleApiBlePairPost);
    server.on("/api/ble/pair/stop", HTTP_POST, handleApiBleStopPairPost);
    server.on("/api/ble/remove", HTTP_POST, handleApiBleRemovePost);

    server.on("/api/mode_ctrl", HTTP_GET, handleApiModeCtrlGet);
    server.on("/api/mode_ctrl", HTTP_POST, handleApiModeCtrlPost);

    // Rules
    #if FEATURE_RULE_ENGINE
    server.on("/api/rules", HTTP_GET, handleApiRulesGet);
    server.on("/api/rules", HTTP_POST, handleApiRulesPost);
    server.on("/api/rules/status", HTTP_GET, handleApiRulesStatus);
    #endif

    // Reboot
    server.on("/api/reboot", HTTP_POST, handleApiRebootPost);

    server.on("/api/temps", HTTP_GET, handleApiTemps);

    // FS manager
    server.on("/api/fs/list", HTTP_GET, handleApiFsList);
    server.on("/api/fs/delete", HTTP_POST, handleApiFsDelete);
    server.on("/api/fs/upload", HTTP_POST, handleApiFsUpload, handleApiFsUploadDone);

    server.begin();
    Serial.println(F("[WEB] WebServer started."));
}

void webserverLoop() {
    server.handleClient();

    if (g_rebootPending) {
        const uint32_t now = millis();
        if ((int32_t)(now - g_rebootAtMs) >= 0) {
            Serial.println(F("[SYS] Rebooting now..."));
            ESP.restart();
        }
    }
}
