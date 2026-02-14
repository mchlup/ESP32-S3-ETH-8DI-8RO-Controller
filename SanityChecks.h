#pragma once

#include <Arduino.h>

namespace SanityChecks {
  // Runs quick non-blocking checks and prints warnings to Serial.
  // Safe to call even if some subsystems are disabled.
  void runOnce();
}
