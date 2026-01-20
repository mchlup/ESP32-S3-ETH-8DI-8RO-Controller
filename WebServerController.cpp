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
#include <vector>

#include "RelayController.h"
#include "JsonUtils.h"
#include "Log.h"
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
static bool g_configOk = false;
static String g_configLastError = "";
static uint32_t g_configLoadWarningsCount = 0;


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
static const char* httpStatusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

static void sendJsonStreamed(const JsonDocument& doc, int statusCode) {
    WiFiClient client = server.client();

    // Fallback (nemělo by nastat, ale radši bezpečně)
    if (!client) {
        String out;
        serializeJson(doc, out);
        server.send(statusCode, "application/json", out);
        return;
    }

    // Spočti délku bez alokace Stringu
    const size_t len = measureJson(doc);

    // Ručně odešli HTTP hlavičky + Content-Length, pak JSON přímo do socketu
    client.printf("HTTP/1.1 %d %s\r\n", statusCode, httpStatusText(statusCode));
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

static void sendJsonStreamed200(const JsonDocument& doc) {
    sendJsonStreamed(doc, 200);
}

static void appendValidationIssues(JsonArray arr, const ValidationResult& validation, bool includeFatal) {
    for (const auto& issue : validation.issues) {
        if (!includeFatal && issue.fatal) continue;
        JsonObject o = arr.createNestedObject();
        o["code"] = issue.code;
        o["path"] = issue.path;
        o["message"] = issue.message;
    }
}

static void sendApiOk(const JsonDocument& dataDoc, const ValidationResult* warnings = nullptr, int statusCode = 200) {
    const size_t extra = 256 + (warnings ? warnings->issues.size() * 128 : 0);
    DynamicJsonDocument resp(dataDoc.memoryUsage() + extra);
    JsonObject root = resp.to<JsonObject>();
    root["ok"] = true;
    root["data"] = dataDoc.as<JsonVariant>();
    JsonArray warnArr = root.createNestedArray("warnings");
    if (warnings) {
        appendValidationIssues(warnArr, *warnings, false);
    }
    sendJsonStreamed(resp, statusCode);
}

static void sendApiError(const char* code, const String& message, const ValidationResult* details, int statusCode) {
    const size_t extra = 256 + (details ? details->issues.size() * 128 : 0);
    DynamicJsonDocument resp(extra);
    JsonObject root = resp.to<JsonObject>();
    root["ok"] = false;
    JsonObject err = root.createNestedObject("error");
    err["code"] = code ? code : "internal_error";
    err["message"] = message;
    JsonArray detailArr = err.createNestedArray("details");
    if (details) {
        appendValidationIssues(detailArr, *details, true);
    }
    sendJsonStreamed(resp, statusCode);
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

static void loadConfigFromFS() {
    const char* cfgPath = "/config.json";
    const char* bakPath = "/config.json.bak";
    bool restoredFromBak = false;

    DynamicJsonDocument doc(2048);
    String err;
    if (!loadJsonFromFile(cfgPath, doc, err)) {
        LOGW("CFG load failed: %s", err.c_str());
        if (!loadJsonFromFile(bakPath, doc, err)) {
            g_configJson = "{}";
            g_configOk = false;
            g_configLastError = "config_load_failed";
            g_configLoadWarningsCount = 1;
            LOGW("CFG load failed (bak): %s", err.c_str());
            applyAllConfig(g_configJson);
            return;
        }
        restoredFromBak = true;
    }

    ValidationResult validation;
    applyDefaultsAndValidate(doc, validation);

    if (!doc.is<JsonObject>()) {
        g_configJson = "{}";
        g_configOk = false;
        g_configLastError = "invalid_config_object";
        g_configLoadWarningsCount = 1;
        applyAllConfig(g_configJson);
        return;
    }

    g_configLoadWarningsCount = validation.issues.size();
    g_configOk = validation.ok;
    g_configLastError = validation.ok ? "" : "config_validation_failed";

    String cfg;
    serializeJson(doc, cfg);
    g_configJson = cfg;

    if (restoredFromBak) {
        String saveErr;
        if (!saveJsonToFileAtomic(cfgPath, doc, saveErr)) {
            LOGW("CFG restore write-back failed: %s", saveErr.c_str());
        } else {
            LOGI("CFG restored from .bak and saved.");
        }
    }

    applyAllConfig(g_configJson);
    LOGI("CFG loaded & applied.");
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
    DynamicJsonDocument doc(4096);
    String err;
    if (!parseJsonBody(g_configJson, doc, 65536, err)) {
        g_configLastError = err;
        LOGW("CFG save parse failed: %s", err.c_str());
        return false;
    }
    return saveJsonToFileAtomic("/config.json", doc, err);
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
        i2c["ok"] = relayIsOk();
        i2c["failCount"] = relayGetI2cFailCount();
        i2c["nextRetryInMs"] = relayGetI2cNextRetryInMs();
        const char* lastI2c = relayGetI2cLastError();
        if (lastI2c && lastI2c[0]) i2c["relayLastError"] = lastI2c; else i2c["relayLastError"] = nullptr;

        JsonObject jsonDiag = diag.createNestedObject("json");
        const JsonDiagnostics& jd = jsonGetDiagnostics();
        jsonDiag["parseErrors"] = jd.parseErrors;
        jsonDiag["lastError"] = jd.lastError.length() ? jd.lastError : nullptr;
        jsonDiag["lastCapacity"] = (uint32_t)jd.lastCapacity;
        jsonDiag["lastUsage"] = (uint32_t)jd.lastUsage;

        JsonObject cfgDiag = diag.createNestedObject("config");
        cfgDiag["ok"] = g_configOk;
        cfgDiag["loadWarningsCount"] = g_configLoadWarningsCount;
        cfgDiag["lastError"] = g_configLastError.length() ? g_configLastError : nullptr;

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

    // --- BLE state ---
    JsonObject ble = doc.createNestedObject("ble");
    ble["state"] = bleGetStateName();
    ble["lastDataAge"] = bleGetLastDataAgeMs();
    ble["reconnects"] = bleGetReconnectCount();
    ble["failCount"] = bleGetFailCount();

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
        sendApiError("validation_failed", "Chybí id nebo cmd.", nullptr, 400);
        return;
    }

    int id = server.arg("id").toInt();
    String cmd = server.arg("cmd");
    cmd.toLowerCase();

    if (id < 1 || id > RELAY_COUNT) {
        sendApiError("validation_failed", "Neplatné id relé.", nullptr, 400);
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
        sendApiError("validation_failed", "Neplatný příkaz.", nullptr, 400);
        return;
    }

    DynamicJsonDocument data(128);
    data["id"] = id;
    data["state"] = relayGetState(static_cast<RelayId>(id - 1));
    sendApiOk(data);
}

// ===== API config =====

void handleApiConfigGet() {
    DynamicJsonDocument cfgDoc(4096);
    ValidationResult warnings;
    String err;
    if (!parseJsonBody(g_configJson, cfgDoc, 65536, err)) {
        cfgDoc.clear();
        cfgDoc.to<JsonObject>();
        warnings.addIssue("config_load_failed_using_defaults", "$", "Načtení konfigurace selhalo, použity defaulty.");
    }
    applyDefaultsAndValidate(cfgDoc, warnings);
    sendApiOk(cfgDoc, &warnings);
}

void handleApiConfigPost() {
    constexpr size_t kMaxConfigBytes = 32768;
    String body = server.arg("plain");
    String err;
    DynamicJsonDocument incoming(2048);
    if (!parseJsonBody(body, incoming, kMaxConfigBytes, err)) {
        const bool tooLarge = err == "payload_too_large";
        sendApiError(tooLarge ? "payload_too_large" : "bad_json",
                     tooLarge ? "JSON payload je příliš velký." : "Neplatný JSON.",
                     nullptr,
                     tooLarge ? 413 : 400);
        return;
    }
    if (!incoming.is<JsonObject>()) {
        sendApiError("validation_failed", "JSON musí být objekt.", nullptr, 400);
        return;
    }

    ValidationResult validation;
    JsonObject inObj = incoming.as<JsonObject>();
    const char* allowedKeys[] = {
        "iofunc", "equitherm", "tuv", "dhwRecirc", "akuHeater", "system",
        "sensors", "schedules", "mqtt", "time", "thermometers", "tempRoles",
        "dallasGpios", "dallasAddrs", "dallasNames", "opentherm",
        "relayNames", "inputNames", "inputActiveLevels", "inputs", "relayMap",
        "modes", "modeNames", "modeDescriptions", "mode_names", "mode_descriptions",
        "autoDefaultOffUnmapped", "auto_default_off_unmapped"
    };

    std::vector<String> unknownKeys;
    for (JsonPair kv : inObj) {
        bool known = false;
        for (const char* key : allowedKeys) {
            if (kv.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            unknownKeys.push_back(String(kv.key().c_str()));
        }
    }
    for (const String& key : unknownKeys) {
        inObj.remove(key);
        validation.addIssue("unknown_key", String("$.") + key, "Neznámý klíč byl ignorován.");
    }

    DynamicJsonDocument baseDoc(4096);
    if (!parseJsonBody(g_configJson, baseDoc, 65536, err)) {
        baseDoc.clear();
        baseDoc.to<JsonObject>();
        validation.addIssue("config_load_failed_using_defaults", "$", "Načtení stávající konfigurace selhalo, použity defaulty.");
    }

    JsonObject baseObj = baseDoc.as<JsonObject>();
    for (JsonPair kv : inObj) {
        baseObj[kv.key()] = kv.value();
    }

    applyDefaultsAndValidate(baseDoc, validation);

    // Validace: relé používaná pro 3c ventily nelze použít jako běžná relé v jiných funkcích
    {
        String errMsg;
        int errRelay = 0;
        if (!validateNoRelayConflictsWithValves(baseDoc, errMsg, errRelay)) {
            ValidationResult errRes;
            errRes.addIssue("relay_conflict_with_valve", "$.relayMap", errMsg, true);
            sendApiError("validation_failed", "Konflikt relé s 3c ventilem.", &errRes, 400);
            return;
        }
    }

    if (!validation.ok) {
        sendApiError("validation_failed", "Konfigurace neprošla validací.", &validation, 400);
        return;
    }

    String sanitized;
    serializeJson(baseDoc, sanitized);

    String previous = g_configJson;
    g_configJson = sanitized;
    if (!saveConfigToFS()) {
        g_configJson = previous;
        g_configOk = false;
        g_configLastError = "save_failed";
        sendApiError("internal_error", "Uložení konfigurace selhalo.", nullptr, 500);
        return;
    }

    applyAllConfig(g_configJson);
    g_configOk = true;
    g_configLastError = "";
    g_configLoadWarningsCount = validation.issues.size();

    DynamicJsonDocument data(64);
    data["saved"] = true;
    sendApiOk(data, validation.issues.empty() ? nullptr : &validation);
}


// ===== API BLE =====

void handleApiBleStatus() {
    DynamicJsonDocument data(4096);
    ValidationResult warnings;
    String err;
    if (!parseJsonBody(bleGetStatusJson(), data, 16384, err)) {
        warnings.addIssue("bad_json", "$", "BLE status není validní JSON.");
        data.clear();
        data.to<JsonObject>();
    }
    sendApiOk(data, warnings.issues.empty() ? nullptr : &warnings);
}

void handleApiOpenThermStatus() {
    DynamicJsonDocument doc(512);
    JsonObject obj = doc.to<JsonObject>();
    openthermFillJson(obj);
    sendApiOk(doc);
}

void handleApiBleConfigGet() {
    DynamicJsonDocument data(4096);
    ValidationResult warnings;
    String err;
    if (!parseJsonBody(bleGetConfigJson(), data, 16384, err)) {
        warnings.addIssue("bad_json", "$", "BLE konfigurace není validní JSON.");
        data.clear();
        data.to<JsonObject>();
    }
    sendApiOk(data, warnings.issues.empty() ? nullptr : &warnings);
}

void handleApiBleConfigPost() {
    constexpr size_t kMaxBleBytes = 8192;
    String body = server.arg("plain");
    String err;
    DynamicJsonDocument incoming(2048);
    if (!parseJsonBody(body, incoming, kMaxBleBytes, err)) {
        const bool tooLarge = err == "payload_too_large";
        sendApiError(tooLarge ? "payload_too_large" : "bad_json",
                     tooLarge ? "JSON payload je příliš velký." : "Neplatný JSON.",
                     nullptr,
                     tooLarge ? 413 : 400);
        return;
    }
    if (!incoming.is<JsonObject>()) {
        sendApiError("validation_failed", "JSON musí být objekt.", nullptr, 400);
        return;
    }

    ValidationResult validation;
    JsonObject obj = incoming.as<JsonObject>();
    if (obj.containsKey("passkey") && !obj["passkey"].is<uint32_t>()) {
        obj["passkey"] = 123456;
        validation.addIssue("wrong_type", "$.passkey", "Hodnota passkey byla opravena.");
    }
    if (obj.containsKey("meteoConnectTimeoutMs")) {
        uint32_t val = obj["meteoConnectTimeoutMs"] | 900;
        if (val < 300) {
            obj["meteoConnectTimeoutMs"] = 300;
            validation.addIssue("out_of_range", "$.meteoConnectTimeoutMs", "Timeout navýšen na 300ms.");
        } else if (val > 5000) {
            obj["meteoConnectTimeoutMs"] = 5000;
            validation.addIssue("out_of_range", "$.meteoConnectTimeoutMs", "Timeout omezen na 5000ms.");
        }
    }
    if (obj.containsKey("meteoScanMs")) {
        uint32_t val = obj["meteoScanMs"] | 4000;
        if (val < 1000) {
            obj["meteoScanMs"] = 1000;
            validation.addIssue("out_of_range", "$.meteoScanMs", "Scan navýšen na 1000ms.");
        } else if (val > 30000) {
            obj["meteoScanMs"] = 30000;
            validation.addIssue("out_of_range", "$.meteoScanMs", "Scan omezen na 30000ms.");
        }
    }

    String sanitized;
    serializeJson(incoming, sanitized);
    String errCode;
    const bool ok = bleSetConfigJson(sanitized, &errCode);
    LOGI("API BLE config POST len=%u result=%s err=%s",
         (unsigned)sanitized.length(), ok ? "ok" : "fail",
         errCode.length() ? errCode.c_str() : "-");
    if (!ok) {
        if (!errCode.length()) errCode = "save/apply_failed";
        sendApiError((errCode == "bad_json") ? "bad_json" : "internal_error",
                     "Uložení BLE konfigurace selhalo.",
                     nullptr,
                     (errCode == "bad_json") ? 400 : 500);
        return;
    }

    DynamicJsonDocument data(64);
    data["saved"] = true;
    sendApiOk(data, validation.issues.empty() ? nullptr : &validation);
}

void handleApiBlePairedGet() {
    DynamicJsonDocument data(2048);
    ValidationResult warnings;
    String err;
    if (!parseJsonBody(bleGetPairedJson(), data, 16384, err)) {
        warnings.addIssue("bad_json", "$", "Seznam zařízení není validní JSON.");
        data.clear();
        data.to<JsonObject>();
    }
    sendApiOk(data, warnings.issues.empty() ? nullptr : &warnings);
}

void handleApiBlePairPost() {
    String body = server.arg("plain");
    body.trim();

    DynamicJsonDocument doc(512);
    String err;
    if (!parseJsonBody(body, doc, 1024, err)) {
        sendApiError("bad_json", "Neplatný JSON.", nullptr, 400);
        return;
    }

    uint32_t seconds = doc["seconds"] | 120;
    const char* role = doc["role"] | "";

    if (!bleStartPairing(seconds, String(role))) {
        sendApiError("internal_error", "Spuštění párování selhalo.", nullptr, 500);
        return;
    }
    DynamicJsonDocument data(64);
    data["pairing"] = true;
    data["seconds"] = seconds;
    sendApiOk(data);
}

void handleApiBleStopPairPost() {
    bleStopPairing();
    DynamicJsonDocument data(64);
    data["pairing"] = false;
    sendApiOk(data);
}

void handleApiBleMeteoRetryPost() {
    const bool enabled = bleMeteoRetryNow();
    DynamicJsonDocument data(64);
    data["meteoEnabled"] = enabled;
    sendApiOk(data);
}

void handleApiBleRemovePost() {
    String body = server.arg("plain");
    body.trim();
    DynamicJsonDocument doc(512);
    String err;
    if (!parseJsonBody(body, doc, 1024, err)) {
        sendApiError("bad_json", "Neplatný JSON.", nullptr, 400);
        return;
    }
    const char* mac = doc["mac"] | "";
    if (!strlen(mac)) {
        sendApiError("validation_failed", "Chybí MAC adresa.", nullptr, 400);
        return;
    }
    if (!bleRemoveDevice(String(mac))) {
        sendApiError("internal_error", "Odebrání zařízení selhalo.", nullptr, 500);
        return;
    }
    DynamicJsonDocument data(64);
    data["removed"] = true;
    sendApiOk(data);
}

// ===== API mode_ctrl =====

void handleApiModeCtrlGet() {
    DynamicJsonDocument data(128);
    data["controlMode"] = (logicGetControlMode() == ControlMode::AUTO) ? "auto" : "manual";
    data["systemMode"] = logicModeToString(logicGetMode());
    sendApiOk(data);
}

void handleApiModeCtrlPost() {
    // 1) JSON body (nové UI)
    String body = server.arg("plain");
    body.trim();
    if (body.length() && body[0] == '{') {
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, body);
        if (err) {
            sendApiError("bad_json", "Neplatný JSON.", nullptr, 400);
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
                sendApiError("validation_failed", "Neplatný režim.", nullptr, 400);
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
                sendApiError("validation_failed", "Relé musí být 1-8.", nullptr, 400);
                return;
            }

            // Bezpečné chování: ruční zásah => MANUAL
            if (logicGetControlMode() == ControlMode::AUTO) {
                logicSetControlMode(ControlMode::MANUAL);
            }
            logicSetRelayOutput((uint8_t)rid, val);
            DynamicJsonDocument data(128);
            data["relay"] = rid;
            data["value"] = val;
            sendApiOk(data);
            return;
        }

        if (action == "relay_raw") {
            const uint8_t relay = (uint8_t)(doc["relay"] | 0); // 1..8
            const bool on = (bool)(doc["on"] | false);
            if (relay < 1 || relay > RELAY_COUNT) {
                sendApiError("validation_failed", "Relé musí být 1-8.", nullptr, 400);
                return;
            }
            logicSetRelayRaw(relay, on);
            DynamicJsonDocument data(64);
            data["relay"] = relay;
            data["on"] = on;
            sendApiOk(data);
            return;
        }

        if (action == "valve_pulse" || action == "valve_stop" || action == "valve_goto") {
            // Bezpečné chování: kalibrace / ruční zásah => MANUAL
            if (logicGetControlMode() == ControlMode::AUTO) {
                logicSetControlMode(ControlMode::MANUAL);
            }

            const uint8_t master = (uint8_t)(doc["master"] | doc["relay"] | 0); // 1..8
            if (master < 1 || master > RELAY_COUNT) {
                sendApiError("validation_failed", "Master musí být 1-8.", nullptr, 400);
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
                    sendApiError("validation_failed", "Neplatná konfigurace ventilu.", nullptr, 400);
                    return;
                }
            }

            if (action == "valve_pulse") {
                const int dirI = (int)(doc["dir"] | doc["direction"] | 0);
                const int8_t dir = (dirI >= 0) ? (int8_t)1 : (int8_t)-1;
                if (!logicValvePulse(master, dir)) {
                    sendApiError("internal_error", "Puls ventilu selhal.", nullptr, 500);
                    return;
                }
                DynamicJsonDocument data(64);
                data["action"] = "valve_pulse";
                sendApiOk(data);
                return;
            }

            if (action == "valve_stop") {
                if (!logicValveStop(master)) {
                    sendApiError("internal_error", "Zastavení ventilu selhalo.", nullptr, 500);
                    return;
                }
                DynamicJsonDocument data(64);
                data["action"] = "valve_stop";
                sendApiOk(data);
                return;
            }

            if (action == "valve_goto") {
                const int pctI = (int)(doc["pct"] | doc["targetPct"] | 0);
                uint8_t pct = (pctI < 0) ? 0 : (pctI > 100 ? 100 : (uint8_t)pctI);
                if (!logicValveGotoPct(master, pct)) {
                    sendApiError("internal_error", "Nastavení ventilu selhalo.", nullptr, 500);
                    return;
                }
                DynamicJsonDocument data(96);
                data["action"] = "valve_goto";
                data["targetPct"] = pct;
                sendApiOk(data);
                return;
            }
        }

        if (action == "mqtt_discovery") {
            mqttRepublishDiscovery();
            DynamicJsonDocument data(64);
            data["discovery"] = true;
            sendApiOk(data);
            return;
        }

        if (action == "auto_recompute") {
            logicRecomputeFromInputs();
            handleApiModeCtrlGet();
            return;
        }

        sendApiError("validation_failed", "Neznámá akce.", nullptr, 400);
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
            sendApiError("validation_failed", "Neplatný režim.", nullptr, 400);
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
        sendApiError("validation_failed", "reboot=false", nullptr, 400);
        return;
    }
    DynamicJsonDocument data(128);
    data["msg"] = "Rebooting...";
    data["whenMs"] = 250;
    sendApiOk(data);
    g_rebootPending = true;
    g_rebootAtMs = millis() + 250;
}

// ===== File manager =====

void handleApiFsList() {
    if (!fsIsReady()) {
        sendApiError("internal_error", "Souborový systém není připojen.", nullptr, 500);
        return;
    }
    fsLock();
    File root = LittleFS.open("/");
    if (!root) {
        fsUnlock();
        sendApiError("internal_error", "Nelze otevřít root.", nullptr, 500);
        return;
    }

    // Pozn.: ArduinoJson minimalizuje fragmentaci heapu oproti ručnímu lepení Stringů
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.createNestedArray("files");

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

    sendApiOk(doc);
}

void handleApiFsDelete() {
    if (!server.hasArg("path")) {
        sendApiError("validation_failed", "Chybí path.", nullptr, 400);
        return;
    }
    String path = server.arg("path");
    if (!path.startsWith("/")) path = "/" + path;

    fsLock();
    if (!LittleFS.exists(path)) {
        fsUnlock();
        sendApiError("not_found", "Soubor nenalezen.", nullptr, 404);
        return;
    }

    if (!LittleFS.remove(path)) {
        fsUnlock();
        sendApiError("internal_error", "Smazání selhalo.", nullptr, 500);
        return;
    }
    fsUnlock();

    DynamicJsonDocument data(128);
    data["deleted"] = true;
    data["path"] = path;
    sendApiOk(data);
}

void handleApiFsUpload() {
    if (!handleFileUploadWrite()) {
        sendApiError("internal_error", "Upload selhal.", nullptr, 500);
    }
}

void handleApiFsUploadDone() {
    DynamicJsonDocument data(64);
    data["uploaded"] = true;
    sendApiOk(data);
}

// ===== API buzzer =====
static void handleApiBuzzerGet() {
    String json;
    buzzerToJson(json);
    DynamicJsonDocument data(1024);
    String err;
    ValidationResult warnings;
    if (!parseJsonBody(json, data, 4096, err)) {
        warnings.addIssue("bad_json", "$", "Buzzer status není validní JSON.");
        data.clear();
        data.to<JsonObject>();
    }
    sendApiOk(data, warnings.issues.empty() ? nullptr : &warnings);
}

static void handleApiBuzzerPost() {
    String body = server.arg("plain");
    body.trim();
    if (!body.length() || body[0] != '{') {
        sendApiError("bad_json", "Neplatný JSON.", nullptr, 400);
        return;
    }

    StaticJsonDocument<768> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        sendApiError("bad_json", "Neplatný JSON.", nullptr, 400);
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
            sendApiError("validation_failed", "Chybí config.", nullptr, 400);
            return;
        }
        buzzerUpdateFromJson(cfg);
        buzzerSaveToFS();
        handleApiBuzzerGet();
        return;
    }

    sendApiError("validation_failed", "Neznámá akce.", nullptr, 400);
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

    sendApiOk(doc);
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

    sendApiOk(doc);
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
