#include "RetryPolicy.h"

#include <esp_system.h>
#include <limits.h>

RetryPolicy::RetryPolicy(uint32_t baseDelayMs,
                         float multiplier,
                         uint32_t maxDelayMs,
                         float jitterFrac)
    : _baseDelayMs(baseDelayMs),
      _multiplier(multiplier > 0.0f ? multiplier : 1.0f),
      _maxDelayMs(maxDelayMs >= baseDelayMs ? maxDelayMs : baseDelayMs),
      _jitterFrac(jitterFrac >= 0.0f ? jitterFrac : 0.0f),
      _failCount(0),
      _nextAttemptAtMs(0) {
  reset(0);
}

void RetryPolicy::reset(uint32_t nowMs) {
  _failCount = 0;
  _nextAttemptAtMs = nowMs;
}

bool RetryPolicy::canAttempt(uint32_t nowMs) const {
  return static_cast<int32_t>(nowMs - _nextAttemptAtMs) >= 0;
}

void RetryPolicy::onSuccess(uint32_t nowMs) {
  _failCount = 0;
  _nextAttemptAtMs = nowMs;
}

void RetryPolicy::onFail(uint32_t nowMs) {
  const uint32_t delayMs = computeDelayMs();
  _nextAttemptAtMs = nowMs + delayMs;

  if (_failCount < UINT32_MAX) {
    ++_failCount;
  }
}

uint32_t RetryPolicy::failCount() const {
  return _failCount;
}

uint32_t RetryPolicy::nextAttemptAt() const {
  return _nextAttemptAtMs;
}

uint32_t RetryPolicy::computeDelayMs() const {
  double delayMs = static_cast<double>(_baseDelayMs);

  for (uint32_t i = 0; i < _failCount; ++i) {
    delayMs *= static_cast<double>(_multiplier);
    if (delayMs >= static_cast<double>(_maxDelayMs)) {
      delayMs = static_cast<double>(_maxDelayMs);
      break;
    }
  }

  if (delayMs > static_cast<double>(_maxDelayMs)) {
    delayMs = static_cast<double>(_maxDelayMs);
  }

  uint32_t result = static_cast<uint32_t>(delayMs);
  if (_jitterFrac <= 0.0f || result == 0) {
    return result;
  }

  double spanValue = static_cast<double>(result) * static_cast<double>(_jitterFrac);
  if (spanValue > static_cast<double>(INT32_MAX)) {
    spanValue = static_cast<double>(INT32_MAX);
  }

  const int32_t span = static_cast<int32_t>(spanValue);
  if (span <= 0) {
    return result;
  }

  const uint64_t range64 = static_cast<uint64_t>(span) * 2ULL + 1ULL;
  const uint32_t range = range64 > UINT32_MAX
      ? UINT32_MAX
      : static_cast<uint32_t>(range64);
  const int32_t jitter = static_cast<int32_t>(esp_random() % range) - span;
  const int64_t jittered = static_cast<int64_t>(result) + jitter;

  if (jittered <= 0) {
    return 0;
  }
  if (jittered >= static_cast<int64_t>(_maxDelayMs)) {
    return _maxDelayMs;
  }
  return static_cast<uint32_t>(jittered);
}
