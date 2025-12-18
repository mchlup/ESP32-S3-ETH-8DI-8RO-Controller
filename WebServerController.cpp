#include "WebServerController.h"

#include <Arduino.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "RelayController.h"
#include "InputController.h"
#include "LogicController.h"
#include "NetworkController.h"
#include "MqttController.h"
#include "BleController.h"
#include "RuleEngine.h"

static WebServer server(80);
static File uploadFile;

// Ne blokující restart (naplánovaný)
static bool     g_rebootPending = false;
static uint32_t g_rebootAtMs    = 0;

// Globální JSON konfigurace (popisy + mapování relé + režimy)
static String g_configJson;

// Rule engine JSON (uložené v LittleFS jako /rules.json)
static String g_rulesJson;
static bool   g_rulesEnabled = false;

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

    if (!LittleFS.exists(p)) return false;

    File f = LittleFS.open(p, "r");
    if (!f) return false;

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

static void loadConfigFromFS() {
    if (!LittleFS.exists("/config.json")) {
        g_configJson = "{}";
        Serial.println(F("[CFG] No config.json, using {}"));
        logicApplyConfig(g_configJson);
        mqttApplyConfig(g_configJson);
        return;
    }

    File f = LittleFS.open("/config.json", "r");
    if (!f) {
        g_configJson = "{}";
        Serial.println(F("[CFG] config.json open failed, using {}"));
        logicApplyConfig(g_configJson);
        mqttApplyConfig(g_configJson);
        return;
    }

    g_configJson = "";
    while (f.available()) g_configJson += (char)f.read();
    f.close();

    if (!g_configJson.length()) g_configJson = "{}";

    logicApplyConfig(g_configJson);
    mqttApplyConfig(g_configJson);

    Serial.println(F("[CFG] config.json loaded & applied."));
}

static bool saveConfigToFS() {
    File f = LittleFS.open("/config.json", "w");
    if (!f) return false;
    f.print(g_configJson);
    f.close();
    return true;
}

// ===== Rules (rules.json) =====
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

// ===== API status =====

void handleApiStatus() {
    String json = "{";

    // --- mode/control ---
    json += "\"systemMode\":\"";
    json += logicModeToString(logicGetMode());
    json += "\",";

    json += "\"controlMode\":\"";
    json += (logicGetControlMode() == ControlMode::AUTO) ? "auto" : "manual";
    json += "\",";
// --- AUTO diagnostika ---
AutoStatus as = logicGetAutoStatus();
json += "\"auto\":{";
json += "\"triggered\":";
json += as.triggered ? "true" : "false";
json += ",\"triggerInput\":";
json += String((uint32_t)as.triggerInput);
json += ",\"triggerMode\":\"";
json += logicModeToString(as.triggerMode);
json += "\"";
json += ",\"usingRelayMap\":";
json += as.usingRelayMap ? "true" : "false";
json += ",\"blockedByRules\":";
json += as.blockedByRules ? "true" : "false";
json += ",\"defaultOffUnmapped\":";
json += logicGetAutoDefaultOffUnmapped() ? "true" : "false";
json += "},";

    // --- relays ---
    json += "\"relays\":[";
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (i > 0) json += ",";
        json += relayGetState(static_cast<RelayId>(i)) ? "1" : "0";
    }
    json += "],";

    // --- inputs (RAW) ---
    json += "\"inputs\":[";
    for (int i = 0; i < INPUT_COUNT; i++) {
        if (i > 0) json += ",";
        json += inputGetRaw(static_cast<InputId>(i)) ? "1" : "0";
    }
    json += "],";

    // --- uptime ---
    json += "\"uptimeMs\":";
    json += String((uint32_t)millis());
    json += ",";

    // --- wifi ---
    const bool wifiConn = networkIsConnected();
    json += "\"wifi\":{";
    json += "\"connected\":";
    json += wifiConn ? "true" : "false";
    json += ",\"ip\":\"";
    json += networkGetIp();
    json += "\"";
    if (wifiConn) {
        json += ",\"rssi\":";
        json += String(WiFi.RSSI());
    }
    json += "},";

    // MQTT status
    json += "\"mqtt\":{";
    json += "\"configured\":";
    json += mqttIsConfigured() ? "true" : "false";
    json += ",\"connected\":";
    json += mqttIsConnected() ? "true" : "false";
    json += "}";

    // convenience (top-level) pro UI kompatibilitu
    json += ",\"ip\":\"";
    json += networkGetIp();
    json += "\"";
    json += ",\"wifiConnected\":";
    json += wifiConn ? "true" : "false";
    json += ",\"mqttConnected\":";
    json += mqttIsConnected() ? "true" : "false";

    // UI kompatibilita: top-level alias
    json += ",\"ruleEngineEnabled\":";
    json += ruleEngineIsEnabled() ? "true" : "false";

    // Rule Engine status
    json += ",\"rules\":";
    json += ruleEngineGetStatusJson();

    json += "}";

    server.send(200, "application/json", json);
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

    // validace JSON
    // UI může posílat delší popisy / názvy, 4kB bývá málo
    DynamicJsonDocument doc(12288);
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    g_configJson = body;

    if (!saveConfigToFS()) {
        server.send(500, "application/json", "{\"error\":\"save failed\"}");
        return;
    }

    logicApplyConfig(g_configJson);
    mqttApplyConfig(g_configJson);

    server.send(200, "application/json", "{\"status\":\"ok\"}");
}


// ===== API BLE =====

void handleApiBleStatus() {
    server.send(200, "application/json", bleGetStatusJson());
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
            relaySet(static_cast<RelayId>(rid - 1), val);
            String json = "{\"status\":\"ok\",\"relay\":";
            json += String(rid);
            json += ",\"value\":";
            json += val ? "true" : "false";
            json += "}";
            server.send(200, "application/json", json);
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
    File root = LittleFS.open("/");
    if (!root) {
        server.send(500, "application/json", "{\"error\":\"fs open failed\"}");
        return;
    }

    String json = "[";
    File file = root.openNextFile();
    bool first = true;
    while (file) {
        if (!first) json += ",";
        first = false;

        json += "{\"name\":\"";
        json += file.name();
        json += "\",\"size\":";
        json += String((uint32_t)file.size());
        json += "}";

        file = root.openNextFile();
    }
    json += "]";
    server.send(200, "application/json", json);
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

// ===== Inicializace webserveru =====

void webserverInit() {
    if (!LittleFS.begin()) {
        Serial.println(F("[FS] LittleFS mount failed."));
    } else {
        Serial.println(F("[FS] LittleFS mounted."));
        loadConfigFromFS();
        loadRulesFromFS();
    }

    server.on("/", HTTP_GET, []() {
        if (!handleFileRead("/index.html")) {
            server.send(404, "text/plain", "index.html not found");
        }
    });

    server.onNotFound([]() {
        if (!handleFileRead(server.uri())) {
            server.send(404, "text/plain", "Not found");
        }
    });

    // API
    server.on("/api/status", HTTP_GET, handleApiStatus);

    server.on("/api/relay", HTTP_GET, handleApiRelay);

    server.on("/api/config", HTTP_GET, handleApiConfigGet);
    server.on("/api/config", HTTP_POST, handleApiConfigPost);

    // BLE
    server.on("/api/ble/status", HTTP_GET, handleApiBleStatus);
    server.on("/api/ble/config", HTTP_GET, handleApiBleConfigGet);
    server.on("/api/ble/config", HTTP_POST, handleApiBleConfigPost);
    server.on("/api/ble/paired", HTTP_GET, handleApiBlePairedGet);
    server.on("/api/ble/pair", HTTP_POST, handleApiBlePairPost);
    server.on("/api/ble/pair/stop", HTTP_POST, handleApiBleStopPairPost);
    server.on("/api/ble/remove", HTTP_POST, handleApiBleRemovePost);


    server.on("/api/mode_ctrl", HTTP_GET, handleApiModeCtrlGet);
    server.on("/api/mode_ctrl", HTTP_POST, handleApiModeCtrlPost);

    // Rules
    server.on("/api/rules", HTTP_GET, handleApiRulesGet);
    server.on("/api/rules", HTTP_POST, handleApiRulesPost);
    server.on("/api/rules/status", HTTP_GET, handleApiRulesStatus);

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

    if (g_rebootPending) {
        const uint32_t now = millis();
        if ((int32_t)(now - g_rebootAtMs) >= 0) {
            Serial.println(F("[SYS] Rebooting now..."));
            ESP.restart();
        }
    }
}
