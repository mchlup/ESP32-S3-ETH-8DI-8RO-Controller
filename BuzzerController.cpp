// IMPORTANT: include Features.h first so FEATURE_BUZZER is visible before
// including the header (which provides stubs when the feature is disabled).
#include "Features.h"
#include "BuzzerController.h"

#if defined(FEATURE_BUZZER)

#include <Arduino.h>
#include "config_pins.h"

namespace {
  bool g_inited = false;
  bool g_playing = false;
  uint32_t g_endMs = 0;
  uint16_t g_freq = 2000;

  void stopNow() {
    noTone(BUZZER_PIN);
    g_playing = false;
  }
}

void buzzerInit() {
  if (g_inited) return;
  g_inited = true;
  pinMode(BUZZER_PIN, OUTPUT);
  stopNow();
}

void buzzerBeep(uint16_t freqHz, uint16_t durationMs) {
  if (!g_inited) buzzerInit();
  g_freq = freqHz;
  tone(BUZZER_PIN, (unsigned int)g_freq);
  g_playing = true;
  g_endMs = millis() + durationMs;
}

void buzzerOnControlModeChanged(bool autoMode) {
  // AUTO: one short beep, MANUAL: two quick beeps
  if (autoMode) {
    buzzerBeep(2200, 60);
  } else {
    buzzerBeep(1600, 60);
    // second beep scheduled by loop
    g_endMs = millis() + 60;
  }
}

void buzzerOnManualModeChanged(const char* /*modeName*/) {
  buzzerBeep(1800, 80);
}

void buzzerLoop() {
  if (!g_inited) return;
  if (!g_playing) return;
  if ((int32_t)(millis() - g_endMs) >= 0) {
    stopNow();
  }
}

#endif // FEATURE_BUZZER
