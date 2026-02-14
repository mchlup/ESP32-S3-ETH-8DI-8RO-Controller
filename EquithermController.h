#pragma once

// NOTE:
// This project historically exposed a separate EquithermController API.
// The current implementation lives in LogicController (logicUpdate()),
// and this header provides a thin compatibility wrapper.

#include <Arduino.h>
#include "LogicController.h"

void equithermInit();
void equithermApplyConfig(const String& json);
void equithermLoop();

// Re-export the logic status type (defined in LogicController.h)
EquithermStatus equithermGetStatus();
