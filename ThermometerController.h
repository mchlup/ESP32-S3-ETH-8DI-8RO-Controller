#pragma once

#include <Arduino.h>

// Konfigurace a stav "virtuálních" teploměrů (MQTT/BLE), které jsou v UI
// zobrazované společně s Dallas teploměry.

struct MqttThermometerCfg {
    String name;
    String topic;
    String jsonKey;
    String role; // canonical role key (outdoor/flow/return/dhw/aku_*)
};

struct BleThermometerCfg {
    String name;
    String id;   // např. "meteo.tempC"
    String role; // canonical role key
};

void thermometersInit();

// Aplikuje konfiguraci z celého config.json (string JSON)
void thermometersApplyConfig(const String& json);

// Konfigurace
const MqttThermometerCfg& thermometersGetMqtt(uint8_t index); // 0..1
const BleThermometerCfg&  thermometersGetBle();

// MQTT: seznam topiců k odběru (z konfigurace MQTT teploměrů)
uint8_t thermometersGetMqttSubscribeTopics(String* outTopics, uint8_t maxTopics);

// MQTT: callback z MqttController (uživatelské MQTT teploměry)
// Pozn.: Aktuálně MqttController už ukládá hodnoty do interní cache pro
// mqttGetLastValue*(), takže tato funkce je dnes best-effort/no-op a slouží
// hlavně pro budoucí rozšíření (validace, mapování, expiry).
void thermometersMqttOnMessage(const String& topic, const String& payload);

// BLE: callback z BleController při přijetí nové hodnoty.
// Aktuálně BLE hodnoty drží BleController (bleGetTempCById), takže je to
// best-effort/no-op pro budoucí rozšíření.
void thermometersBleOnReading(const String& id, float tempC);
