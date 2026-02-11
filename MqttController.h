#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Optional module flag
//
// MQTT support is controlled by FEATURE_MQTT (defined by FeatureMqtt.h).
// When the feature is disabled, this header provides lightweight no-op stubs so
// other modules can still compile without #ifdef noise.
// ---------------------------------------------------------------------------

#if defined(FEATURE_MQTT)

void mqttInit();
void mqttLoop();

bool mqttPublish(const String& topic, const String& payload, bool retain = false);

// Returns true if we have a cached last value for a topic.
bool mqttGetLastValueInfo(const String& topic, String* outPayload, uint32_t* outLastMs);

#else

inline void mqttInit() {}
inline void mqttLoop() {}

inline bool mqttPublish(const String&, const String&, bool = false) { return false; }
inline bool mqttGetLastValueInfo(const String&, String*, uint32_t*) { return false; }

#endif
