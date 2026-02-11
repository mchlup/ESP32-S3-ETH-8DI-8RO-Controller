#pragma once

#include <Arduino.h>

// Exponential backoff helper used by RelayController.
//
// IMPORTANT:
// Arduino-ESP32 defines a macro named _max(a,b). Avoid member names like "_max".

class RetryPolicy {
 public:
  RetryPolicy(uint32_t baseDelayMs = 50,
              float multiplier = 1.7f,
              uint32_t maxDelayMs = 1000,
              float jitterFrac = 0.2f)
      : _baseDelayMs(baseDelayMs),
        _multiplier(multiplier),
        _maxDelayMs(maxDelayMs),
        _jitterFrac(jitterFrac) {
    reset(0);
  }

  // Reset attempts and allow immediate attempt.
  void reset(uint32_t nowMs = 0) {
    _failCount = 0;
    _nextAttemptAtMs = nowMs;
  }

  // True if we may attempt now.
  bool canAttempt(uint32_t nowMs) const {
    return (int32_t)(nowMs - _nextAttemptAtMs) >= 0;
  }

  // Mark a successful attempt and allow immediate next attempt.
  void onSuccess(uint32_t nowMs) {
    (void)nowMs;
    _failCount = 0;
    _nextAttemptAtMs = 0;  // immediate
  }

  // Mark a failed attempt and schedule next time.
  void onFail(uint32_t nowMs) {
    const uint32_t delay = computeDelayMs();
    _nextAttemptAtMs = nowMs + delay;
    _failCount++;
  }

  uint32_t failCount() const { return _failCount; }
  uint32_t nextAttemptAt() const { return _nextAttemptAtMs; }

 private:
  uint32_t computeDelayMs() const {
    // base * multiplier^failCount, clamped to max, optional jitter.
    double d = (double)_baseDelayMs;
    for (uint32_t i = 0; i < _failCount; i++) d *= (double)_multiplier;
    if (d > (double)_maxDelayMs) d = (double)_maxDelayMs;
    uint32_t out = (uint32_t)d;

    if (_jitterFrac > 0.0f && out > 0) {
      const int32_t span = (int32_t)((float)out * _jitterFrac);
      if (span > 0) {
        const int32_t rnd = (int32_t)(esp_random() % (uint32_t)(span * 2 + 1)) - span;
        int32_t j = (int32_t)out + rnd;
        if (j < 0) j = 0;
        out = (uint32_t)j;
      }
    }
    return out;
  }

  uint32_t _baseDelayMs;
  float _multiplier;
  uint32_t _maxDelayMs;
  float _jitterFrac;

  uint32_t _failCount = 0;
  uint32_t _nextAttemptAtMs = 0;
};
