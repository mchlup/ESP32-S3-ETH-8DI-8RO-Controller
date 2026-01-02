#include "I2cBus.h"
#include <Wire.h>
#include "config_pins.h"

static bool s_i2cReady = false;

void i2cInit() {
  if (s_i2cReady) return;
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_FREQ_HZ);
  s_i2cReady = true;
}

bool i2cIsReady() {
  return s_i2cReady;
}
