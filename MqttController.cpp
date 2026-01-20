#include "MqttController.h"

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "NetworkController.h"
#include "RelayController.h"
#include "InputController.h"
#include "LogicController.h"
#include "ThermometerController.h"

// ===== Cache přijatých hodnot =====
struct MqttCacheEntry {
    String topic;
    String value;
    uint32_t lastMs = 0;
};

static const uint8_t MQTT_CACHE_MAX = 16;
static MqttCacheEntry mqttCache[MQTT_CACHE_MAX];

static void cachePut(const String& topic, const String& value) {
    if (!topic.length()) return;

    // update existing
    for (uint8_t i = 0; i < MQTT_CACHE_MAX; i++) {
        if (mqttCache[i].topic == topic) {
            mqttCache[i].value = value;
            mqttCache[i].lastMs = millis();
            return;
        }
    }

    // find empty
    for (uint8_t i = 0; i < MQTT_CACHE_MAX; i++) {
        if (!mqttCache[i].topic.length()) {
            mqttCache[i].topic = topic;
            mqttCache[i].value = value;
            mqttCache[i].lastMs = millis();
            return;
        }
    }

    // replace oldest (simple LRU)
    uint8_t oldest = 0;
    uint32_t oldestMs = mqttCache[0].lastMs;
    for (uint8_t i = 1; i < MQTT_CACHE_MAX; i++) {
        if (mqttCache[i].lastMs < oldestMs) {
            oldestMs = mqttCache[i].lastMs;
            oldest = i;
        }
    }
    mqttCache[oldest].topic = topic;
    mqttCache[oldest].value = value;
    mqttCache[oldest].lastMs = millis();
}

bool mqttGetLastValue(const String& topic, String* outValue) {
    if (!outValue) return false;
    for (uint8_t i = 0; i < MQTT_CACHE_MAX; i++) {
        if (mqttCache[i].topic == topic) {
            *outValue = mqttCache[i].value;
            return true;
        }
    }
    return false;
}

bool mqttGetLastValueInfo(const String& topic, String* outValue, uint32_t* outLastMs) {
    if (!outValue && !outLastMs) return false;
    for (uint8_t i = 0; i < MQTT_CACHE_MAX; i++) {
        if (mqttCache[i].topic == topic) {
            if (outValue) *outValue = mqttCache[i].value;
            if (outLastMs) *outLastMs = mqttCache[i].lastMs;
            return true;
        }
    }
    return false;
}

// ===== Konfigurace MQTT =====

struct MqttConfig {
    bool     enabled   = false;
    String   host      = "";
    uint16_t port      = 1883;
    String   user      = "";
    String   pass      = "";
    String   clientId  = "";
    String   baseTopic = "espheat";
    String   haPrefix  = "homeassistant";
};

static MqttConfig mqttCfg;

static WiFiClient   wifiClient;
static PubSubClient mqttClient(wifiClient);

static uint32_t lastReconnectAttempt = 0;
static const uint32_t RECONNECT_INTERVAL_MS = 5000;

static uint32_t lastPublishMs = 0;
static const uint32_t PUBLISH_INTERVAL_MS = 10000;

// Poslední známé hodnoty pro změnové publikování
static bool lastRelayState[RELAY_COUNT] = { false };
static bool lastInputState[INPUT_COUNT] = { false };
static SystemMode  lastMode        = SystemMode::MODE1;
static ControlMode lastControlMode = ControlMode::MANUAL;

// Názvy pro Home Assistant discovery – aktualizují se z config.json
static String haRelayNames[RELAY_COUNT];
static String haInputNames[INPUT_COUNT];

// Pomocné funkce pro topic
static String topicRelayState(uint8_t idx) {
    return mqttCfg.baseTopic + "/relay/" + String(idx + 1) + "/state";
}
static String topicRelaySet(uint8_t idx) {
    return mqttCfg.baseTopic + "/relay/" + String(idx + 1) + "/set";
}
static String topicInputState(uint8_t idx) {
    return mqttCfg.baseTopic + "/input/" + String(idx + 1) + "/state";
}
static String topicModeState() {
    return mqttCfg.baseTopic + "/mode/state";
}
static String topicModeSet() {
    return mqttCfg.baseTopic + "/mode/set";
}
static String topicControlModeState() {
    return mqttCfg.baseTopic + "/control_mode/state";
}
static String topicControlModeSet() {
    return mqttCfg.baseTopic + "/control_mode/set";
}

static bool stringToMode(const String& s, SystemMode& out) {
    String x = s;
    x.toUpperCase();
    if (x == "MODE1")   { out = SystemMode::MODE1; return true; }
    if (x == "MODE2") { out = SystemMode::MODE2; return true; }
    if (x == "MODE3") { out = SystemMode::MODE3; return true; }
    if (x == "MODE4") { out = SystemMode::MODE4; return true; }
    if (x == "MODE5") { out = SystemMode::MODE5; return true; }
    return false;
}

static void publishHaDiscovery(const String relayNames[RELAY_COUNT], const String inputNames[INPUT_COUNT]) {
    if (!mqttClient.connected()) return;

    String devId = mqttCfg.clientId.length() ? mqttCfg.clientId : String("esp-heatctrl");
    DynamicJsonDocument doc(512);
    char payloadBuf[768];

    // --- Relé jako switch ---
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        doc.clear();
        String objId    = "relay" + String(i + 1);
        String uniqId   = devId + "_" + objId;
        String cfgTopic = mqttCfg.haPrefix + "/switch/" + devId + "/" + objId + "/config";

        String name = relayNames[i];
        if (!name.length()) name = "Relay " + String(i + 1);

        doc["name"]    = name;
        doc["uniq_id"] = uniqId;
        doc["cmd_t"]   = topicRelaySet(i);
        doc["stat_t"]  = topicRelayState(i);
        doc["pl_on"]   = "ON";
        doc["pl_off"]  = "OFF";
        doc["stat_on"] = "ON";
        doc["stat_off"]= "OFF";
        doc["retain"]  = true;

        JsonObject dev = doc.createNestedObject("dev");
        dev["ids"][0] = devId;
        dev["name"]   = "ESP Heat & Domestic Controller";
        dev["mf"]     = "Custom";
        dev["mdl"]    = "ESP32-S3-POE-8DI8DO";

        const size_t n = serializeJson(doc, payloadBuf, sizeof(payloadBuf));
        if (n > 0 && n < sizeof(payloadBuf)) {
            mqttClient.publish(cfgTopic.c_str(), payloadBuf, true);
        } else {
            // Fallback (nemělo by nastat, ale ať to nikdy nezkolabuje kvůli bufferu)
            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(cfgTopic.c_str(), payload.c_str(), true);
        }
    }

    // --- Vstupy jako binary_sensor ---
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        doc.clear();
        String objId    = "input" + String(i + 1);
        String uniqId   = devId + "_" + objId;
        String cfgTopic = mqttCfg.haPrefix + "/binary_sensor/" + devId + "/" + objId + "/config";

        String name = inputNames[i];
        if (!name.length()) name = "Input " + String(i + 1);

        doc["name"]    = name;
        doc["uniq_id"] = uniqId;
        doc["stat_t"]  = topicInputState(i);
        doc["pl_on"]   = "ON";
        doc["pl_off"]  = "OFF";
        doc["dev_cla"] = "power"; // obecné

        JsonObject dev = doc.createNestedObject("dev");
        dev["ids"][0] = devId;
        dev["name"]   = "ESP Heat & Domestic Controller";
        dev["mf"]     = "Custom";
        dev["mdl"]    = "ESP32-S3-POE-8DI8DO";

        const size_t n = serializeJson(doc, payloadBuf, sizeof(payloadBuf));
        if (n > 0 && n < sizeof(payloadBuf)) {
            mqttClient.publish(cfgTopic.c_str(), payloadBuf, true);
        } else {
            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(cfgTopic.c_str(), payload.c_str(), true);
        }
    }

    // --- Control mode jako select ---
    doc.clear();
    {
        String objId    = "control_mode";
        String uniqId   = devId + "_" + objId;
        String cfgTopic = mqttCfg.haPrefix + "/select/" + devId + "/" + objId + "/config";

        doc["name"]    = "Control Mode";
        doc["uniq_id"] = uniqId;
        doc["cmd_t"]   = topicControlModeSet();
        doc["stat_t"]  = topicControlModeState();
        doc["retain"]  = true;

        JsonArray opts = doc.createNestedArray("options");
        opts.add("auto");
        opts.add("manual");

        JsonObject dev = doc.createNestedObject("dev");
        dev["ids"][0] = devId;
        dev["name"]   = "ESP Heat & Domestic Controller";
        dev["mf"]     = "Custom";
        dev["mdl"]    = "ESP32-S3-POE-8DI8DO";

        const size_t n = serializeJson(doc, payloadBuf, sizeof(payloadBuf));
        if (n > 0 && n < sizeof(payloadBuf)) {
            mqttClient.publish(cfgTopic.c_str(), payloadBuf, true);
        } else {
            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(cfgTopic.c_str(), payload.c_str(), true);
        }
    }

    // --- Mode jako select ---
    doc.clear();
    {
        String objId    = "mode";
        String uniqId   = devId + "_" + objId;
        String cfgTopic = mqttCfg.haPrefix + "/select/" + devId + "/" + objId + "/config";

        doc["name"]    = "System Mode";
        doc["uniq_id"] = uniqId;
        doc["cmd_t"]   = topicModeSet();
        doc["stat_t"]  = topicModeState();
        doc["retain"]  = true;

        JsonArray opts = doc.createNestedArray("options");
        opts.add("MODE1");
        opts.add("MODE2");
        opts.add("MODE3");
        opts.add("MODE4");
        opts.add("MODE5");

        JsonObject dev = doc.createNestedObject("dev");
        dev["ids"][0] = devId;
        dev["name"]   = "ESP Heat & Domestic Controller";
        dev["mf"]     = "Custom";
        dev["mdl"]    = "ESP32-S3-POE-8DI8DO";

        const size_t n = serializeJson(doc, payloadBuf, sizeof(payloadBuf));
        if (n > 0 && n < sizeof(payloadBuf)) {
            mqttClient.publish(cfgTopic.c_str(), payloadBuf, true);
        } else {
            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(cfgTopic.c_str(), payload.c_str(), true);
        }
    }

    Serial.println(F("[MQTT] HA discovery published."));
}

static void mqttSubscribeAll() {
    if (!mqttClient.connected()) return;

    mqttClient.subscribe(topicModeSet().c_str());
    mqttClient.subscribe(topicControlModeSet().c_str());

    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        mqttClient.subscribe(topicRelaySet(i).c_str());
    }


// Extra subscriptions requested by logic (e.g., Equitherm MQTT sources)
String extraTopics[12];
uint8_t n = logicGetMqttSubscribeTopics(extraTopics, 12);
for (uint8_t i = 0; i < n; i++) {
    if (!extraTopics[i].length()) continue;
    mqttClient.subscribe(extraTopics[i].c_str());
}

    // Externí MQTT teploměry (konfigurace "Teploměry")
    {
        String tTopics[4];
        uint8_t tn = thermometersGetMqttSubscribeTopics(tTopics, 4);
        for (uint8_t i = 0; i < tn; i++) {
            if (!tTopics[i].length()) continue;
            mqttClient.subscribe(tTopics[i].c_str());
        }
    }

    Serial.println(F("[MQTT] Subscribed topics."));
}

static void publishRelayState(uint8_t idx) {
    if (!mqttClient.connected()) return;
    bool s = relayGetState(static_cast<RelayId>(idx));
    mqttClient.publish(topicRelayState(idx).c_str(), s ? "ON" : "OFF", true);
}

static void publishInputState(uint8_t idx) {
    if (!mqttClient.connected()) return;
    bool s = inputGetState(static_cast<InputId>(idx));
    mqttClient.publish(topicInputState(idx).c_str(), s ? "ON" : "OFF", true);
}

static void publishModeState() {
    if (!mqttClient.connected()) return;
    mqttClient.publish(topicModeState().c_str(), logicModeToString(logicGetMode()), true);
}

static void publishControlModeState() {
    if (!mqttClient.connected()) return;
    mqttClient.publish(topicControlModeState().c_str(),
                       (logicGetControlMode() == ControlMode::AUTO) ? "auto" : "manual",
                       true);
}

void mqttPublishFullState() {
    if (!mqttClient.connected()) return;

    for (uint8_t i = 0; i < RELAY_COUNT; i++) publishRelayState(i);
    for (uint8_t i = 0; i < INPUT_COUNT; i++) publishInputState(i);

    publishModeState();
    publishControlModeState();
}

static void handleControlModeSet(const String& payload) {
    String p = payload;
    p.toLowerCase();
    if (p == "auto") {
        logicSetControlMode(ControlMode::AUTO);
    } else if (p == "manual") {
        logicSetControlMode(ControlMode::MANUAL);
    }
}

static void handleModeSet(const String& payload) {
    // bezpečné chování: volba režimu přes MQTT vždy přepne do MANUAL
    SystemMode m;
    if (stringToMode(payload, m)) {
        logicSetControlMode(ControlMode::MANUAL);
        logicSetManualMode(m);
    }
}

static void handleRelaySet(uint8_t idx, const String& payload) {
    if (idx >= RELAY_COUNT) return;
    String p = payload;
    p.toUpperCase();
    bool on = (p == "ON" || p == "1" || p == "TRUE");

    // Bezpečné chování: ruční zásah přes MQTT => MANUAL
    if (logicGetControlMode() == ControlMode::AUTO) {
        logicSetControlMode(ControlMode::MANUAL);
    }

    relaySet(static_cast<RelayId>(idx), on);
}

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String t = String(topic);
    String p;
    p.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) p += (char)payload[i];
    p.trim();

    // uložíme poslední hodnotu (MQTT podmínky / diagnostika)
    cachePut(t, p);

    // externí MQTT teploměry (konfigurace "Teploměry")
    thermometersMqttOnMessage(t, p);

    if (t == topicControlModeSet()) {
        handleControlModeSet(p);
        return;
    }
    if (t == topicModeSet()) {
        handleModeSet(p);
        return;
    }

    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        if (t == topicRelaySet(i)) {
            handleRelaySet(i, p);
            return;
        }
    }
}

static bool mqttConnect() {
    if (!mqttCfg.enabled) return false;
    if (!networkIsConnected()) return false;

    mqttClient.setServer(mqttCfg.host.c_str(), mqttCfg.port);
    mqttClient.setCallback(mqttCallback);

    String cid = mqttCfg.clientId.length() ? mqttCfg.clientId : String("esp-heatctrl");
    bool ok = false;

    if (mqttCfg.user.length()) {
        ok = mqttClient.connect(cid.c_str(), mqttCfg.user.c_str(), mqttCfg.pass.c_str());
    } else {
        ok = mqttClient.connect(cid.c_str());
    }

    if (!ok) {
        Serial.print(F("[MQTT] Connect failed, rc="));
        Serial.println(mqttClient.state());
        return false;
    }

    Serial.println(F("[MQTT] Connected."));
    mqttSubscribeAll();
    publishHaDiscovery(haRelayNames, haInputNames);
    mqttPublishFullState();
    return true;
}

void mqttInit() {
    mqttClient.setBufferSize(2048);
    lastReconnectAttempt = 0;
    lastPublishMs = 0;

    for (uint8_t i = 0; i < RELAY_COUNT; i++) lastRelayState[i] = relayGetState(static_cast<RelayId>(i));
    for (uint8_t i = 0; i < INPUT_COUNT; i++) lastInputState[i] = inputGetState(static_cast<InputId>(i));

    lastMode = logicGetMode();
    lastControlMode = logicGetControlMode();
}

void mqttLoop() {
    if (!mqttCfg.enabled) return;

    if (!mqttClient.connected()) {
        uint32_t now = millis();
        if ((now - lastReconnectAttempt) >= RECONNECT_INTERVAL_MS) {
            lastReconnectAttempt = now;
            mqttConnect();
        }
        return;
    }

    mqttClient.loop();

    uint32_t now = millis();
    // Detekce/publikace změn nechceme spouštět zbytečně často.
    // MQTT client.loop() ale musí běžet stále.
    const bool allowFastPublish = (now - lastPublishMs) >= 200;
    if (allowFastPublish) {
        lastPublishMs = now;

        // změny relé
        for (uint8_t i = 0; i < RELAY_COUNT; i++) {
            bool s = relayGetState(static_cast<RelayId>(i));
            if (s != lastRelayState[i]) {
                lastRelayState[i] = s;
                publishRelayState(i);
            }
        }
    // změny vstupů
        for (uint8_t i = 0; i < INPUT_COUNT; i++) {
            bool s = inputGetState(static_cast<InputId>(i));
            if (s != lastInputState[i]) {
                lastInputState[i] = s;
                publishInputState(i);
            }
        }
    // změny módů
        SystemMode m = logicGetMode();
        if (m != lastMode) {
            lastMode = m;
            publishModeState();
        }

     ControlMode cm = logicGetControlMode();
        if (cm != lastControlMode) {
            lastControlMode = cm;
            publishControlModeState();
        }
    }

    // full state periodicky (pro jistotu)
    static uint32_t lastFull = 0;
    if ((now - lastFull) >= PUBLISH_INTERVAL_MS) {
        lastFull = now;
        mqttPublishFullState();
    }
}

void mqttApplyConfig(const String& json) {
    StaticJsonDocument<256> filter;
    // IMPORTANT: for arrays, using [0] in ArduinoJson filter means "only first element".
    filter["mqtt"] = true;
    filter["relayNames"] = true;
    filter["inputNames"] = true;
    // backward compatibility (older configs nested under cfg)
    filter["cfg"]["mqtt"] = true;
    filter["cfg"]["relayNames"] = true;
    filter["cfg"]["inputNames"] = true;

    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
    if (err) {
        Serial.print(F("[MQTT] config parse failed: "));
        Serial.println(err.c_str());
        return;
    }

    JsonObjectConst rootObj = doc.as<JsonObjectConst>();
    JsonObjectConst cfgObj = rootObj;
    if (rootObj.containsKey("cfg") && rootObj["cfg"].is<JsonObjectConst>()) cfgObj = rootObj["cfg"].as<JsonObjectConst>();

    // cfgObj is JsonObjectConst, so we must read as const variants.
    JsonObjectConst m = cfgObj["mqtt"].as<JsonObjectConst>();
    if (m.isNull()) {
        mqttCfg.enabled = false;
        return;
    }

    mqttCfg.enabled   = m["enabled"] | false;
    mqttCfg.host      = (const char*)(m["host"] | "");
    mqttCfg.port      = (uint16_t)(m["port"] | 1883);
    mqttCfg.user      = (const char*)(m["user"] | "");
    mqttCfg.pass      = (const char*)(m["pass"] | "");
    mqttCfg.clientId  = (const char*)(m["clientId"] | "");
    mqttCfg.baseTopic = (const char*)(m["baseTopic"] | "espheat");
    mqttCfg.haPrefix  = (const char*)(m["haPrefix"] | "homeassistant");

    // názvy z configu
    JsonArrayConst rn = cfgObj["relayNames"].as<JsonArrayConst>();
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        haRelayNames[i] = (rn.isNull() || i >= rn.size()) ? "" : String((const char*)(rn[i] | ""));
    }

    JsonArrayConst in = cfgObj["inputNames"].as<JsonArrayConst>();
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        haInputNames[i] = (in.isNull() || i >= in.size()) ? "" : String((const char*)(in[i] | ""));
    }

    if (mqttClient.connected()) {
        mqttSubscribeAll();
    }

    Serial.println(F("[MQTT] config applied."));
}

bool mqttIsConfigured() {
    return mqttCfg.enabled && mqttCfg.host.length();
}

bool mqttIsConnected() {
    return mqttClient.connected();
}

void mqttRepublishDiscovery() {
    if (!mqttClient.connected()) return;
    publishHaDiscovery(haRelayNames, haInputNames);
    mqttPublishFullState();
}
