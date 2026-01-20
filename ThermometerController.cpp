#include "ThermometerController.h"

#include <ArduinoJson.h>

#include "ThermoRoles.h"

static MqttThermometerCfg s_mqtt[2];
static BleThermometerCfg  s_ble;

void thermometersInit() {
    for (uint8_t i = 0; i < 2; i++) {
        s_mqtt[i].name = "";
        s_mqtt[i].topic = "";
        s_mqtt[i].jsonKey = "tempC";
        s_mqtt[i].role = "";
    }

    s_ble.name = "BLE Meteo";
    s_ble.id   = "meteo.tempC";
    // Backward compatibility: historically BLE meteo was treated as outdoor by default.
    s_ble.role = "outdoor";
}

void thermometersApplyConfig(const String& json) {
    StaticJsonDocument<256> filter;
    filter["thermometers"]["mqtt"][0]["name"] = true;
    filter["thermometers"]["mqtt"][0]["topic"] = true;
    filter["thermometers"]["mqtt"][0]["jsonKey"] = true;
    filter["thermometers"]["mqtt"][0]["role"] = true;
    filter["thermometers"]["mqtt"][1]["name"] = true;
    filter["thermometers"]["mqtt"][1]["topic"] = true;
    filter["thermometers"]["mqtt"][1]["jsonKey"] = true;
    filter["thermometers"]["mqtt"][1]["role"] = true;
    filter["thermometers"]["ble"]["name"] = true;
    filter["thermometers"]["ble"]["id"] = true;
    filter["thermometers"]["ble"]["role"] = true;

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
    if (err) {
        // keep previous
        return;
    }

    JsonObject root = doc.as<JsonObject>();
    JsonObject t = root["thermometers"].as<JsonObject>();
    if (!t) {
        // keep defaults, but make sure at least initialized
        return;
    }

    JsonArray mqttArr = t["mqtt"].as<JsonArray>();
    for (uint8_t i = 0; i < 2; i++) {
        JsonObject o = mqttArr ? mqttArr[i].as<JsonObject>() : JsonObject();
        if (o) {
            s_mqtt[i].name   = String((const char*)(o["name"] | ""));
            s_mqtt[i].topic  = String((const char*)(o["topic"] | ""));
            s_mqtt[i].jsonKey= String((const char*)(o["jsonKey"] | "tempC"));
            s_mqtt[i].role   = thermoNormalizeRole(String((const char*)(o["role"] | "")));
        } else {
            // if missing, reset to defaults
            s_mqtt[i].name = "";
            s_mqtt[i].topic = "";
            s_mqtt[i].jsonKey = "tempC";
            s_mqtt[i].role = "";
        }
    }

    JsonObject bleObj = t["ble"].as<JsonObject>();
    if (bleObj) {
        s_ble.name = String((const char*)(bleObj["name"] | "BLE Meteo"));
        s_ble.id   = String((const char*)(bleObj["id"] | "meteo.tempC"));
        // If role is missing in older configs, keep default "outdoor".
        const String r = thermoNormalizeRole(String((const char*)(bleObj["role"] | "")));
        if (r.length()) s_ble.role = r;
    }
}

const MqttThermometerCfg& thermometersGetMqtt(uint8_t index) {
    if (index >= 2) index = 0;
    return s_mqtt[index];
}

const BleThermometerCfg& thermometersGetBle() {
    return s_ble;
}

uint8_t thermometersGetMqttSubscribeTopics(String* outTopics, uint8_t maxTopics) {
    uint8_t n = 0;
    auto add = [&](const String& t) {
        if (!t.length()) return;
        for (uint8_t i = 0; i < n && outTopics; i++) {
            if (outTopics[i] == t) return;
        }
        if (outTopics && n < maxTopics) outTopics[n] = t;
        n++;
    };

    add(s_mqtt[0].topic);
    add(s_mqtt[1].topic);

    if (!outTopics) return n;
    return (n > maxTopics) ? maxTopics : n;
}

void thermometersMqttOnMessage(const String& topic, const String& payload) {
    (void)topic;
    (void)payload;
    // Aktuálně se poslední MQTT payload drží v MqttController cache,
    // odkud se parsuje i v API (/api/dash) a v logice (Equitherm).
    // Zde je záměrně no-op pro budoucí rozšíření.
}

void thermometersBleOnReading(const String& id, float tempC) {
    (void)id;
    (void)tempC;
    // Aktuálně BLE hodnoty drží BleController (bleGetTempCById / bleGetMeteoTempC).
    // Zde je záměrně no-op pro budoucí rozšíření.
}
