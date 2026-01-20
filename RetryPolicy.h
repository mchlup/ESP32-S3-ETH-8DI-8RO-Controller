#pragma once

#include <Arduino.h>

class RetryPolicy {
public:
    RetryPolicy(uint32_t baseMs = 100,
                float factor = 1.7f,
                uint32_t maxMs = 5000,
                float jitterFrac = 0.2f);

    void reset(uint32_t nowMs);
    bool canAttempt(uint32_t nowMs) const;
    void onSuccess(uint32_t nowMs);
    void onFail(uint32_t nowMs);
    uint32_t nextAttemptAt() const;
    uint32_t failCount() const;

private:
    uint32_t computeDelayMs() const;

    uint32_t m_baseMs;
    float m_factor;
    uint32_t m_maxMs;
    float m_jitterFrac;

    uint32_t m_failCount;
    uint32_t m_nextAttemptAt;
};
