#include "RelayController.h"
#include <Wire.h>
#include "I2cBus.h"
#include "config_pins.h"
#include "RetryPolicy.h"
#include "Log.h"

// Minimal driver for TCA9554 output register
static constexpr uint8_t REG_INPUT  = 0x00;
static constexpr uint8_t REG_OUTPUT = 0x01;
static constexpr uint8_t REG_POL    = 0x02;
static constexpr uint8_t REG_CFG    = 0x03;

static bool s_ok = false;
static uint8_t s_mask = 0x00; // logical ON bits (bit0=R1 ... bit7=R8)

// ---- Non-blocking apply state ----
static bool     s_applyPending = false;
static uint8_t  s_pendingMask  = 0x00;
static RetryPolicy s_applyRetry(50, 1.7f, 1000, 0.2f);
static RetryPolicy s_recoverRetry(500, 1.7f, 30000, 0.2f);

// ---- Telemetry ----
static uint32_t s_i2cErrors     = 0;
static uint32_t s_i2cRecoveries = 0;
static uint32_t s_lastI2cErrMs  = 0;
static char     s_lastI2cErr[96] = {0};
static uint32_t s_lastI2cLogMs  = 0;

// Pokud by se ukázalo, že relé je active-low, přepni na 1.
#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW 0
#endif

static uint8_t toHw(uint8_t logicalMask) {
#if RELAY_ACTIVE_LOW
  return (uint8_t)~logicalMask;
#else
  return logicalMask;
#endif
}

static void noteI2cErrorThrottled(const char* msg) {
  s_i2cErrors++;
  s_lastI2cErrMs = millis();
  if (msg) {
    snprintf(s_lastI2cErr, sizeof(s_lastI2cErr), "%s", msg);
  }

  const uint32_t now = s_lastI2cErrMs;
  // Throttle: do not spam Serial during bus faults
  if ((uint32_t)(now - s_lastI2cLogMs) >= 5000) {
    s_lastI2cLogMs = now;
    LOGW("RELAY I2C error: %s", s_lastI2cErr);
  }
}

static bool writeRegRaw(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  Wire.write(val);
  const uint8_t rc = Wire.endTransmission();
  if (rc != 0) {
    char buf[64];
    snprintf(buf, sizeof(buf), "write reg 0x%02X rc=%u", reg, (unsigned)rc);
    noteI2cErrorThrottled(buf);
    return false;
  }
  return true;
}

static bool readRegRaw(uint8_t reg, uint8_t &out) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    char buf[64];
    snprintf(buf, sizeof(buf), "read reg 0x%02X addrNACK", reg);
    noteI2cErrorThrottled(buf);
    return false;
  }
  const uint8_t n = Wire.requestFrom((int)TCA9554_ADDR, 1);
  if (n != 1) {
    char buf[64];
    snprintf(buf, sizeof(buf), "read reg 0x%02X req=%u", reg, (unsigned)n);
    noteI2cErrorThrottled(buf);
    return false;
  }
  out = Wire.read();
  return true;
}

static void scheduleApply(uint8_t logicalMask) {
  s_applyPending = true;
  s_pendingMask  = logicalMask;
  s_applyRetry.reset(millis());
}

static void processPending(uint32_t now) {
  if (!s_applyPending) return;
  if (!s_ok) return;
  if (!s_applyRetry.canAttempt(now)) return;

  const uint8_t hw = toHw(s_pendingMask);

  bool ok = writeRegRaw(REG_OUTPUT, hw);
  if (ok) {
    uint8_t rb = 0;
    ok = readRegRaw(REG_OUTPUT, rb) && (rb == hw);
    if (!ok) {
      noteI2cErrorThrottled("verify mismatch/output read failed");
    }
  }

  if (ok) {
    s_applyPending = false;
    s_applyRetry.onSuccess(now);
    return;
  }

  // Non-blocking retry: next attempt later (no delay())
  s_applyRetry.onFail(now);
  if (s_applyRetry.failCount() >= 3) {
    // Mark expander as not OK -> recovery path will re-init and re-apply mask
    s_ok = false;
    return;
  }
}

static bool initTca() {
  i2cInit();

  // polarity normal
  if (!writeRegRaw(REG_POL, 0x00)) return false;

  // all 8 as outputs (0=output)
  if (!writeRegRaw(REG_CFG, 0x00)) return false;

  // set initial outputs (write + verify)
  const uint8_t hw = toHw(s_mask);
  if (!writeRegRaw(REG_OUTPUT, hw)) return false;
  uint8_t rb = 0;
  if (!readRegRaw(REG_OUTPUT, rb) || (rb != hw)) {
    noteI2cErrorThrottled("init verify mismatch");
    return false;
  }

  return true;
}

void relayInit() {
  s_mask = 0x00;        // default OFF
  scheduleApply(s_mask);
  s_ok = initTca();
  if (s_ok) {
    s_i2cRecoveries++; // first successful init
    s_recoverRetry.onSuccess(millis());
    processPending(millis());
  }
}

void relayUpdate() {
  const uint32_t now = millis();

  // Apply pending mask if needed (non-blocking)
  processPending(now);

  // Periodic health-check (detect expander reset / desync)
  static uint32_t lastCheckMs = 0;
  if (s_ok && (uint32_t)(now - lastCheckMs) >= 2000) {
    lastCheckMs = now;
    uint8_t rb = 0;
    if (!readRegRaw(REG_OUTPUT, rb) || (rb != toHw(s_mask))) {
      noteI2cErrorThrottled("health-check desync");
      s_ok = false;
    }
  }

  // Auto-recovery if expander is not OK
  if (!s_ok) {
    if (!s_recoverRetry.canAttempt(now)) return;

    // Re-init expander and re-apply desired mask
    const bool ok = initTca();
    if (ok) {
      s_ok = true;
      s_i2cRecoveries++;
      s_recoverRetry.onSuccess(now);
      // Ensure requested mask is applied (in case init succeeded but output differs)
      scheduleApply(s_mask);
      processPending(now);
    } else {
      s_recoverRetry.onFail(now);
    }
  }
}

void relaySet(RelayId id, bool on) {
  if ((uint8_t)id >= RELAY_COUNT) return;

  const uint8_t bit = (uint8_t)(1U << (uint8_t)id);
  if (on) s_mask |= bit;
  else    s_mask &= (uint8_t)~bit;

  scheduleApply(s_mask);
  processPending(millis());
}

void relayToggle(RelayId id) {
  relaySet(id, !relayGetState(id));
}

bool relayGetState(RelayId id) {
  if ((uint8_t)id >= RELAY_COUNT) return false;
  return (s_mask & (1U << (uint8_t)id)) != 0;
}

void relayAllOff() {
  relaySetMask(0x00);
}

void relayAllOn() {
  relaySetMask(0xFF);
}

uint8_t relayGetMask() {
  return s_mask;
}

void relaySetMask(uint8_t mask) {
  s_mask = mask;
  scheduleApply(s_mask);
  processPending(millis());
}

void relayPrintStates(Stream &out) {
  out.print(F("[RELAY] mask=0b"));
  for (int i = 7; i >= 0; i--) out.print((s_mask >> i) & 1);
  out.print(F(" ["));
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    out.print(F("R")); out.print(i + 1);
    out.print(F("=")); out.print(relayGetState((RelayId)i) ? F("ON") : F("OFF"));
    if (i != RELAY_COUNT - 1) out.print(F(", "));
  }
  out.println(F("]"));
}

// --- Diagnostics / telemetry ---
uint32_t relayGetI2cErrorCount() {
  return s_i2cErrors;
}

uint32_t relayGetI2cRecoveryCount() {
  return s_i2cRecoveries;
}

uint32_t relayGetI2cLastErrorMs() {
  return s_lastI2cErrMs;
}

const char* relayGetI2cLastError() {
  return s_lastI2cErr;
}

bool relayIsOk() {
  return s_ok;
}

uint32_t relayGetI2cNextRetryInMs() {
  const uint32_t now = millis();
  if (s_ok) return 0;
  const uint32_t nextAt = s_recoverRetry.nextAttemptAt();
  if ((int32_t)(nextAt - now) <= 0) return 0;
  return nextAt - now;
}

uint32_t relayGetI2cFailCount() {
  return (uint32_t)s_recoverRetry.failCount();
}
