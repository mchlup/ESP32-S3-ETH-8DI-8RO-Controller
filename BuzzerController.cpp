#include "BuzzerController.h"
#include "config_pins.h"

namespace {
  struct Step {
    bool on = false;
    uint16_t durationMs = 0;
  };

  struct PatternState {
    const Step* steps = nullptr;
    uint8_t count = 0;
    uint8_t index = 0;
    bool active = false;
    bool repeat = false;
    bool warning = false;
    uint32_t stepStartedMs = 0;
  };

  constexpr Step kStartupPattern[] = {
    {true, 90}, {false, 90}, {true, 90}, {false, 0}
  };

  constexpr Step kWarningPattern[] = {
    {true, 450}, {false, 200}, {true, 450}, {false, 900}
  };

  PatternState s_pat;
  bool s_pinReady = false;

  void applyOutput(bool on) {
    if (!s_pinReady) return;
    digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
  }

  void stopPattern() {
    s_pat.active = false;
    s_pat.repeat = false;
    s_pat.warning = false;
    s_pat.steps = nullptr;
    s_pat.count = 0;
    s_pat.index = 0;
    s_pat.stepStartedMs = 0;
    applyOutput(false);
  }

  void startPattern(const Step* steps, uint8_t count, bool repeat, bool warning) {
    if (!steps || count == 0) {
      stopPattern();
      return;
    }
    s_pat.steps = steps;
    s_pat.count = count;
    s_pat.index = 0;
    s_pat.active = true;
    s_pat.repeat = repeat;
    s_pat.warning = warning;
    s_pat.stepStartedMs = millis();
    applyOutput(s_pat.steps[0].on);
  }
}

void buzzerInit() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  s_pinReady = true;
  stopPattern();
}

void buzzerLoop() {
  if (!s_pat.active || !s_pat.steps || s_pat.count == 0) return;
  const uint32_t now = millis();
  const Step& cur = s_pat.steps[s_pat.index];
  if ((uint32_t)(now - s_pat.stepStartedMs) < cur.durationMs) return;

  uint8_t next = (uint8_t)(s_pat.index + 1);
  if (next >= s_pat.count) {
    if (!s_pat.repeat) {
      stopPattern();
      return;
    }
    next = 0;
  }

  s_pat.index = next;
  s_pat.stepStartedMs = now;
  applyOutput(s_pat.steps[s_pat.index].on);
}

void buzzerPlayStartup() {
  if (s_pat.warning) return; // warning has priority
  startPattern(kStartupPattern, (uint8_t)(sizeof(kStartupPattern) / sizeof(kStartupPattern[0])), false, false);
}

void buzzerPlayWarning(bool enable) {
  if (!enable) {
    if (s_pat.warning) stopPattern();
    return;
  }
  startPattern(kWarningPattern, (uint8_t)(sizeof(kWarningPattern) / sizeof(kWarningPattern[0])), true, true);
}

bool buzzerIsWarningActive() {
  return s_pat.active && s_pat.warning;
}

bool buzzerIsBusy() {
  return s_pat.active;
}
