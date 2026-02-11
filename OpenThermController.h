#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// OpenTherm is an optional module.
//
// Enable it by defining FEATURE_OPENTHERM (typically from Features.h).
//
// If FEATURE_OPENTHERM is NOT defined, this header still compiles and provides
// tiny no-op stubs so any accidental includes won't break the build.
// ---------------------------------------------------------------------------

struct OpenThermStatus {
  bool present = false;
  bool ready = false;
  bool fault = false;
  float boilerTempC = NAN;
  float returnTempC = NAN;
  uint32_t lastUpdateMs = 0;
  String reason = "";
};

#if defined(FEATURE_OPENTHERM)

void openthermInit();
void openthermLoop();
OpenThermStatus openthermGetStatus();

#else

inline void openthermInit() {}
inline void openthermLoop() {}
inline OpenThermStatus openthermGetStatus() { return OpenThermStatus(); }

#endif
