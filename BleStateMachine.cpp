#include "BleStateMachine.h"

BleStateMachine::BleStateMachine()
    : m_state(BleState::BLE_DISABLED),
      m_stateEnteredMs(0),
      m_lastDataMs(0),
      m_reconnects(0),
      m_failCount(0) {}

void BleStateMachine::configure(const BleStateMachineConfig& cfg) {
    m_cfg = cfg;
}

void BleStateMachine::setEnabled(bool enabled, uint32_t nowMs) {
    if (!enabled) {
        enterState(BleState::BLE_DISABLED, nowMs);
        return;
    }
    if (m_state == BleState::BLE_DISABLED) {
        enterState(BleState::IDLE, nowMs);
    }
}

void BleStateMachine::tick(uint32_t nowMs) {
    if (m_state == BleState::BLE_DISABLED) return;

    switch (m_state) {
        case BleState::SCANNING:
            if ((int32_t)(nowMs - m_stateEnteredMs) >= (int32_t)m_cfg.scanTimeoutMs) {
                enterState(BleState::WAIT_RETRY, nowMs);
            }
            break;
        case BleState::CONNECTING:
            if ((int32_t)(nowMs - m_stateEnteredMs) >= (int32_t)m_cfg.connectTimeoutMs) {
                onConnectFail(nowMs);
            }
            break;
        case BleState::DISCOVERING:
            if ((int32_t)(nowMs - m_stateEnteredMs) >= (int32_t)m_cfg.discoverTimeoutMs) {
                onDiscoverFail(nowMs);
            }
            break;
        case BleState::SUBSCRIBING:
            if ((int32_t)(nowMs - m_stateEnteredMs) >= (int32_t)m_cfg.subscribeTimeoutMs) {
                onSubscribeFail(nowMs);
            }
            break;
        case BleState::CONNECTED:
            if (m_lastDataMs && (int32_t)(nowMs - m_lastDataMs) >= (int32_t)m_cfg.noDataTimeoutMs) {
                enterState(BleState::WAIT_RETRY, nowMs);
            }
            break;
        case BleState::WAIT_RETRY:
        case BleState::ERROR_LOCKOUT:
        case BleState::IDLE:
        case BleState::BLE_DISABLED:
            break;
    }
}

void BleStateMachine::onScanStart(uint32_t nowMs) {
    enterState(BleState::SCANNING, nowMs);
}

void BleStateMachine::onScanStop(uint32_t nowMs, bool found) {
    if (found) {
        enterState(BleState::CONNECTING, nowMs);
    } else {
        enterState(BleState::WAIT_RETRY, nowMs);
    }
}

void BleStateMachine::onConnectStart(uint32_t nowMs) {
    enterState(BleState::CONNECTING, nowMs);
}

void BleStateMachine::onConnectFail(uint32_t nowMs) {
    m_failCount++;
    if (m_cfg.errorLockoutAfter && m_failCount >= m_cfg.errorLockoutAfter) {
        enterState(BleState::ERROR_LOCKOUT, nowMs);
        return;
    }
    enterState(BleState::WAIT_RETRY, nowMs);
}

void BleStateMachine::onDiscoverStart(uint32_t nowMs) {
    enterState(BleState::DISCOVERING, nowMs);
}

void BleStateMachine::onDiscoverFail(uint32_t nowMs) {
    m_failCount++;
    enterState(BleState::WAIT_RETRY, nowMs);
}

void BleStateMachine::onSubscribeStart(uint32_t nowMs) {
    enterState(BleState::SUBSCRIBING, nowMs);
}

void BleStateMachine::onSubscribeFail(uint32_t nowMs) {
    m_failCount++;
    enterState(BleState::WAIT_RETRY, nowMs);
}

void BleStateMachine::onConnected(uint32_t nowMs) {
    enterState(BleState::CONNECTED, nowMs);
    m_failCount = 0;
    m_reconnects++;
}

void BleStateMachine::onDisconnected(uint32_t nowMs) {
    enterState(BleState::WAIT_RETRY, nowMs);
}

void BleStateMachine::onNotify(uint32_t nowMs) {
    m_lastDataMs = nowMs;
    if (m_state != BleState::CONNECTED) {
        enterState(BleState::CONNECTED, nowMs);
    }
}

BleState BleStateMachine::state() const {
    return m_state;
}

const char* BleStateMachine::stateName() const {
    switch (m_state) {
        case BleState::BLE_DISABLED: return "BLE_DISABLED";
        case BleState::IDLE: return "IDLE";
        case BleState::SCANNING: return "SCANNING";
        case BleState::CONNECTING: return "CONNECTING";
        case BleState::DISCOVERING: return "DISCOVERING";
        case BleState::SUBSCRIBING: return "SUBSCRIBING";
        case BleState::CONNECTED: return "CONNECTED";
        case BleState::WAIT_RETRY: return "WAIT_RETRY";
        case BleState::ERROR_LOCKOUT: return "ERROR_LOCKOUT";
    }
    return "UNKNOWN";
}

uint32_t BleStateMachine::lastDataAgeMs(uint32_t nowMs) const {
    if (!m_lastDataMs) return 0;
    return nowMs - m_lastDataMs;
}

uint32_t BleStateMachine::reconnectCount() const {
    return m_reconnects;
}

uint32_t BleStateMachine::failCount() const {
    return m_failCount;
}

void BleStateMachine::enterState(BleState next, uint32_t nowMs) {
    if (m_state == next) return;
    m_state = next;
    m_stateEnteredMs = nowMs;
}
