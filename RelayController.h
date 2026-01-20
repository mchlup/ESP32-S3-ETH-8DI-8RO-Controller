#pragma once
#include <Arduino.h>

#define RELAY_COUNT 8

enum RelayId : uint8_t {
  RELAY_1 = 0,
  RELAY_2,
  RELAY_3,
  RELAY_4,
  RELAY_5,
  RELAY_6,
  RELAY_7,
  RELAY_8
};

void relayInit();
void relayUpdate();

void relaySet(RelayId id, bool on);
void relayToggle(RelayId id);
bool relayGetState(RelayId id);

void relayAllOff();
void relayAllOn();

uint8_t relayGetMask();
void relaySetMask(uint8_t mask);

void relayPrintStates(Stream &out);

// --- Diagnostics / telemetry ---
uint32_t relayGetI2cErrorCount();
uint32_t relayGetI2cRecoveryCount();
uint32_t relayGetI2cLastErrorMs();
const char* relayGetI2cLastError();
bool relayIsOk();
uint32_t relayGetI2cNextRetryInMs();
uint32_t relayGetI2cFailCount();
