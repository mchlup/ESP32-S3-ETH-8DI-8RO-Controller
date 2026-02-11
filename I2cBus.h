#pragma once

#include <Arduino.h>

// Centralized I2C init for the board.
void i2cInit();
bool i2cIsReady();
