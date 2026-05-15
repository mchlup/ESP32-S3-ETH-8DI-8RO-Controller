#include "MqttController.h"

#include <WiFi.h>
#include <esp_system.h>
#include <mqtt_client.h>

#include "ConfigStore.h"
#include "NetworkController.h"
#include "RelayController.h"
#include "InputController.h"
#include "TemperatureManager.h"
#include "OpenThermController.h"
#include "BleController.h"
#include "OtaController.h"
#include "EquithermController.h"
#include "DhwController.h"
#include "Log.h"

namespace {
  struct RuntimeCfg {
    bool enabled = false;
    String host;
    uint16_t port = 1883;
    String username;
    String password;
    String clientId;
    String baseTopic;
    uint32_t publishIntervalMs = 10000;
    bool haEnabled = true;
    bool haDiscovery = true;
    String discoveryPrefix;
    String nodeId;
  };

  struct RuntimeState {
    bool inited = false;
    bool connected = false;
    bool discoveryPublished = false;
    bool subscribed = false;
    uint32_t lastConnectAttemptMs = 0;
    uint32_t lastPublishMs = 0;
    uint32_t lastDiscoveryMs = 0;
    uint32_t connectCount = 0;
    uint32_t disconnectCount = 0;
    int lastError = 0;
    String lastErrorText;
    String runtime = "disabled";
    String uri;
    String topicState;
    String topicAvailability;
    String topicCmdRoot;
    String topicInfo;
    esp_mqtt_client_handle_t client = nullptr;
  };

  RuntimeCfg s_cfg;
  RuntimeState s_st;

  static constexpr uint32_t kReconnectMinMs = 5000;

  static String copyMqttSpan(const char* data, int len) {
    if (!data || len <= 0) return String();
    String out;
    out.reserve((unsigned)len);
    for (int i = 0; i < len; ++i) out += data[i];
    return out;
  }

  static String normalizedTopic(const String& src) {
    String out = src;
    out.trim();
    while (out.startsWith("/")) out.remove(0, 1);
    while (out.endsWith("/")) out.remove(out.length() - 1);
    return out;
  }

  static String deviceName() {
    return String("ESP32 Controller");
  }

  static String mqttTopicState() { return s_st.topicState.length() ? s_st.topicState : (s_cfg.baseTopic + "/state"); }
  static String mqttTopicAvailability() { return s_st.topicAvailability.length() ? s_st.topicAvailability : (s_cfg.baseTopic + "/availability"); }
  static String mqttTopicCmdRoot() { return s_st.topicCmdRoot.length() ? s_st.topicCmdRoot : (s_cfg.baseTopic + "/cmd"); }
  static String mqttTopicInfo() { return s_st.topicInfo.length() ? s_st.topicInfo : (s_cfg.baseTopic + "/info"); }

  static String relayStateTemplate(uint8_t idx) {
    const uint8_t mask = (uint8_t)(1u << idx);
    return String("{{ 'ON' if (value_json.rel.mask | int(0)) & ") + String(mask) + String(" > 0 else 'OFF' }}");
  }

  static String inputStateTemplate(uint8_t idx) {
    const uint8_t mask = (uint8_t)(1u << idx);
    return String("{{ 'ON' if (value_json.in.actMask | int(0)) & ") + String(mask) + String(" > 0 else 'OFF' }}");
  }

  static const char* tempSourceText(TempSource s) {
    switch (s) {
      case TempSource::OpenTherm: return "opentherm";
      case TempSource::Dallas: return "dallas";
      case TempSource::Ble: return "ble";
      default: return "none";
    }
  }

  static void loadCfg() {
    s_cfg.enabled = ConfigStore::getMqttEnabled();
    s_cfg.host = ConfigStore::getMqttHost();
    s_cfg.port = ConfigStore::getMqttPort();
    s_cfg.username = ConfigStore::getMqttUsername();
    s_cfg.password = ConfigStore::getMqttPassword();
    s_cfg.clientId = ConfigStore::getMqttClientId();
    s_cfg.baseTopic = normalizedTopic(ConfigStore::getMqttBaseTopic());
    s_cfg.publishIntervalMs = ConfigStore::getMqttPublishIntervalMs();
    s_cfg.haEnabled = ConfigStore::getMqttHaEnabled();
    s_cfg.haDiscovery = ConfigStore::getMqttHaDiscovery();
    s_cfg.discoveryPrefix = normalizedTopic(ConfigStore::getMqttDiscoveryPrefix());
    s_cfg.nodeId = normalizedTopic(ConfigStore::getMqttNodeId());
    if (!s_cfg.clientId.length()) s_cfg.clientId = "esp32-controller";
    if (!s_cfg.baseTopic.length()) s_cfg.baseTopic = "esp32-controller";
    if (!s_cfg.discoveryPrefix.length()) s_cfg.discoveryPrefix = "homeassistant";
    if (!s_cfg.nodeId.length()) s_cfg.nodeId = "esp32_controller";
    if (!s_cfg.port) s_cfg.port = 1883;
    if (s_cfg.publishIntervalMs < 1000) s_cfg.publishIntervalMs = 1000;
    s_st.uri = String("mqtt://") + s_cfg.host + ":" + String(s_cfg.port);
    s_st.topicState = s_cfg.baseTopic + "/state";
    s_st.topicAvailability = s_cfg.baseTopic + "/availability";
    s_st.topicCmdRoot = s_cfg.baseTopic + "/cmd";
    s_st.topicInfo = s_cfg.baseTopic + "/info";
  }

  static void mqttStopClient() {
    if (s_st.client) {
      esp_mqtt_client_stop(s_st.client);
      esp_mqtt_client_destroy(s_st.client);
      s_st.client = nullptr;
    }
    s_st.connected = false;
    s_st.discoveryPublished = false;
    s_st.subscribed = false;
  }

  static void mqttNoteError(const String& msg, int code = 0) {
    s_st.lastError = code;
    s_st.lastErrorText = msg;
    if (msg.length()) LOGW("MQTT: %s", msg.c_str());
  }

  static bool publishRaw(const String& topic, const String& payload, bool retain = false, int qos = 0) {
    if (!s_st.client || !s_st.connected) return false;
    const int msgId = esp_mqtt_client_publish(s_st.client, topic.c_str(), payload.c_str(), payload.length(), qos, retain ? 1 : 0);
    return msgId >= 0;
  }

  static bool publishJson(const String& topic, DynamicJsonDocument& doc, bool retain = false, int qos = 0) {
    String payload;
    serializeJson(doc, payload);
    return publishRaw(topic, payload, retain, qos);
  }

  static void fillDevice(JsonObject device) {
    JsonArray ids = device.createNestedArray("identifiers");
    ids.add(s_cfg.nodeId);
    device["name"] = deviceName();
    device["manufacturer"] = "Waveshare";
    device["model"] = "ESP32-S3-ETH-8DI-8RO";
    device["sw_version"] = "mqtt-ha";
    device["configuration_url"] = String("http://") + networkGetIp();
  }

  static void fillTemps(JsonObject temps) {
    const auto putRole = [&](const char* key, TempRole role) {
      TempValue tv = TemperatureManager::get(role, 600000);
      if (tv.valid && isfinite(tv.c)) temps[key] = tv.c;
      else temps[key] = nullptr;
      temps[String(key) + "Src"] = tempSourceText(tv.src);
      temps[String(key) + "AgeMs"] = tv.valid ? tv.ageMs : 0;
    };
    putRole("outside", TempRole::Outside);
    putRole("flow", TempRole::Flow);
    putRole("return", TempRole::Return);
    putRole("dhw_tank", TempRole::DhwTank);
    putRole("tank_top", TempRole::TankTop);
    putRole("tank_mid", TempRole::TankMid);
    putRole("tank_bottom", TempRole::TankBottom);
    putRole("dhw_return", TempRole::DhwReturn);
  }

  static String buildStateJson() {
    DynamicJsonDocument doc(4096);
    doc["ms"] = (uint32_t)millis();
    doc["wifi"] = networkIsWifiConnected();
    doc["eth"] = networkIsEthernetConnected();
    doc["ip"] = networkGetIp();
    JsonObject temps = doc.createNestedObject("temps");
    fillTemps(temps);

    JsonObject rel = doc.createNestedObject("rel");
    rel["mask"] = relayGetMask();
    rel["ok"] = relayIsOk();

    JsonObject in = doc.createNestedObject("in");
    uint8_t actMask = 0;
    for (uint8_t i = 0; i < 8; i++) if (inputGetState((InputId)i)) actMask |= (uint8_t)(1u << i);
    in["actMask"] = actMask;

    JsonObject ot = doc.createNestedObject("ot");
    openthermFillFastJson(ot);

    JsonObject ble = doc.createNestedObject("ble");
    bleFillFastJson(ble);

    JsonObject ota = doc.createNestedObject("ota");
    otaFillFastJson(ota);

    JsonObject time = doc.createNestedObject("time");
    time["valid"] = networkIsTimeValid();
    if (networkIsTimeValid()) time["iso"] = networkGetTimeIso(); else time["iso"] = nullptr;
    time["src"] = networkGetTimeSource();

    JsonObject eq = doc.createNestedObject("eq");
    equithermFillFastJson(eq);

    JsonObject dhw = doc.createNestedObject("dhw");
    dhwFillFastJson(dhw);

    String out;
    serializeJson(doc, out);
    return out;
  }

  static void publishAvailability(const char* payload) {
    publishRaw(mqttTopicAvailability(), String(payload), true, 1);
  }

  static void publishDiscoveryEntity(const char* component, const char* objectId, DynamicJsonDocument& doc) {
    String topic = s_cfg.discoveryPrefix + "/" + component + "/" + s_cfg.nodeId + "/" + objectId + "/config";
    publishJson(topic, doc, true, 1);
  }

  static void addCommonEntityFields(DynamicJsonDocument& doc, const char* name, const char* uniqSuffix) {
    JsonObject root = doc.as<JsonObject>();
    root["name"] = name;
    root["uniq_id"] = s_cfg.nodeId + String("_") + uniqSuffix;
    root["stat_t"] = mqttTopicState();
    root["avty_t"] = mqttTopicAvailability();
    root["pl_avail"] = "online";
    root["pl_not_avail"] = "offline";
    JsonObject device = root.createNestedObject("dev");
    fillDevice(device);
  }

  static void publishDiscovery() {
    if (!s_st.connected || !s_cfg.haEnabled || !s_cfg.haDiscovery) return;

    // Sensors
    struct SensorDef {
      const char* objectId;
      const char* name;
      const char* valueTpl;
      const char* unit;
      const char* deviceClass;
      const char* stateClass;
      const char* icon;
    };
    static const SensorDef sensors[] = {
      {"outside_temp", "Venkovni teplota", "{{ value_json.temps.outside }}", "°C", "temperature", "measurement", nullptr},
      {"flow_temp", "Topna voda", "{{ value_json.temps.flow }}", "°C", "temperature", "measurement", nullptr},
      {"return_temp", "Zpatecka", "{{ value_json.temps.return }}", "°C", "temperature", "measurement", nullptr},
      {"dhw_temp", "TUV", "{{ value_json.temps.dhw_tank }}", "°C", "temperature", "measurement", nullptr},
      {"tank_top_temp", "AKU nahore", "{{ value_json.temps.tank_top }}", "°C", "temperature", "measurement", nullptr},
      {"tank_mid_temp", "AKU uprostred", "{{ value_json.temps.tank_mid }}", "°C", "temperature", "measurement", nullptr},
      {"tank_bottom_temp", "AKU dole", "{{ value_json.temps.tank_bottom }}", "°C", "temperature", "measurement", nullptr},
      {"mix_position", "Smesovaci ventil", "{{ value_json.eq.mix.pct }}", "%", nullptr, "measurement", "mdi:valve"},
      {"boiler_pressure", "Tlak systemu", "{{ value_json.ot.pr }}", "bar", "pressure", "measurement", nullptr},
      {"boiler_setpoint", "Pozadovana CH", "{{ value_json.ot.cs }}", "°C", "temperature", "measurement", nullptr},
    };
    for (const auto& def : sensors) {
      DynamicJsonDocument doc(1024);
      addCommonEntityFields(doc, def.name, def.objectId);
      JsonObject root = doc.as<JsonObject>();
      root["val_tpl"] = def.valueTpl;
      if (def.unit) root["unit_of_meas"] = def.unit;
      if (def.deviceClass) root["dev_cla"] = def.deviceClass;
      if (def.stateClass) root["stat_cla"] = def.stateClass;
      if (def.icon) root["icon"] = def.icon;
      publishDiscoveryEntity("sensor", def.objectId, doc);
    }

    // Relay switches
    for (uint8_t i = 0; i < 8; i++) {
      DynamicJsonDocument doc(1152);
      const String objectId = String("relay_") + String(i + 1);
      const String name = String("Relé ") + String(i + 1);
      addCommonEntityFields(doc, name.c_str(), objectId.c_str());
      JsonObject root = doc.as<JsonObject>();
      root["cmd_t"] = mqttTopicCmdRoot() + "/relay/" + String(i + 1) + "/set";
      root["val_tpl"] = relayStateTemplate(i);
      root["pl_on"] = "ON";
      root["pl_off"] = "OFF";
      root["stat_on"] = "ON";
      root["stat_off"] = "OFF";
      root["icon"] = "mdi:electric-switch";
      publishDiscoveryEntity("switch", objectId.c_str(), doc);
    }

    // Inputs as binary sensors
    static const char* inputNames[3] = {"Vstup Den/Noc", "Vstup TUV", "Vstup cirkulace"};
    for (uint8_t i = 0; i < 3; i++) {
      DynamicJsonDocument doc(1024);
      const String objectId = String("input_") + String(i + 1);
      addCommonEntityFields(doc, inputNames[i], objectId.c_str());
      JsonObject root = doc.as<JsonObject>();
      root["val_tpl"] = inputStateTemplate(i);
      root["pl_on"] = "ON";
      root["pl_off"] = "OFF";
      publishDiscoveryEntity("binary_sensor", objectId.c_str(), doc);
    }

    // Mode select
    {
      DynamicJsonDocument doc(1024);
      addCommonEntityFields(doc, "Ekviterm mode", "equitherm_mode");
      JsonObject root = doc.as<JsonObject>();
      root["cmd_t"] = mqttTopicCmdRoot() + "/equitherm/mode/set";
      root["val_tpl"] = "{{ value_json.eq.me if value_json.eq.me is defined and value_json.eq.me else value_json.eq.m }}";
      JsonArray opts = root.createNestedArray("options");
      opts.add("auto");
      opts.add("day");
      opts.add("night");
      root["icon"] = "mdi:radiator";
      publishDiscoveryEntity("select", "equitherm_mode", doc);
    }

    // DHW + circulation switches
    {
      DynamicJsonDocument doc(1024);
      addCommonEntityFields(doc, "Ohrev TUV", "dhw_heat");
      JsonObject root = doc.as<JsonObject>();
      root["cmd_t"] = mqttTopicCmdRoot() + "/dhw/heat/set";
      root["val_tpl"] = "{{ 'ON' if value_json.dhw.ha else 'OFF' }}";
      root["pl_on"] = "ON";
      root["pl_off"] = "OFF";
      root["stat_on"] = "ON";
      root["stat_off"] = "OFF";
      root["icon"] = "mdi:water-boiler";
      publishDiscoveryEntity("switch", "dhw_heat", doc);
    }
    {
      DynamicJsonDocument doc(1024);
      addCommonEntityFields(doc, "Cirkulace TUV", "dhw_circ");
      JsonObject root = doc.as<JsonObject>();
      root["cmd_t"] = mqttTopicCmdRoot() + "/dhw/circ/set";
      root["val_tpl"] = "{{ 'ON' if value_json.dhw.ca else 'OFF' }}";
      root["pl_on"] = "ON";
      root["pl_off"] = "OFF";
      root["stat_on"] = "ON";
      root["stat_off"] = "OFF";
      root["icon"] = "mdi:pump";
      publishDiscoveryEntity("switch", "dhw_circ", doc);
    }

    s_st.discoveryPublished = true;
    s_st.lastDiscoveryMs = millis();
  }

  static void publishInfo() {
    DynamicJsonDocument doc(768);
    doc["nodeId"] = s_cfg.nodeId;
    doc["clientId"] = s_cfg.clientId;
    doc["ip"] = networkGetIp();
    doc["wifi"] = networkIsWifiConnected();
    doc["eth"] = networkIsEthernetConnected();
    doc["haEnabled"] = s_cfg.haEnabled;
    doc["haDiscovery"] = s_cfg.haDiscovery;
    publishJson(mqttTopicInfo(), doc, true, 1);
  }

  static void publishState(bool force = false) {
    if (!s_st.connected) return;
    const uint32_t now = millis();
    if (!force && (uint32_t)(now - s_st.lastPublishMs) < s_cfg.publishIntervalMs) return;
    s_st.lastPublishMs = now;
    publishRaw(mqttTopicState(), buildStateJson(), true, 0);
  }

  static void handleCmdRelay(const String& suffix, const String& payload) {
    if (!suffix.startsWith("relay/")) return;
    int slash = suffix.indexOf('/', 6);
    String idxTxt = (slash >= 0) ? suffix.substring(6, slash) : suffix.substring(6);
    const int relayNum = idxTxt.toInt();
    if (relayNum < 1 || relayNum > 8) return;
    String p = payload; p.trim(); p.toUpperCase();
    if (p == "TOGGLE") relayToggle((RelayId)(relayNum - 1));
    else if (p == "ON" || p == "1" || p == "TRUE") relaySet((RelayId)(relayNum - 1), true);
    else if (p == "OFF" || p == "0" || p == "FALSE") relaySet((RelayId)(relayNum - 1), false);
  }

  static void handleCmdEquitherm(const String& suffix, const String& payload) {
    if (suffix != "equitherm/mode/set") return;
    String p = payload; p.trim(); p.toLowerCase();
    if (p != "auto" && p != "day" && p != "night") return;
    String err;
    DynamicJsonDocument doc(128);
    doc["mode"] = p;
    String js; serializeJson(doc, js);
    equithermHandleCmdJson(js, err);
  }

  static void handleCmdDhw(const String& suffix, const String& payload) {
    String p = payload; p.trim(); p.toUpperCase();
    String js;
    if (suffix == "dhw/heat/set") {
      if (p == "BOOST") js = String("{\"heatActive\":true,\"boostMin\":15}");
      else if (p == "ON" || p == "1" || p == "TRUE") js = String("{\"heatActive\":true}");
      else if (p == "OFF" || p == "0" || p == "FALSE") js = String("{\"heatActive\":false}");
    } else if (suffix == "dhw/circ/set") {
      if (p == "ON" || p == "1" || p == "TRUE") js = String("{\"circActive\":true}");
      else if (p == "OFF" || p == "0" || p == "FALSE") js = String("{\"circActive\":false}");
    }
    if (!js.length()) return;
    String err;
    dhwHandleCmdJson(js, err);
  }

  static void handleCmdMix(const String& suffix, const String& payload) {
    if (suffix != "mix/pulse/set") return;
    String p = payload; p.trim(); p.toUpperCase();
    StaticJsonDocument<160> doc;
    if (p == "A" || p == "OPEN") doc["mixPulse"] = "a";
    else if (p == "B" || p == "CLOSE") doc["mixPulse"] = "b";
    else if (p == "STOP") doc["mixPulse"] = "stop";
    else if (p == "A_END") doc["mixMove"] = "a_end";
    else if (p == "B_END") doc["mixMove"] = "b_end";
    else return;
    String js; serializeJson(doc, js);
    String err;
    equithermHandleCmdJson(js, err);
  }

  static void mqttHandlePayload(const char* topicC, const char* data, int len) {
    String topic(topicC ? topicC : "");
    String payload;
    for (int i = 0; i < len; i++) payload += data[i];
    const String prefix = mqttTopicCmdRoot() + "/";
    if (!topic.startsWith(prefix)) return;
    const String suffix = topic.substring(prefix.length());
    handleCmdRelay(suffix, payload);
    handleCmdEquitherm(suffix, payload);
    handleCmdDhw(suffix, payload);
    handleCmdMix(suffix, payload);
    publishState(true);
  }

  static void mqttSubscribeCommands() {
    if (!s_st.client || !s_st.connected || s_st.subscribed) return;
    const String root = mqttTopicCmdRoot() + "/#";
    esp_mqtt_client_subscribe(s_st.client, root.c_str(), 1);
    s_st.subscribed = true;
  }

  static void mqttStartClient() {
    mqttStopClient();
    if (!s_cfg.enabled) {
      s_st.runtime = "disabled";
      return;
    }
    if (!networkIsConnected()) {
      s_st.runtime = "waiting_network";
      return;
    }
    if (!s_cfg.host.length()) {
      s_st.runtime = "missing_host";
      return;
    }

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = s_st.uri.c_str();
    cfg.credentials.username = s_cfg.username.length() ? s_cfg.username.c_str() : nullptr;
    cfg.credentials.authentication.password = s_cfg.password.length() ? s_cfg.password.c_str() : nullptr;
    cfg.credentials.client_id = s_cfg.clientId.c_str();
    cfg.session.keepalive = 30;
    cfg.session.last_will.topic = s_st.topicAvailability.c_str();
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;

    s_st.client = esp_mqtt_client_init(&cfg);
    if (!s_st.client) {
      mqttNoteError("init failed");
      s_st.runtime = "init_failed";
      return;
    }
    esp_mqtt_client_register_event(s_st.client, MQTT_EVENT_ANY, [](void*, esp_event_base_t, int32_t event_id, void* event_data) {
      esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
      switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
          s_st.connected = true;
          s_st.connectCount++;
          s_st.runtime = "connected";
          s_st.discoveryPublished = false;
          s_st.subscribed = false;
          s_st.lastError = 0;
          s_st.lastErrorText = "";
          publishAvailability("online");
          mqttSubscribeCommands();
          publishInfo();
          publishState(true);
          publishDiscovery();
          break;
        case MQTT_EVENT_DISCONNECTED:
          s_st.connected = false;
          s_st.disconnectCount++;
          s_st.runtime = "disconnected";
          break;
        case MQTT_EVENT_ERROR:
          s_st.connected = false;
          s_st.runtime = "error";
          mqttNoteError("event error", event && event->error_handle ? event->error_handle->error_type : -1);
          break;
        case MQTT_EVENT_DATA: {
          const String topic = copyMqttSpan(event ? event->topic : nullptr, event ? event->topic_len : 0);
          const String payload = copyMqttSpan(event ? event->data : nullptr, event ? event->data_len : 0);
          mqttHandlePayload(topic.c_str(), payload.c_str(), payload.length());
          break;
        }
        default:
          break;
      }
    }, nullptr);

    const esp_err_t err = esp_mqtt_client_start(s_st.client);
    if (err != ESP_OK) {
      mqttNoteError(String("start failed ") + String((int)err), (int)err);
      s_st.runtime = "start_failed";
      mqttStopClient();
      return;
    }
    s_st.lastConnectAttemptMs = millis();
    s_st.runtime = "connecting";
  }
}

void mqttInit() {
  if (s_st.inited) return;
  s_st.inited = true;
  loadCfg();
  mqttStartClient();
}

void mqttLoop() {
  mqttInit();
  loadCfg();
  if (!s_cfg.enabled) {
    mqttStopClient();
    s_st.runtime = "disabled";
    return;
  }
  if (!networkIsConnected()) {
    if (s_st.connected) publishAvailability("offline");
    mqttStopClient();
    s_st.runtime = "waiting_network";
    return;
  }
  if (!s_st.client) {
    const uint32_t now = millis();
    if ((uint32_t)(now - s_st.lastConnectAttemptMs) >= kReconnectMinMs) mqttStartClient();
    return;
  }
  if (s_st.connected) {
    mqttSubscribeCommands();
    if (!s_st.discoveryPublished) publishDiscovery();
    publishState(false);
  }
}

void mqttApplyConfig(const String& json) {
  (void)json;
  loadCfg();
  mqttStopClient();
  s_st.lastConnectAttemptMs = 0;
  mqttStartClient();
}

bool mqttIsConnected() {
  return s_st.connected;
}

void mqttForcePublish() {
  publishState(true);
}

void mqttFillStatusJson(JsonObject& mqtt, bool includePreview) {
  loadCfg();
  mqtt["enabled"] = s_cfg.enabled;
  mqtt["connected"] = s_st.connected;
  mqtt["implemented"] = true;
  mqtt["runtime"] = s_st.runtime;
  mqtt["host"] = s_cfg.host;
  mqtt["port"] = (uint32_t)s_cfg.port;
  mqtt["username"] = s_cfg.username;
  mqtt["passwordSet"] = s_cfg.password.length() > 0;
  mqtt["clientId"] = s_cfg.clientId;
  mqtt["baseTopic"] = s_cfg.baseTopic;
  mqtt["publishIntervalMs"] = (uint32_t)s_cfg.publishIntervalMs;
  mqtt["stateTopic"] = mqttTopicState();
  mqtt["availabilityTopic"] = mqttTopicAvailability();
  mqtt["connectCount"] = (uint32_t)s_st.connectCount;
  mqtt["disconnectCount"] = (uint32_t)s_st.disconnectCount;
  mqtt["lastError"] = s_st.lastError;
  if (s_st.lastErrorText.length()) mqtt["lastErrorText"] = s_st.lastErrorText; else mqtt["lastErrorText"] = nullptr;
  mqtt["discoveryPublished"] = s_st.discoveryPublished;
  mqtt["lastPublishMs"] = (uint32_t)s_st.lastPublishMs;
  JsonObject ha = mqtt.createNestedObject("homeAssistant");
  ha["enabled"] = s_cfg.haEnabled;
  ha["discovery"] = s_cfg.haDiscovery;
  ha["discoveryPrefix"] = s_cfg.discoveryPrefix;
  ha["nodeId"] = s_cfg.nodeId;

  if (includePreview) {
    JsonObject preview = mqtt.createNestedObject("preview");
    preview["stateTopic"] = mqttTopicState();
    preview["availabilityTopic"] = mqttTopicAvailability();
    preview["commandRoot"] = mqttTopicCmdRoot();
    preview["discoveryPrefix"] = s_cfg.discoveryPrefix;
    JsonObject device = preview.createNestedObject("device");
    fillDevice(device);
    JsonArray entities = preview.createNestedArray("entities");
    const char* simpleEntities[] = {
      "sensor/outside_temp", "sensor/flow_temp", "sensor/return_temp", "sensor/dhw_temp",
      "sensor/tank_top_temp", "sensor/tank_mid_temp", "sensor/tank_bottom_temp", "sensor/mix_position",
      "switch/relay_1", "switch/relay_2", "switch/relay_3", "switch/relay_4",
      "switch/relay_5", "switch/relay_6", "switch/relay_7", "switch/relay_8",
      "binary_sensor/input_1", "binary_sensor/input_2", "binary_sensor/input_3",
      "switch/dhw_heat", "switch/dhw_circ", "select/equitherm_mode"
    };
    for (const char* e : simpleEntities) entities.add(e);
  }
}

String mqttGetStatusJson() {
  DynamicJsonDocument doc(8192);
  doc["ok"] = true;
  JsonObject mqtt = doc.createNestedObject("mqtt");
  mqttFillStatusJson(mqtt, true);
  String out;
  serializeJson(doc, out);
  return out;
}
