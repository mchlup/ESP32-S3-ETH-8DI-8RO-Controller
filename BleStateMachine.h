#pragma once

#include <Arduino.h>

enum class BleState : uint8_t {
    BLE_DISABLED,
    IDLE,
    SCANNING,
    CONNECTING,
    DISCOVERING,
    SUBSCRIBING,
    CONNECTED,
    WAIT_RETRY,
    ERROR_LOCKOUT
};

struct BleStateMachineConfig {
    uint32_t scanTimeoutMs = 6000;
    uint32_t connectTimeoutMs = 1500;
    uint32_t discoverTimeoutMs = 2000;
    uint32_t subscribeTimeoutMs = 2000;
    uint32_t noDataTimeoutMs = 60000;
    uint32_t errorLockoutAfter = 10;
};

class BleStateMachine {
public:
    BleStateMachine();

    void configure(const BleStateMachineConfig& cfg);
    void setEnabled(bool enabled, uint32_t nowMs);
    void tick(uint32_t nowMs);

    void onScanStart(uint32_t nowMs);
    void onScanStop(uint32_t nowMs, bool found);
    void onConnectStart(uint32_t nowMs);
    void onConnectFail(uint32_t nowMs);
    void onDiscoverStart(uint32_t nowMs);
    void onDiscoverFail(uint32_t nowMs);
    void onSubscribeStart(uint32_t nowMs);
    void onSubscribeFail(uint32_t nowMs);
    void onConnected(uint32_t nowMs);
    void onDisconnected(uint32_t nowMs);
    void onNotify(uint32_t nowMs);

    BleState state() const;
    const char* stateName() const;
    uint32_t lastDataAgeMs(uint32_t nowMs) const;
    uint32_t reconnectCount() const;
    uint32_t failCount() const;

private:
    void enterState(BleState next, uint32_t nowMs);

    BleStateMachineConfig m_cfg;
    BleState m_state;
    uint32_t m_stateEnteredMs;
    uint32_t m_lastDataMs;
    uint32_t m_reconnects;
    uint32_t m_failCount;
};
