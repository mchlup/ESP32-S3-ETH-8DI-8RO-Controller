#include "WebServerController.h"

#include <Arduino.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include "FsController.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
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
#include "DallasController.h"

static WebServer server(80);
static File uploadFile;

// Ne blokující restart (naplánovaný)
static bool     g_rebootPending = false;
static uint32_t g_rebootAtMs    = 0;

// Globální JSON konfigurace (popisy + mapování relé + režimy)
static String g_configJson;
static bool g_configLoaded = false;
static DynamicJsonDocument g_dashDoc(32768);
// /api/status narostlo (equitherm + TUV + recirc + opentherm + valves). 12k bývá na hraně a
// vede k neúplným odpovědím (ArduinoJson při nedostatku kapacity tiše "ořeže" vnořené části).
static DynamicJsonDocument g_statusDoc(24576);


// --- Validation helpers: reserve relays used by 3-way valves so they cannot be used elsewhere ---
static bool isValve3wayRole(const char* role) {
    if (!role) return false;
    return strncmp(role, "valve_3way_", 10) == 0;
}

static bool valveRoleNeedsPeer(const char* role) {
    if (!role) return false;
    return strcmp(role, "valve_3way_mix") == 0 ||
           strcmp(role, "valve_3way_2rel") == 0 ||
           strcmp(role, "valve_3way_spring") == 0;
}

static void collectReservedRelaysFromValves(JsonDocument& doc, bool reserved[9]) {
    for (int i = 0; i < 9; i++) reserved[i] = false;

    JsonArray outputs = doc["iofunc"]["outputs"].as<JsonArray>();
    if (outputs.isNull()) return;

    int idx = 0;
    for (JsonVariant vv : outputs) {
        JsonObject o = vv.as<JsonObject>();
        const char* role = o["role"] | "";
        const int master = idx + 1; // 1..8
        if (master >= 1 && master <= 8 && isValve3wayRole(role)) {
            reserved[master] = true;
            if (valveRoleNeedsPeer(role)) {
                JsonObject params = o["params"].as<JsonObject>();
                int peer = 0;
                if (!params.isNull()) {
                    peer = (int)(params["peerRel"] | params["partnerRelay"] | 0);
                }
                if (peer < 1 || peer > 8) {
                    peer = (master < 8) ? (master + 1) : 0;
                }
                if (peer >= 1 && peer <= 8) reserved[peer] = true;
            }
        }
        idx++;
    }
}

static bool isReservedRelayNum(int relayNum, const bool reserved[9]) {
    if (relayNum < 1 || relayNum > 8) return false;
    return reserved[relayNum];
}

static bool validateNoRelayConflictsWithValves(JsonDocument& doc, String& errMsg, int& errRelay) {
    bool reserved[9];
    collectReservedRelaysFromValves(doc, reserved);

    // 1) relayMap: mapping for reserved relays must be disabled (input=0)
    JsonArray relayMap = doc["relayMap"].as<JsonArray>();
    if (!relayMap.isNull()) {
        int idx = 0;
        for (JsonVariant vv : relayMap) {
            const int relayNum = idx + 1;
            JsonObject m = vv.as<JsonObject>();
            int input = m.isNull() ? 0 : (int)(m["input"] | 0);
            if (isReservedRelayNum(relayNum, reserved) && input > 0) {
                errRelay = relayNum;
                errMsg = "Relé R" + String(relayNum) + " je použito pro 3c ventil a nesmí být mapováno jako běžné relé (relayMap).";
                return false;
            }
            idx++;
        }
    }


    // 1b) modes: reserved relays must not be forced by manual modes
    JsonArray modes = doc["modes"].as<JsonArray>();
    if (!modes.isNull()) {
        for (JsonVariant mv : modes) {
            JsonObject mode = mv.as<JsonObject>();
            if (mode.isNull()) continue;
            JsonArray rs = mode["relayStates"].as<JsonArray>();
            if (rs.isNull()) rs = mode["relay_states"].as<JsonArray>();
            if (rs.isNull()) continue;

            int ridx = 0;
            for (JsonVariant sv : rs) {
                const int relayNum = ridx + 1;
                const int v = (int)(sv | 0);
                if (isReservedRelayNum(relayNum, reserved) && v != 0) {
                    errRelay = relayNum;
                    errMsg = "Relé R" + String(relayNum) + " je použito pro 3c ventil a nesmí být spínáno přes režimy (modes.relayStates).";
                    return false;
                }
                ridx++;
            }
        }
    }

    // 2) Explicit relays used by other features
    struct PathRelay { const char* a; const char* b; } paths[] = {
        {"tuv", "relay"},
        {"tuv", "requestRelay"},
        {"tuv", "request_relay"},
        {"dhwRecirc", "pumpRelay"},
        {"boiler", "dhwRequestRelay"},
        {"boiler", "dhw_request_relay"},
        {"boiler", "nightModeRelay"},
        {"boiler", "night_mode_relay"},
        {"akuHeater", "relay"},
        {"akuHeater", "relayIndex"},
    };

    for (auto &pr : paths) {
        JsonObject o = doc[pr.a].as<JsonObject>();
        if (o.isNull()) continue;
        int r = (int)(o[pr.b] | 0);
        if (isReservedRelayNum(r, reserved)) {
            errRelay = r;
            errMsg = "Relé R" + String(r) + " je použito pro 3c ventil a nelze ho použít v konfiguraci (" + String(pr.a) + "." + String(pr.b) + ").";
            return false;
        }
    }

    errRelay = 0;
    errMsg = "";
    return true;
}

// Krátký cache pro /api/status (UI občas pošle více requestů těsně po sobě).
static uint32_t g_statusCacheAtMs = 0;
static bool     g_statusCacheValid = false;

static void applyAllConfig(const String& json){
    networkApplyConfig(json);
    dallasApplyConfig(json);
    thermometersApplyConfig(json);
    openthermApplyConfig(json);
    // Logika může doplnit/změnit seznam MQTT topiců (např. ekviterm + MQTT teploměry).
    // Proto aplikuj logiku dříve a MQTT až poté (aby subscribe odpovídal nové konfiguraci).
    logicApplyConfig(json);
    mqttApplyConfig(json);
}

// ===== Pomocné: streamované JSON odpovědi bez velkých String alokací =====
static void sendJsonStreamed200(const JsonDocument& doc) {
    WiFiClient client = server.client();

    // Fallback (nemělo by nastat, ale radši bezpečně)
    if (!client) {
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
        return;
    }

    // Spočti délku bez alokace Stringu
    const size_t len = measureJson(doc);

    // Ručně odešli HTTP hlavičky + Content-Length, pak JSON přímo do socketu
    client.print(F("HTTP/1.1 200 OK\r\n"));
    client.print(F("Content-Type: application/json\r\n"));
    client.print(F("Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"));
    client.print(F("Pragma: no-cache\r\n"));
    client.print(F("Expires: 0\r\n"));
    client.print(F("Connection: close\r\n"));
    client.print(F("Content-Length: "));
    client.print(len);
    client.print(F("\r\n\r\n"));

    serializeJson(doc, client);
}

// ===== Pomocné funkce pro FS =====

static void sendWebUiFallback() {
    const String html =
        "<!doctype html><html lang=\"cs\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>WebUI není ve FS</title>"
        "<style>"
        "body{font-family:Arial,Helvetica,sans-serif;background:#0f172a;color:#e2e8f0;padding:24px;}"
        "a{color:#38bdf8;text-decoration:none}a:hover{text-decoration:underline}"
        ".card{max-width:720px;margin:0 auto;background:#111827;border:1px solid #1f2937;"
        "border-radius:12px;padding:20px}"
        ".muted{color:#94a3b8}"
        "</style></head><body><div class=\"card\">"
        "<h1>WebUI není ve FS</h1>"
        "<p class=\"muted\">Nahraj LittleFS image s UI soubory (např. index.html a /js/app.js).</p>"
        "<p><a href=\"/api/fs/list\">/api/fs/list</a> &middot; <a href=\"/api/status\">/api/status</a></p>"
        "</div></body></html>";
    server.send(200, "text/html", html);
}

static bool handleFileRead(const String& path) {
    if (!fsIsReady()) return false;
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

    // FS zámek drž jen krátce (open/exists). Streamování může trvat dlouho a blokovalo by
    // paralelní FS operace (zápis konfigurace, upload atd.).
    fsLock();
    if (!LittleFS.exists(p)) {
        fsUnlock();
        return false;
    }

    File f = LittleFS.open(p, "r");
    fsUnlock();
    if (!f) {
        return false;
    }

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
    fsLock();
    f.close();
    fsUnlock();
    return true;
}

static bool handleFileUploadWrite() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        String filename = up.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        fsLock();
        uploadFile = LittleFS.open(filename, "w");
        fsUnlock();
        if (!uploadFile) return false;
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            fsLock();
            uploadFile.write(up.buf, up.currentSize);
            fsUnlock();
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            fsLock();
            uploadFile.close();
            fsUnlock();
        }
    }
    return true;
}

// ===== Konfigurace (config.json) =====

static bool loadFileToString(const char* path, String& out) {
    fsLock();
    File f = LittleFS.open(path, "r");
    if (!f) {
        fsUnlock();
        return false;
    }

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
    fsUnlock();
    return true;
}

static size_t configJsonCapacityForSize(size_t jsonSize) {
    const size_t minCap = 4096;
    const size_t maxCap = 32768;
    size_t cap = (jsonSize * 13) / 10 + 1024;
    if (cap < minCap) cap = minCap;
    if (cap > maxCap) cap = maxCap;
    return cap;
}

static bool isValidJsonObject(const String& json) {
    // Validace konfigurace – musí to být JSON objekt.
    // Pozor: pro jistotu necháváme větší buffer, aby validní config nebyl odmítnut jen kvůli velikosti.
    DynamicJsonDocument doc(configJsonCapacityForSize(json.length()));
    DeserializationError err = deserializeJson(doc, json);
    return (!err) && doc.is<JsonObject>();
}

static void loadConfigFromFS() {
    const char* cfgPath = "/config.json";
    const char* bakPath = "/config.json.bak";
    bool restoredFromBak = false;

    fsLock();
    const bool hasCfg = LittleFS.exists(cfgPath);
    fsUnlock();
    if (!hasCfg) {
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
        fsLock();
        const bool hasBak = LittleFS.exists(bakPath);
        fsUnlock();
        if (hasBak) {
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
        fsLock();
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
        fsUnlock();
    }

    applyAllConfig(g_configJson);

    Serial.println(F("[CFG] config.json loaded & applied."));
    g_configLoaded = true;
}


// ===== API dash (Dashboard V2) =====
static void handleApiDash() {
    DynamicJsonDocument& doc = g_dashDoc;
    doc.clear();

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
        o["targetPct"] = vs.targetPct;
        o["targetB"] = vs.targetB;
    }

    // ✅ Stream JSON bez String out
    sendJsonStreamed200(doc);
}

static bool saveConfigToFS() {
    const char* bakPath = "/config.json.bak";
    return fsWriteAtomicKeepBak("/config.json", g_configJson, bakPath, true);
}

// ===== API status =====

void handleApiStatus() {
    // Jednoduchý krátký cache: pokud je voláno příliš často, pošli poslední
    // již sestavený JSON (bez znovu-skládání doc). Tím snížíš CPU/latenci při
    // špičkách z WebUI.
    const uint32_t nowMs = (uint32_t)millis();
    if (g_statusCacheValid && (uint32_t)(nowMs - g_statusCacheAtMs) < 250) {
        sendJsonStreamed200(g_statusDoc);
        return;
    }

    // Stabilní status přes ArduinoJson (bez ručního skládání Stringů)
    DynamicJsonDocument& doc = g_statusDoc;
    doc.clear();

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
    // IMPORTANT:
    // Web UI polls /api/status frequently (2s). Some sensor backends (notably DS18B20)
    // can occasionally report "invalid" for a single cycle (bus timing/noise).
    // If we return `null` when invalid, the UI briefly "drops" values.
    //
    // Therefore we always return the *last known* temperature (if available) and
    // add a per-entry `valid` flag. UI already supports object entries.
    JsonArray temps = doc.createNestedArray("temps");
    for (int i = 0; i < INPUT_COUNT; i++) {
        JsonObject o = temps.createNestedObject();
        const bool valid = logicIsTempValid((uint8_t)i);
        const float tC = logicGetTempC((uint8_t)i);
        o["valid"] = valid;
        if (isfinite(tC)) o["tempC"] = tC;
        else o["tempC"] = nullptr;
    }

    // --- valves (legacy list for UI) ---
    JsonArray valvesList = doc.createNestedArray("valvesList");
    for (uint8_t r = 1; r <= RELAY_COUNT; r++) {
        ValveUiStatus vs;
        if (!logicGetValveUiStatus(r, vs)) continue;
        JsonObject o = valvesList.createNestedObject();
        o["master"] = vs.master;
        o["peer"] = vs.peer;
        o["posPct"] = vs.posPct;
        o["moving"] = vs.moving;
        o["targetB"] = vs.targetB;
    }

    // --- uptime ---
    doc["uptimeMs"] = nowMs;

    // --- heap diagnostics (stability) ---
    {
        JsonObject heap = doc.createNestedObject("heap");
        heap["free"] = (uint32_t)ESP.getFreeHeap();
        heap["minFree"] = (uint32_t)ESP.getMinFreeHeap();
        heap["largest"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    }

    // --- error counters / diagnostics (stability) ---
    {
        JsonObject diag = doc.createNestedObject("diag");
        JsonObject i2c = diag.createNestedObject("i2c");
        i2c["relayErrors"] = relayGetI2cErrorCount();
        i2c["relayRecoveries"] = relayGetI2cRecoveryCount();
        i2c["relayLastErrorMs"] = relayGetI2cLastErrorMs();
        const char* lastI2c = relayGetI2cLastError();
        if (lastI2c && lastI2c[0]) i2c["relayLastError"] = lastI2c; else i2c["relayLastError"] = nullptr;

        JsonObject cfg = diag.createNestedObject("configApply");
        cfg["ok"] = logicGetConfigApplyOkCount();
        cfg["fail"] = logicGetConfigApplyFailCount();
        cfg["noMemory"] = logicGetConfigApplyNoMemoryCount();
        cfg["filterOverflow"] = logicGetConfigApplyFilterOverflowCount();
        cfg["oversize"] = logicGetConfigApplyOversizeCount();
        cfg["lastFailMs"] = logicGetConfigApplyLastFailMs();
        cfg["lastInputLen"] = logicGetConfigApplyLastInputLen();
        const char* lastCfg = logicGetConfigApplyLastError();
        if (lastCfg && lastCfg[0]) cfg["lastError"] = lastCfg; else cfg["lastError"] = nullptr;
    }

    // --- wifi ---
    const bool netConn = networkIsConnected();
    const bool wifiConn = networkIsWifiConnected();
    const bool ethConn  = networkIsEthernetConnected();
    JsonObject wifi = doc.createNestedObject("wifi");
    wifi["connected"] = netConn;
    wifi["ip"] = networkGetIp();
    wifi["link"] = wifiConn ? "wifi" : (ethConn ? "eth" : "down");
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
    tuv["active"] = ts.active;
    tuv["scheduleEnabled"] = ts.scheduleEnabled;
    tuv["demandActive"] = ts.demandActive;
    tuv["modeActive"] = ts.modeActive;
    tuv["reason"] = ts.reason;
    tuv["source"] = ts.source;
    tuv["boilerRelayOn"] = ts.boilerRelayOn;
    tuv["eqValveMaster"] = ts.eqValveMaster;
    tuv["eqValveTargetPct"] = ts.eqValveTargetPct;
    tuv["eqValveSavedPct"] = ts.eqValveSavedPct;
    tuv["eqValveSavedValid"] = ts.eqValveSavedValid;
    tuv["valveMaster"] = ts.valveMaster;
    tuv["valveTargetPct"] = ts.valveTargetPct;
    tuv["valvePosPct"] = ts.valvePosPct;
    tuv["valveMoving"] = ts.valveMoving;
    tuv["valveMode"] = ts.valveMode;
    tuv["bypassPct"] = ts.bypassPct;
    tuv["chPct"] = ts.chPct;
    tuv["bypassInvert"] = ts.bypassInvert;
    tuv["nightMode"] = logicGetNightMode();

    // --- Heat call ---
    {
        JsonObject hc = doc.createNestedObject("heatCall");
        const bool raw = logicGetHeatCallActive();
        hc["raw"] = raw;
        hc["day"] = raw;
        hc["night"] = !raw;
    }

    // --- equitherm ---
    {
        EquithermStatus es = logicGetEquithermStatus();
        JsonObject eq = doc.createNestedObject("equitherm");
        eq["enabled"] = es.enabled;
        eq["active"]  = es.active;
        eq["night"]   = es.night;
        eq["reason"]  = logicGetEquithermReason();

        if (isfinite(es.outdoorC)) eq["outdoorC"] = es.outdoorC; else eq["outdoorC"] = nullptr;
        eq["outdoorValid"] = es.outdoorValid;
        eq["outdoorAgeMs"] = es.outdoorAgeMs;
        if (es.outdoorReason.length()) eq["outdoorReason"] = es.outdoorReason; else eq["outdoorReason"] = nullptr;
        if (isfinite(es.flowC))    eq["flowC"]    = es.flowC;    else eq["flowC"]    = nullptr;
        if (isfinite(es.boilerInC)) eq["boilerInC"] = es.boilerInC; else eq["boilerInC"] = nullptr;
        eq["boilerInValid"] = es.boilerInValid;
        if (isfinite(es.targetFlowC)) eq["targetFlowC"] = es.targetFlowC; else eq["targetFlowC"] = nullptr;
        if (isfinite(es.actualC)) eq["actualC"] = es.actualC; else eq["actualC"] = nullptr;
        if (isfinite(es.targetC)) eq["targetC"] = es.targetC; else eq["targetC"] = nullptr;
        eq["akuSupportActive"] = es.akuSupportActive;
        if (es.akuSupportReason.length()) eq["akuSupportReason"] = es.akuSupportReason; else eq["akuSupportReason"] = nullptr;
        if (isfinite(es.akuTopC)) eq["akuTopC"] = es.akuTopC; else eq["akuTopC"] = nullptr;
        if (isfinite(es.akuMidC)) eq["akuMidC"] = es.akuMidC; else eq["akuMidC"] = nullptr;
        if (isfinite(es.akuBottomC)) eq["akuBottomC"] = es.akuBottomC; else eq["akuBottomC"] = nullptr;
        eq["akuTopValid"] = es.akuTopValid;
        eq["akuMidValid"] = es.akuMidValid;
        eq["akuBottomValid"] = es.akuBottomValid;

        eq["valveMaster"]   = es.valveMaster;     // 0 = none
        eq["valvePosPct"]   = es.valvePosPct;
        eq["valveTargetPct"]= es.valveTargetPct;
        eq["valveMoving"]   = es.valveMoving;
        eq["lastAdjustMs"]  = es.lastAdjustMs;
    }

    // --- valves (V2/V3 detail) ---
    {
        JsonObject valves = doc.createNestedObject("valves");
        EquithermStatus es = logicGetEquithermStatus();
        JsonObject v2 = valves.createNestedObject("v2");
        v2["currentPct"] = es.valvePosPct;
        v2["targetPct"] = es.valveTargetPct;
        v2["moving"] = es.valveMoving;

        JsonObject v3 = valves.createNestedObject("v3");
        v3["chPct"] = ts.chPct;
        v3["bypassPct"] = ts.bypassPct;
        v3["invert"] = ts.bypassInvert;
        v3["targetPct"] = ts.valveTargetPct;
        v3["currentPct"] = ts.valvePosPct;
        bool v3moving = false;
        if (ts.valveMaster >= 1) {
            ValveUiStatus vs;
            if (logicGetValveUiStatus(ts.valveMaster, vs)) v3moving = vs.moving;
        }
        v3["moving"] = v3moving;
    }

    // --- sensors ---
    {
        EquithermStatus es = logicGetEquithermStatus();
        JsonObject sensors = doc.createNestedObject("sensors");
        JsonObject outdoor = sensors.createNestedObject("outdoor");
        outdoor["valid"] = es.outdoorValid;
        outdoor["ageMs"] = es.outdoorAgeMs;
        outdoor["source"] = "equitherm";
    }

    // --- recirc ---
    {
        RecircStatus rs = logicGetRecircStatus();
        JsonObject rec = doc.createNestedObject("recirc");
        rec["enabled"] = rs.enabled;
        rec["active"] = rs.active;
        rec["mode"] = rs.mode;
        rec["untilMs"] = rs.untilMs;
        rec["remainingMs"] = rs.remainingMs;
        rec["stopReached"] = rs.stopReached;
        if (isfinite(rs.returnTempC)) rec["returnTempC"] = rs.returnTempC; else rec["returnTempC"] = nullptr;
        rec["returnTempValid"] = rs.returnTempValid;
    }

    // --- AKU heater ---
    {
        AkuHeaterStatus hs = logicGetAkuHeaterStatus();
        JsonObject h = doc.createNestedObject("akuHeater");
        h["enabled"] = hs.enabled;
        h["active"] = hs.active;
        h["mode"] = hs.mode;
        h["reason"] = hs.reason;
        if (isfinite(hs.topC)) h["topC"] = hs.topC; else h["topC"] = nullptr;
        h["topValid"] = hs.topValid;
    }

    // --- opentherm ---
    {
        JsonObject ot = doc.createNestedObject("opentherm");
        openthermFillJson(ot);
    }

    // Aktualizuj cache timestamp pro případ, že UI pošle několik requestů těsně po sobě.
    g_statusCacheAtMs = nowMs;
    g_statusCacheValid = true;

    // ✅ Stream JSON bez String out
    sendJsonStreamed200(doc);
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

    DynamicJsonDocument filter(2048);
    filter["iofunc"] = true;
    filter["equitherm"] = true;
    filter["tuv"] = true;
    filter["dhwRecirc"] = true;
    filter["akuHeater"] = true;
    filter["system"] = true;
    filter["sensors"] = true;
    filter["schedules"] = true;
    filter["mqtt"] = true;
    filter["time"] = true;
    filter["thermometers"] = true;
    filter["tempRoles"] = true;
    filter["dallasGpios"] = true;
    filter["dallasAddrs"] = true;
    filter["dallasNames"] = true;
    filter["opentherm"] = true;
    filter["relayNames"] = true;
    filter["inputNames"] = true;
    filter["inputActiveLevels"] = true;
    filter["inputs"] = true;
    filter["relayMap"] = true;
    filter["modes"] = true;
    filter["modeNames"] = true;
    filter["modeDescriptions"] = true;
    filter["mode_names"] = true;
    filter["mode_descriptions"] = true;
    filter["autoDefaultOffUnmapped"] = true;
    filter["auto_default_off_unmapped"] = true;

    DynamicJsonDocument doc(configJsonCapacityForSize(body.length()));
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }
    if (!doc.is<JsonObject>()) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    // Validace: relé používaná pro 3c ventily nelze použít jako běžná relé v jiných funkcích
    {
        String errMsg;
        int errRelay = 0;
        if (!validateNoRelayConflictsWithValves(doc, errMsg, errRelay)) {
            DynamicJsonDocument out(512);
            out["error"] = "relay_conflict_with_valve";
            out["relay"] = errRelay;
            out["message"] = errMsg;
            String resp;
            serializeJson(out, resp);
            server.send(400, "application/json", resp);
            return;
        }
    }

    String sanitized;
    serializeJson(doc, sanitized);

    String previous = g_configJson;
    g_configJson = sanitized;
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
    String errCode;
    const bool ok = bleSetConfigJson(body, &errCode);
    Serial.printf("[API] BLE config POST len=%u result=%s err=%s\n",
                  (unsigned)body.length(), ok ? "ok" : "fail",
                  errCode.length() ? errCode.c_str() : "-");
    if (!ok) {
        if (!errCode.length()) errCode = "save/apply_failed";
        String out = String("{\"error\":\"") + errCode + "\"}";
        const int status = (errCode == "bad_json") ? 400 : 500;
        server.send(status, "application/json", out);
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

void handleApiBleMeteoRetryPost() {
    const bool enabled = bleMeteoRetryNow();
    if (!enabled) {
        server.send(200, "application/json", "{\"status\":\"ok\",\"meteoEnabled\":false}");
        return;
    }
    server.send(200, "application/json", "{\"status\":\"ok\",\"meteoEnabled\":true}");
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

        if (action == "valve_pulse" || action == "valve_stop" || action == "valve_goto") {
            // Bezpečné chování: kalibrace / ruční zásah => MANUAL
            if (logicGetControlMode() == ControlMode::AUTO) {
                logicSetControlMode(ControlMode::MANUAL);
            }

            const uint8_t master = (uint8_t)(doc["master"] | doc["relay"] | 0); // 1..8
            if (master < 1 || master > RELAY_COUNT) {
                server.send(400, "application/json", "{\"error\":\"master must be 1-8\"}");
                return;
            }

            // Optional runtime config override (useful for calibration without saving config)
            const bool hasCfg = doc.containsKey("peer") || doc.containsKey("peerRel") ||
                                doc.containsKey("singleRelay") || doc.containsKey("invertDir") ||
                                doc.containsKey("travelTime") || doc.containsKey("travelTimeS") ||
                                doc.containsKey("pulseTime") || doc.containsKey("pulseTimeS") ||
                                doc.containsKey("guardTime") || doc.containsKey("guardTimeS") ||
                                doc.containsKey("minSwitchS") || doc.containsKey("minSwitch") ||
                                doc.containsKey("defaultPos");
            if (hasCfg) {
                const uint8_t peer = (uint8_t)(doc["peer"] | doc["peerRel"] | 0);
                const bool singleRelay = (bool)(doc["singleRelay"] | false);
                const bool invertDir = (bool)(doc["invertDir"] | false);

                float travelS = NAN;
                if (doc.containsKey("travelTimeS")) travelS = doc["travelTimeS"].as<float>();
                else if (doc.containsKey("travelTime")) travelS = doc["travelTime"].as<float>();

                float pulseS = NAN;
                if (doc.containsKey("pulseTimeS")) pulseS = doc["pulseTimeS"].as<float>();
                else if (doc.containsKey("pulseTime")) pulseS = doc["pulseTime"].as<float>();

                float guardS = NAN;
                if (doc.containsKey("guardTimeS")) guardS = doc["guardTimeS"].as<float>();
                else if (doc.containsKey("guardTime")) guardS = doc["guardTime"].as<float>();

                float minSwitchS = NAN;
                if (doc.containsKey("minSwitchS")) minSwitchS = doc["minSwitchS"].as<float>();
                else if (doc.containsKey("minSwitch")) minSwitchS = doc["minSwitch"].as<float>();

                const char* dp = (const char*)(doc["defaultPos"] | "A");
                const char defaultPos = (dp && dp[0]) ? dp[0] : 'A';

                if (!logicValveManualConfigure(master, peer, singleRelay, invertDir, travelS, pulseS, guardS, minSwitchS, defaultPos)) {
                    server.send(400, "application/json", "{\"error\":\"invalid valve cfg\"}");
                    return;
                }
            }

            if (action == "valve_pulse") {
                const int dirI = (int)(doc["dir"] | doc["direction"] | 0);
                const int8_t dir = (dirI >= 0) ? (int8_t)1 : (int8_t)-1;
                if (!logicValvePulse(master, dir)) {
                    server.send(400, "application/json", "{\"error\":\"valve pulse failed\"}");
                    return;
                }
                server.send(200, "application/json", "{\"ok\":true}");
                return;
            }

            if (action == "valve_stop") {
                if (!logicValveStop(master)) {
                    server.send(400, "application/json", "{\"error\":\"valve stop failed\"}");
                    return;
                }
                server.send(200, "application/json", "{\"ok\":true}");
                return;
            }

            if (action == "valve_goto") {
                const int pctI = (int)(doc["pct"] | doc["targetPct"] | 0);
                uint8_t pct = (pctI < 0) ? 0 : (pctI > 100 ? 100 : (uint8_t)pctI);
                if (!logicValveGotoPct(master, pct)) {
                    server.send(400, "application/json", "{\"error\":\"valve goto failed\"}");
                    return;
                }
                server.send(200, "application/json", "{\"ok\":true}");
                return;
            }
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
    fsLock();
    File root = LittleFS.open("/");
    if (!root) {
        fsUnlock();
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
    root.close();
    fsUnlock();

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

    fsLock();
    if (!LittleFS.exists(path)) {
        fsUnlock();
        server.send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    if (!LittleFS.remove(path)) {
        fsUnlock();
        server.send(500, "application/json", "{\"error\":\"remove failed\"}");
        return;
    }
    fsUnlock();

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
    doc["mqtt"] = true;
    doc["ble"] = true;
    doc["opentherm"] = true;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void webserverLoadConfigFromFS() {
    if (fsIsReady()) {
        loadConfigFromFS();
    }
}

// ===== Inicializace webserveru =====
void webserverInit() {
    // FS je mountnutý v setup() přes fsInit(); tady jen načti konfiguraci, pokud je dostupná
    if (fsIsReady() && !g_configLoaded) {
        loadConfigFromFS();
    }

    server.on("/", HTTP_GET, []() {
        if (!fsIsReady()) {
            sendWebUiFallback();
            return;
        }
        fsLock();
        const bool hasIndex = LittleFS.exists("/index.html");
        fsUnlock();
        if (!hasIndex) {
            sendWebUiFallback();
            return;
        }
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
    server.on("/api/ble/meteo/retry", HTTP_GET, handleApiBleMeteoRetryPost);
    server.on("/api/ble/meteo/retry", HTTP_POST, handleApiBleMeteoRetryPost);
    server.on("/api/ble/remove", HTTP_POST, handleApiBleRemovePost);

    server.on("/api/mode_ctrl", HTTP_GET, handleApiModeCtrlGet);
    server.on("/api/mode_ctrl", HTTP_POST, handleApiModeCtrlPost);

    // Reboot
    server.on("/api/reboot", HTTP_POST, handleApiRebootPost);

    // FS manager
    server.on("/api/fs/list", HTTP_GET, handleApiFsList);
    server.on("/api/fs/delete", HTTP_POST, handleApiFsDelete);
    server.on("/api/fs/upload", HTTP_POST, handleApiFsUpload, handleApiFsUploadDone);

    server.begin();
    Serial.println(F("[WEB] WebServer started."));
}

void webserverLoop() {
    server.handleClient();

    // Throttled system/heap health log (stability).
    static uint32_t s_lastDiagLogMs = 0;
    const uint32_t nowMs = millis();
    if ((uint32_t)(nowMs - s_lastDiagLogMs) >= 60000UL) {
        s_lastDiagLogMs = nowMs;
        const uint32_t freeHeap = (uint32_t)ESP.getFreeHeap();
        const uint32_t minFree  = (uint32_t)ESP.getMinFreeHeap();
        const uint32_t largest  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        Serial.printf("[SYS] heap free=%u min=%u largest=%u; i2c relayErr=%u recov=%u; cfgApply fail=%u noMem=%u\n",
                      freeHeap, minFree, largest,
                      (unsigned)relayGetI2cErrorCount(), (unsigned)relayGetI2cRecoveryCount(),
                      (unsigned)logicGetConfigApplyFailCount(), (unsigned)logicGetConfigApplyNoMemoryCount());
    }


    if (g_rebootPending) {
        const uint32_t now = millis();
        if ((int32_t)(now - g_rebootAtMs) >= 0) {
            Serial.println(F("[SYS] Rebooting now..."));
            ESP.restart();
        }
    }
}
