#pragma once

#include <Arduino.h>

void buzzerInit();
void buzzerLoop();

void buzzerPlayStartup();
void buzzerPlayWarning(bool enable);
bool buzzerIsWarningActive();
bool buzzerIsBusy();
