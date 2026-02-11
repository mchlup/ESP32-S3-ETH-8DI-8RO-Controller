#pragma once
#include <Arduino.h>

void equithermInit();
void equithermApplyConfig(const String& json);
void equithermLoop();

struct EquithermStatus {
    bool active;
    bool nightMode;
    float outsideTemp;
    float currentTemp;
    float targetTemp;
};
EquithermStatus equithermGetStatus();
