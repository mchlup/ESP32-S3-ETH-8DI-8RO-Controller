#include "ThermometerController.h"

#include "FsController.h"
#include "Log.h"
#include "ThermoRoles.h"

#include <ArduinoJson.h>

namespace {
  MqttThermometerCfg g_mqtt[2];
  BleThermometerCfg g_ble;
  bool g_inited = false;

  static void normalizeCfg() {
    // BLE role defaults to outdoor (project requirement).
    g_ble.role = thermoNormalizeRole(g_ble.role);
    if (!g_ble.role.length()) g_ble.role = "outdoor";

    for (uint8_t i = 0; i < 2; i++) {
      g_mqtt[i].role = thermoNormalizeRole(g_mqtt[i].role);
      g_mqtt[i].topic.trim();
      g_mqtt[i].jsonKey.trim();
    }
  }

  void loadFromConfigJson() {
    String json;
    if (!fsReadTextFile("/config.json", json)) return;

    StaticJsonDocument<4096> doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
      LOGW("thermometersInit: config.json parse failed: %s", err.c_str());
      return;
    }

    // BLE
    if (doc.containsKey("bleThermometer") && doc["bleThermometer"].is<JsonObject>()) {
      JsonObject o = doc["bleThermometer"].as<JsonObject>();
      g_ble.id = String((const char*)(o["id"] | ""));
      g_ble.role = String((const char*)(o["role"] | ""));
    } else {
      // Backward compatibility keys
      g_ble.id = String((const char*)(doc["bleId"] | ""));
      g_ble.role = String((const char*)(doc["bleRole"] | ""));
    }

    // MQTT thermometers: array of 2
    if (doc.containsKey("mqttThermometers") && doc["mqttThermometers"].is<JsonArray>()) {
      JsonArray a = doc["mqttThermometers"].as<JsonArray>();
      for (uint8_t i=0;i<2 && i<a.size();i++) {
        JsonObject o = a[i].as<JsonObject>();
        g_mqtt[i].topic = String((const char*)(o["topic"] | ""));
        g_mqtt[i].role  = String((const char*)(o["role"] | ""));
        g_mqtt[i].jsonKey = String((const char*)(o["jsonKey"] | ""));
      }
    } else {
      // Legacy keys
      for (uint8_t i=0;i<2;i++) {
        String kT = String("mqttTopic") + String(i+1);
        String kR = String("mqttRole") + String(i+1);
        String kJ = String("mqttJsonKey") + String(i+1);
        g_mqtt[i].topic = String((const char*)(doc[kT] | ""));
        g_mqtt[i].role  = String((const char*)(doc[kR] | ""));
        g_mqtt[i].jsonKey = String((const char*)(doc[kJ] | ""));
      }
    }

    normalizeCfg();
  }

  static void loadFromString(const String& json) {
    StaticJsonDocument<4096> doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
      LOGW("thermometersApplyConfig: JSON parse failed: %s", err.c_str());
      return;
    }

    // BLE
    if (doc.containsKey("bleThermometer") && doc["bleThermometer"].is<JsonObject>()) {
      JsonObject o = doc["bleThermometer"].as<JsonObject>();
      g_ble.id = String((const char*)(o["id"] | ""));
      g_ble.role = String((const char*)(o["role"] | ""));
    } else {
      // Backward compatibility keys
      g_ble.id = String((const char*)(doc["bleId"] | ""));
      g_ble.role = String((const char*)(doc["bleRole"] | ""));
    }

    // MQTT thermometers: array of 2
    if (doc.containsKey("mqttThermometers") && doc["mqttThermometers"].is<JsonArray>()) {
      JsonArray a = doc["mqttThermometers"].as<JsonArray>();
      for (uint8_t i=0;i<2 && i<a.size();i++) {
        JsonObject o = a[i].as<JsonObject>();
        g_mqtt[i].topic = String((const char*)(o["topic"] | ""));
        g_mqtt[i].role  = String((const char*)(o["role"] | ""));
        g_mqtt[i].jsonKey = String((const char*)(o["jsonKey"] | ""));
      }
    } else {
      // Legacy keys
      for (uint8_t i=0;i<2;i++) {
        String kT = String("mqttTopic") + String(i+1);
        String kR = String("mqttRole") + String(i+1);
        String kJ = String("mqttJsonKey") + String(i+1);
        g_mqtt[i].topic = String((const char*)(doc[kT] | ""));
        g_mqtt[i].role  = String((const char*)(doc[kR] | ""));
        g_mqtt[i].jsonKey = String((const char*)(doc[kJ] | ""));
      }
    }

    normalizeCfg();
  }
}

void thermometersInit() {
  if (g_inited) return;
  g_inited = true;
  // defaults
  g_mqtt[0] = {};
  g_mqtt[1] = {};
  g_ble = {};
  loadFromConfigJson();
  normalizeCfg();
}

void thermometersApplyConfig(const String& json) {
  // Allow calling even before thermometersInit().
  if (!g_inited) {
    g_inited = true;
    g_mqtt[0] = {};
    g_mqtt[1] = {};
    g_ble = {};
  }
  loadFromString(json);
}

const MqttThermometerCfg& thermometersGetMqtt(uint8_t idx) {
  if (idx >= 2) idx = 0;
  return g_mqtt[idx];
}

const BleThermometerCfg& thermometersGetBle() {
  return g_ble;
}

uint8_t thermometersGetMqttSubscribeTopics(String* outTopics, uint8_t maxTopics) {
  uint8_t cnt = 0;
  for (uint8_t i=0;i<2;i++) {
    if (!g_mqtt[i].topic.length()) continue;
    if (outTopics && cnt < maxTopics) outTopics[cnt] = g_mqtt[i].topic;
    cnt++;
  }
  return cnt;
}
