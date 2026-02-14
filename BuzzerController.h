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

// "GONG" notification (non-blocking)
// - intentionally not an exact copy of any OEM chime
// - designed to sound similar: 2-tone gong, optionally repeated
void buzzerGong(uint8_t repeats = 1);

// Play one of the built-in sounds by ID (non-blocking)
// Supported IDs:
//   - "chime"  : modern short notification (ascending)
//   - "vag"    : softer 2-tone car-like chime (seatbelt-style)
//   - "gong"   : 2-tone gong (car-like)
//   - "alert"  : stronger alert (descending)
//   - "off"    : silence (stops current sound)
void buzzerPlaySound(const char* soundId, uint8_t repeats = 1);

// Play configured sound assigned to a notification type (non-blocking)
// Supported types:
//   - "info"
//   - "warning"
//   - "alarm"
void buzzerNotify(const char* type);

// Apply runtime config from /config.json
// Expected keys (top-level):
//   buzzer: {
//     enabled: bool,
//     freqHz: number, durationMs: number,
//     volume: number(0..255),
//     envAttackCurve: number (0.2..8), envReleaseCurve: number (0.2..8),
//     vibratoCents: number (0..60), vibratoHz: number (0..20), detuneCents: number (0..30),
//     notifyMode: bool, notifyManual: bool,
//     notifications: { info: string, warning: string, alarm: string }
//   }
void buzzerApplyConfig(const String& json);

#else

inline void buzzerInit() {}
inline void buzzerLoop() {}
inline void buzzerOnControlModeChanged(bool) {}
inline void buzzerOnManualModeChanged(const char*) {}
inline void buzzerBeep(uint16_t = 0, uint16_t = 0) {}
inline void buzzerGong(uint8_t = 1) {}
inline void buzzerPlaySound(const char*, uint8_t = 1) {}
inline void buzzerNotify(const char*) {}
inline void buzzerApplyConfig(const String&) {}

#endif
