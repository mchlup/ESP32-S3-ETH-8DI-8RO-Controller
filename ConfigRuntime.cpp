#include "ConfigRuntime.h"

#include <ArduinoJson.h>

#include "BleController.h"
#include "ConfigStore.h"
#include "DallasController.h"
#include "DhwController.h"
#include "EquithermController.h"
#include "MqttController.h"
#include "NetworkController.h"
#include "OpenThermController.h"
#include "OtaController.h"
#include "PressureAlarmController.h"

namespace {
  String buildOpenThermConfigJson() {
    DynamicJsonDocument doc(512);
    JsonObject ot = doc.createNestedObject("opentherm");
    ot["enabled"] = ConfigStore::getOtEnabled();
    ot["autoStart"] = ConfigStore::getOtAutoStart();
    ot["pollMs"] = ConfigStore::getOtPollMs();
    ot["bootDelayMs"] = ConfigStore::getOtBootDelayMs();
    const String mode = ConfigStore::getOtMode();
    ot["mode"] = mode;
    ot["allowRawWrite"] = ConfigStore::getOtAllowRawWrite();
    ot["boilerControl"] = mode == "control" ? "opentherm" : "relay";
    String out;
    serializeJson(doc, out);
    return out;
  }

  String buildBleConfigJson() {
    DynamicJsonDocument doc(256);
    doc["enabled"] = ConfigStore::getBleEnabled();
    doc["namePrefix"] = ConfigStore::getBleNamePrefix();
    doc["scanIntervalMs"] = ConfigStore::getBleScanIntervalMs();
    String out;
    serializeJson(doc, out);
    return out;
  }

  String buildTimeConfigJson() {
    DynamicJsonDocument doc(256);
    JsonObject t = doc.createNestedObject("time");
    t["enabled"] = ConfigStore::getTimeEnabled();
    t["tz"] = ConfigStore::getTimeTz();
    JsonArray ntp = t.createNestedArray("ntp");
    ntp.add(ConfigStore::getTimeNtp1());
    ntp.add(ConfigStore::getTimeNtp2());
    ntp.add(ConfigStore::getTimeNtp3());
    String out;
    serializeJson(doc, out);
    return out;
  }

  String buildOtaConfigJson() {
    DynamicJsonDocument doc(256);
    doc["enabled"] = ConfigStore::getOtaEnabled();
    doc["hostname"] = ConfigStore::getOtaHostname();
    doc["port"] = ConfigStore::getOtaPort();
    doc["password"] = ConfigStore::getOtaPassword();
    String out;
    serializeJson(doc, out);
    return out;
  }
}

namespace ConfigRuntime {
  void loadAllFromStore() {
    ConfigStore::begin();
  }

  void applyAllRuntime() {
    openthermApplyConfig(buildOpenThermConfigJson());
    bleApplyConfig(buildBleConfigJson());
    networkApplyConfig(buildTimeConfigJson());
    otaApplyConfig(buildOtaConfigJson());
    dallasApplyConfig(String("{\"enabled\":") + (ConfigStore::getDallasEnabled() ? "true}" : "false}"));
    equithermReloadFromStore();
    dhwReloadFromStore();
    mqttApplyConfig(String());
    pressureAlarmReloadFromStore();
  }
}
