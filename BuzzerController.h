#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Optional feature
//
// FEATURE_BUZZER is defined by FeatureBuzzer.h (included from Features.h).
// When disabled, this header provides lightweight no-op stubs so the rest of
// the project can compile without sprinkling #ifdefs everywhere.
// ---------------------------------------------------------------------------

#if defined(FEATURE_BUZZER)

void buzzerInit();
void buzzerLoop();

// UI/logic notifications
void buzzerOnControlModeChanged(bool autoMode);
void buzzerOnManualModeChanged(const char* modeName);

// Generic one-shot beep (non-blocking)
void buzzerBeep(uint16_t freqHz = 2000, uint16_t durationMs = 80);

#else

inline void buzzerInit() {}
inline void buzzerLoop() {}
inline void buzzerOnControlModeChanged(bool) {}
inline void buzzerOnManualModeChanged(const char*) {}
inline void buzzerBeep(uint16_t = 0, uint16_t = 0) {}

#endif
