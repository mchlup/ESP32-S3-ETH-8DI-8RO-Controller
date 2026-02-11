#pragma once

#include <Arduino.h>

static constexpr uint8_t RELAY_COUNT = 8;

enum class RelayId : uint8_t {
  R1 = 0,
  R2,
  R3,
  R4,
  R5,
  R6,
  R7,
  R8
};

void relayInit();
void relayUpdate();

void relaySet(RelayId id, bool on);
void relayToggle(RelayId id);
bool relayGetState(RelayId id);

// Logical mask (bit0=R1 ... bit7=R8)
uint8_t relayGetMask();
void relaySetMask(uint8_t mask);

void relayAllOff();
void relayAllOn();

// !!! DŮLEŽITÉ: Print& (ne Stream&) – kvůli LogicController / .ino
void relayPrintStates(Print& out);

// Health
bool relayIsOk();

// Telemetry for diagnostics/UI
uint32_t relayGetI2cErrorCount();
uint32_t relayGetI2cRecoveryCount();
uint32_t relayGetI2cLastErrorMs();
const char* relayGetI2cLastError();
uint32_t relayGetI2cNextRetryInMs();
uint32_t relayGetI2cFailCount();
