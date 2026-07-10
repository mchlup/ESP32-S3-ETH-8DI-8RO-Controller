#pragma once

#include <Arduino.h>

// Exponential backoff helper used by RelayController.
//
// NOTE:
// Arduino-ESP32 defines a macro named _max(a,b), therefore member names
// deliberately avoid identifiers such as "_max".
class RetryPolicy {
 public:
  RetryPolicy(uint32_t baseDelayMs = 50,
              float multiplier = 1.7f,
              uint32_t maxDelayMs = 1000,
              float jitterFrac = 0.2f);

  // Reset the failure counter and allow an attempt at nowMs.
  void reset(uint32_t nowMs = 0);

  // Returns true when the scheduled retry time has been reached.
  // The signed subtraction keeps the comparison safe across millis() wraparound.
  bool canAttempt(uint32_t nowMs) const;

  // Mark an attempt as successful and allow another immediate attempt.
  void onSuccess(uint32_t nowMs);

  // Mark an attempt as failed and schedule the next retry.
  void onFail(uint32_t nowMs);

  uint32_t failCount() const;
  uint32_t nextAttemptAt() const;

 private:
  uint32_t computeDelayMs() const;

  uint32_t _baseDelayMs;
  float _multiplier;
  uint32_t _maxDelayMs;
  float _jitterFrac;

  uint32_t _failCount;
  uint32_t _nextAttemptAtMs;
};
