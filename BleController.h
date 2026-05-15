#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// BLE meteo client for ESP32C3_BLE_Sensor (ESP-Meteostanice-Outdoor)
// Service UUID: 7b7c1001-3a2b-4f2a-8bb0-8d2c2c1a1001
// Char UUID:    7b7c1002-3a2b-4f2a-8bb0-8d2c2c1a1001 (notify)
// Payload 6B (LE): int16 temp_x10, uint8 hum, uint16 press_hPa, int8 trend

struct BleConfig {
  bool enabled = true;
  String namePrefix = "ESP-Meteostanice"; // matches sensor requirement
  uint32_t scanIntervalMs = 10000;
  uint32_t reconnectBackoffMs = 5000;
};

struct BleMeteoData {
  bool valid = false;
  float tempC = NAN;
  int   humidityPct = -1;
  float pressureHpa = NAN;
  int   trend = 0;
  uint32_t lastUpdateMs = 0;
};

struct BleStatus {
  bool enabled = false;
  bool scanning = false;
  bool connected = false;
  String peer;
  uint32_t lastUpdateMs = 0;
  uint32_t lastConnectAttemptMs = 0;
  String lastError;
};

void bleInit();
void bleLoop();
void bleApplyConfig(const String& json);

BleConfig bleGetConfig();
BleStatus bleGetStatus();
BleMeteoData bleGetMeteo();

// For fast JSON payloads (if needed by UI later)
String bleGetStatusJson();
void bleFillFastJson(JsonObject& out);
