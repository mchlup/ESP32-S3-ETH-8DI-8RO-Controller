#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// OtaController - Arduino IDE OTA (network upload)
//
// Purpose:
//   - Allow remote firmware upload directly from Arduino IDE via network port
//     (ArduinoOTA), without manually dealing with .bin files.
//
// Notes:
//   - Requires active IP connectivity (WiFi or Ethernet).
//   - Uses mDNS to advertise the device so Arduino IDE shows a "Network Port".
//   - Password is optional (stored in Preferences via ConfigStore).
// ---------------------------------------------------------------------------

#if defined(FEATURE_OTA)

#include <ArduinoJson.h>

struct OtaConfig {
  bool enabled = true;
  String hostname;
  uint16_t port = 3232;
  bool passwordSet = false; // informational only
};

void otaInit();
void otaLoop();

OtaConfig otaGetConfig();
void otaApplyConfig(const String& json); // expects object { enabled, hostname, port, password }

void otaFillFastJson(JsonObject out); // lightweight status for /api/fast
String otaGetStatusJson();                 // detailed JSON for /api/ota/status

#else

struct OtaConfig {
  bool enabled = false;
  String hostname;
  uint16_t port = 0;
  bool passwordSet = false;
};

inline void otaInit() {}
inline void otaLoop() {}
inline OtaConfig otaGetConfig() { return OtaConfig{}; }
inline void otaApplyConfig(const String&) {}
inline void otaFillFastJson(...) {}
inline String otaGetStatusJson() { return String("{\"ok\":false,\"err\":\"disabled\"}"); }

#endif
