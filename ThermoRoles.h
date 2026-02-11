#pragma once

#include <Arduino.h>

// Normalization helpers for role strings used across UI/config.

inline String thermoNormalizeRole(const String& in) {
  String s = in;
  s.trim();
  s.toLowerCase();
  s.replace(" ", "_");
  s.replace("-", "_");
  // collapse repeated underscores
  while (s.indexOf("__") >= 0) s.replace("__", "_");
  return s;
}
