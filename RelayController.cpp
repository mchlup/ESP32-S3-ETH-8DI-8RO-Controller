#include "RelayController.h"
#include <Wire.h>
#include "I2cBus.h"
#include "config_pins.h"

// Minimal driver for TCA9554 output register
static constexpr uint8_t REG_INPUT  = 0x00;
static constexpr uint8_t REG_OUTPUT = 0x01;
static constexpr uint8_t REG_POL    = 0x02;
static constexpr uint8_t REG_CFG    = 0x03;

static bool s_ok = false;
static uint8_t s_mask = 0x00; // logical ON bits (bit0=R1 ... bit7=R8)

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

static bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool initTca() {
  i2cInit();

  // polarity normal
  if (!writeReg(REG_POL, 0x00)) return false;

  // all 8 as outputs (0=output)
  if (!writeReg(REG_CFG, 0x00)) return false;

  // set initial outputs
  if (!writeReg(REG_OUTPUT, toHw(s_mask))) return false;

  return true;
}

void relayInit() {
  s_mask = 0x00;        // zachovej default OFF
  s_ok = initTca();
}

void relayUpdate() {
  // reserved – currently nothing
}

void relaySet(RelayId id, bool on) {
  if (!s_ok) return;
  if ((uint8_t)id >= RELAY_COUNT) return;

  uint8_t bit = (uint8_t)(1U << (uint8_t)id);
  if (on) s_mask |= bit;
  else    s_mask &= (uint8_t)~bit;

  writeReg(REG_OUTPUT, toHw(s_mask));
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
  if (!s_ok) return;
  s_mask = mask;
  writeReg(REG_OUTPUT, toHw(s_mask));
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
