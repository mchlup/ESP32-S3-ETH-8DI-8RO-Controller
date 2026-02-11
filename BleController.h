#pragma once

#include <Arduino.h>

// ArduinoJson v7+ types live in a namespace; include here so users of
// bleFillFastJson don't need to guess the correct JsonObject type.
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Optional feature
//
// FEATURE_BLE is defined by FeatureBle.h (included from Features.h).
// When disabled, this header provides lightweight no-op stubs.
// ---------------------------------------------------------------------------

#if defined(FEATURE_BLE)

void bleInit();
void bleLoop();

// Apply settings from /config.json (called by webserverLoadConfigFromFS).
// Expected JSON path: {"ble": {"enabled":true, "mac":"AA:BB:CC:DD:EE:FF", "type":"atc_mitherm", "maxAgeMs":600000}}
void bleApplyConfig(const String& json);

// Temperature helper for LogicController.
// If id is empty, returns default "meteo" temperature.
bool bleGetTempCById(const String& id, float& outC);
bool bleGetMeteoTempC(float& outC);
// Extended helpers with optional per-call maxAge override.
// If maxAgeOverrideMs == 0, uses configured ble.maxAgeMs.
// If ageMs is provided, it's filled with age of the last reading (ms).
bool bleGetTempCByIdEx(const String& id, float& outC, uint32_t maxAgeOverrideMs, uint32_t* ageMs);
bool bleGetMeteoTempCEx(float& outC, uint32_t maxAgeOverrideMs, uint32_t* ageMs);

// Diagnostics: is scan running?
bool bleIsScanning();


// Optional extended reading (if the broadcaster provides it).
// Returns false if not available / stale.
bool bleGetMeteoReading(float& outTempC, int& outHumPct, int& outPressHpa, int& outTrend);

// JSON status for /api/ble/status (compact, stable fields).
String bleGetStatusJson();

// Debug helper: force value without BLE.
void bleDebugSetMeteoTempC(float c);

// Fill BLE meteo snapshot into /api/fast (and therefore SSE payload).
// Keys are short on purpose to keep /api/fast compact.
//  ok: bool (we have fresh data)
//  fr: bool (fresh vs stale)
//  a : ageMs
//  r : RSSI
//  t : tempC
//  h : humidity % (optional)
//  p : pressure hPa (optional)
//  en: BLE enabled
//  typ: decoder type
//  mac: allowMac (optional)
void bleFillFastJson(ArduinoJson::JsonObject b);

#else

inline void bleInit() {}
inline void bleLoop() {}
inline void bleApplyConfig(const String&) {}

inline bool bleGetTempCById(const String&, float&) { return false; }
inline bool bleGetMeteoTempC(float&) { return false; }
inline bool bleGetMeteoReading(float&, int&, int&, int&) { return false; }

inline String bleGetStatusJson() { return String("{\"en\":false}"); }
inline void bleDebugSetMeteoTempC(float) {}
inline void bleFillFastJson(ArduinoJson::JsonObject) {}

#endif
