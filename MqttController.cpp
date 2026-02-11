// Feature gate first (keeps PubSubClient/WiFi out of the build when disabled).
#include "Features.h"

#include "MqttController.h"

#if defined(FEATURE_MQTT)

#include "NetworkController.h"
#include "FsController.h"
#include "Log.h"

#include "LogicController.h"
#include "ThermometerController.h"

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

namespace {
  WiFiClient g_netClient;
  PubSubClient g_mqtt(g_netClient);

  String g_host = "";
  uint16_t g_port = 1883;
  String g_user = "";
  String g_pass = "";
  String g_clientId = "esp32-heat";
  String g_baseTopic = "esp32/heat";

  uint32_t g_nextReconnectMs = 0;

  struct LastValue {
    String topic;
    String payload;
    uint32_t lastMs = 0;
  };
  LastValue g_last[16];

  void cacheValue(const String& topic, const String& payload) {
    uint8_t empty = 255;
    for (uint8_t i=0;i<16;i++) {
      if (g_last[i].topic == topic) {
        g_last[i].payload = payload;
        g_last[i].lastMs = millis();
        return;
      }
      if (!g_last[i].topic.length() && empty == 255) empty = i;
    }
    if (empty != 255) {
      g_last[empty].topic = topic;
      g_last[empty].payload = payload;
      g_last[empty].lastMs = millis();
    }
  }

  void onMsg(char* topic, byte* payload, unsigned int len) {
    String t(topic ? topic : "");
    String p;
    p.reserve(len);
    for (unsigned int i=0;i<len;i++) p += (char)payload[i];
    cacheValue(t, p);
  }

  void loadConfig() {
    String json;
    if (!fsReadTextFile("/mqtt.json", json)) {
      // also allow config.json
      fsReadTextFile("/config.json", json);
    }
    if (!json.length()) return;
    StaticJsonDocument<2048> doc;
    if (deserializeJson(doc, json)) return;
    JsonObject m = doc.containsKey("mqtt") ? doc["mqtt"].as<JsonObject>() : doc.as<JsonObject>();
    g_host = String((const char*)(m["host"] | g_host.c_str()));
    g_port = (uint16_t)(m["port"] | g_port);
    g_user = String((const char*)(m["user"] | g_user.c_str()));
    g_pass = String((const char*)(m["pass"] | g_pass.c_str()));
    g_clientId = String((const char*)(m["clientId"] | g_clientId.c_str()));
    g_baseTopic = String((const char*)(m["baseTopic"] | g_baseTopic.c_str()));
  }

  void subscribeAll() {
    String topics[8];
    uint8_t cnt = logicGetMqttSubscribeTopics(topics, 8);
    // add thermometer mqtt topics (can overlap)
    String t2[2];
    uint8_t cnt2 = thermometersGetMqttSubscribeTopics(t2, 2);
    for (uint8_t i=0;i<cnt2 && cnt<8;i++) topics[cnt++] = t2[i];

    for (uint8_t i=0;i<cnt;i++) {
      if (!topics[i].length()) continue;
      g_mqtt.subscribe(topics[i].c_str());
      LOGI("MQTT subscribe: %s", topics[i].c_str());
    }
  }
}

void mqttInit() {
  loadConfig();
  g_mqtt.setCallback(onMsg);
  if (g_host.length()) {
    g_mqtt.setServer(g_host.c_str(), g_port);
  }
}

bool mqttPublish(const String& topic, const String& payload, bool retain) {
  if (!g_mqtt.connected()) return false;
  return g_mqtt.publish(topic.c_str(), payload.c_str(), retain);
}

bool mqttGetLastValueInfo(const String& topic, String* outPayload, uint32_t* outLastMs) {
  for (uint8_t i=0;i<16;i++) {
    if (g_last[i].topic == topic) {
      if (outPayload) *outPayload = g_last[i].payload;
      if (outLastMs) *outLastMs = g_last[i].lastMs;
      return true;
    }
  }
  return false;
}

void mqttLoop() {
  // Allow build even without config
  if (!g_host.length()) return;

  if (!networkIsConnected()) {
    if (g_mqtt.connected()) g_mqtt.disconnect();
    return;
  }

  if (!g_mqtt.connected()) {
    const uint32_t now = millis();
    if ((int32_t)(now - g_nextReconnectMs) < 0) return;
    g_nextReconnectMs = now + 5000;

    LOGI("MQTT connecting to %s:%u...", g_host.c_str(), g_port);
    bool ok;
    if (g_user.length()) ok = g_mqtt.connect(g_clientId.c_str(), g_user.c_str(), g_pass.c_str());
    else ok = g_mqtt.connect(g_clientId.c_str());
    if (ok) {
      LOGI("MQTT connected");
      subscribeAll();
    } else {
      LOGW("MQTT connect failed, state=%d", g_mqtt.state());
    }
    return;
  }

  g_mqtt.loop();
}

#endif // FEATURE_MQTT
