#pragma once
#include <Arduino.h>

// BLE modul pro ESP32-S3 (dual-role):
// 1) GATT Server pro BLE displej / mobil (dashboard + povely)
// 2) GATT Client pro BLE meteostanici (příjem venkovních dat)
//
// Konfigurace: /ble.json (LittleFS)
// Seznam spárovaných/allowlist: /ble_paired.json (LittleFS)
//
// Pozn.: NimBLE si bondy ukládá do NVS. My navíc držíme vlastní allowlist v LittleFS,
// aby bylo možné povolit/zakázat zařízení bez mazání celé NVS.

void bleInit();
void bleLoop();

// JSON pro WebUI/API
String bleGetStatusJson();
String bleGetConfigJson();
bool bleSetConfigJson(const String& json);

// Párování / správa zařízení
String bleGetPairedJson();                 // {"devices":[{mac,name,role,addedAt}]}
bool bleStartPairing(uint32_t seconds, const String& roleHint); // roleHint: "display" / "meteo" / ""
bool bleStopPairing();
bool bleRemoveDevice(const String& mac);   // odebere z allowlistu + smaže bond z NimBLE (pokud jde)
bool bleClearDevices();

// Meteo poslední hodnoty (přístupné i přes status)
bool bleHasMeteoFix();

// Meteo – rychlý přístup k posledním hodnotám
bool bleGetMeteoTempC(float &outC); // true pokud je fix a hodnota je validní

// Obecný getter podle id (rezerva do budoucna). Aktuálně podporuje minimálně:
//  - "meteo" / "meteo.tempC" / "temp" / "tempC" / "" (default)
bool bleGetTempCById(const String& id, float &outC);
