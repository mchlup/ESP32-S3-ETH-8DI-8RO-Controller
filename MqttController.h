#pragma once
#include <Arduino.h>

// Inicializace MQTT (po WiFi)
void mqttInit();

// Volat v hlavní smyčce (neblokující)
void mqttLoop();

// Aplikace konfigurace z /config.json (celý JSON string)
void mqttApplyConfig(const String& json);

// Info o stavu MQTT
bool mqttIsConfigured();
bool mqttIsConnected();

// Publikace celého aktuálního stavu (pro HA po reconnectu)
void mqttPublishFullState();
// Republish Home Assistant discovery + full state (pokud je připojeno)
void mqttRepublishDiscovery();
// Vrátí poslední přijatou hodnotu pro daný topic (pokud existuje). Používá Rule engine (MQTT podmínky).
bool mqttGetLastValue(const String& topic, String* outValue);

// Jako mqttGetLastValue(), navíc vrací millis() timestamp posledního příjmu.
bool mqttGetLastValueInfo(const String& topic, String* outValue, uint32_t* outLastMs);