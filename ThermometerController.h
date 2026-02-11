#pragma once

#include <Arduino.h>

struct MqttThermometerCfg {
  String topic;
  String role;
  // Optional JSON key to extract temperature from payload.
  // If empty, parser will try to extract the first numeric value.
  String jsonKey;
};

struct BleThermometerCfg {
  String id;   // optional device id
  String role; // e.g. "outdoor"
};

void thermometersInit();

// Apply thermometer-related configuration from the whole /config.json payload.
// This updates in-memory BLE/MQTT thermometer roles immediately (no restart needed).
void thermometersApplyConfig(const String& json);

const MqttThermometerCfg& thermometersGetMqtt(uint8_t idx); // idx 0..1
const BleThermometerCfg&  thermometersGetBle();

// Subscribe topics required by thermometer config.
// If outTopics == nullptr, only returns the count.
uint8_t thermometersGetMqttSubscribeTopics(String* outTopics, uint8_t maxTopics);
