#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Optional feature
//
// FEATURE_WEBSERVER is defined by FeatureWebServer.h (included from Features.h).
// When disabled, this header provides lightweight no-op stubs so other modules
// can compile without #ifdef noise.
// ---------------------------------------------------------------------------

#if defined(FEATURE_WEBSERVER)

void webserverLoadConfigFromFS();

void webserverInit();
void webserverLoop();

// Hint for the server that something changed and SSE clients should get
// an update as soon as possible.
void webserverNotifyStateChanged();

#else

inline void webserverLoadConfigFromFS() {}
inline void webserverInit() {}
inline void webserverLoop() {}
inline void webserverNotifyStateChanged() {}

#endif
