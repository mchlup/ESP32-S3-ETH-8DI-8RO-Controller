#include "RetryPolicy.h"

RetryPolicy::RetryPolicy(uint32_t baseMs, float factor, uint32_t maxMs, float jitterFrac)
    : m_baseMs(baseMs),
      m_factor(factor),
      m_maxMs(maxMs),
      m_jitterFrac(jitterFrac),
      m_failCount(0),
      m_nextAttemptAt(0) {}

void RetryPolicy::reset(uint32_t nowMs) {
    m_failCount = 0;
    m_nextAttemptAt = nowMs;
}

bool RetryPolicy::canAttempt(uint32_t nowMs) const {
    return (int32_t)(nowMs - m_nextAttemptAt) >= 0;
}

void RetryPolicy::onSuccess(uint32_t nowMs) {
    m_failCount = 0;
    m_nextAttemptAt = nowMs;
}

void RetryPolicy::onFail(uint32_t nowMs) {
    m_failCount = (m_failCount < 0xFFFFFFFFUL) ? (m_failCount + 1U) : m_failCount;
    uint32_t delayMs = computeDelayMs();
    m_nextAttemptAt = nowMs + delayMs;
}

uint32_t RetryPolicy::nextAttemptAt() const {
    return m_nextAttemptAt;
}

uint32_t RetryPolicy::failCount() const {
    return m_failCount;
}

uint32_t RetryPolicy::computeDelayMs() const {
    if (m_failCount == 0) return m_baseMs;
    float delay = (float)m_baseMs;
    for (uint32_t i = 1; i < m_failCount; i++) {
        delay *= m_factor;
        if (delay >= (float)m_maxMs) {
            delay = (float)m_maxMs;
            break;
        }
    }
    if (delay > (float)m_maxMs) delay = (float)m_maxMs;

    const float jitterSpan = delay * m_jitterFrac;
    const int32_t jitter = (jitterSpan > 1.0f)
        ? (int32_t)(random(-(int32_t)jitterSpan, (int32_t)jitterSpan + 1))
        : 0;

    int32_t withJitter = (int32_t)delay + jitter;
    if (withJitter < (int32_t)m_baseMs) withJitter = (int32_t)m_baseMs;
    return (uint32_t)withJitter;
}
