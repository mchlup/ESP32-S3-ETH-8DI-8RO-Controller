#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// NetworkController
//
// Unified network layer for this project:
//   - WiFiManager autoConnect / config portal (non-blocking)
//   - W5500 Ethernet on the Waveshare ESP32-S3-ETH-8DI-8RO
//   - shared IP/status helpers for UI, MQTT, OTA and time sync
//
// The web UI and service stack should always ask this layer for connectivity
// instead of checking WiFi directly, otherwise Ethernet-only deployments break.
// ---------------------------------------------------------------------------

#if defined(FEATURE_NETWORK)

void networkInit();
void networkLoop();

void networkApplyConfig(const String& json);

// Request WiFiManager config portal on next boot (persistent flag + reboot).
void networkRequestConfigPortal();

bool networkIsConnected();
bool networkIsWifiConnected();
bool networkIsEthernetConnected();
String networkGetIp();

// Time helpers (not part of minimal build)
bool networkIsTimeValid();
String networkGetTimeIso();
uint32_t networkGetTimeEpoch();
String networkGetTimeSource();
bool networkIsRtcPresent();

#else

inline void networkInit() {}
inline void networkLoop() {}
inline void networkApplyConfig(const String&) {}
inline void networkRequestConfigPortal() {}
inline bool networkIsConnected() { return false; }
inline bool networkIsWifiConnected() { return false; }
inline bool networkIsEthernetConnected() { return false; }
inline String networkGetIp() { return String(); }
inline bool networkIsTimeValid() { return false; }
inline String networkGetTimeIso() { return String(); }
inline uint32_t networkGetTimeEpoch() { return 0; }
inline String networkGetTimeSource() { return String("disabled"); }
inline bool networkIsRtcPresent() { return false; }

#endif
