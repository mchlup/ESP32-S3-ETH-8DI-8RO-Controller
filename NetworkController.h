#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Optional module flag
//
// Network support is controlled by FEATURE_NETWORK (defined by FeatureNetwork.h).
// When the feature is disabled, this header provides lightweight no-op stubs so
// other modules can still compile without #ifdef noise.
// ---------------------------------------------------------------------------

#if defined(FEATURE_NETWORK)

void networkInit();
void networkLoop();

void networkApplyConfig(const String& json);

bool networkIsConnected();
bool networkIsWifiConnected();
bool networkIsEthernetConnected();
String networkGetIp();

bool networkIsTimeValid();
String networkGetTimeIso();
uint32_t networkGetTimeEpoch();
String networkGetTimeSource();
bool networkIsRtcPresent();

#else

// ---- stubs (feature disabled) ----
inline void networkInit() {}
inline void networkLoop() {}

inline void networkApplyConfig(const String&) {}

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
