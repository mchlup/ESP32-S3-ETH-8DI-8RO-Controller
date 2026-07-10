#pragma once

#include <Arduino.h>

// Parse a temperature from an arbitrary payload.
// Accepts a plain number, a numeric string with a Celsius suffix,
// or the first numeric token found in a non-JSON text payload.
bool parseTempC(const String& payload, float& outC);

// Extract a temperature from a payload with an optional JSON key/path.
//
// Supported examples:
//   "23.4"
//   "23.4 C"
//   {"temperature":23.4}
//   {"sensor":{"temperature":"23.4 C"}} with key "sensor.temperature"
//   [10.0, 23.4] with key "1"
//
// When jsonKey is empty, common keys are auto-detected:
// tempC, temperature, temp, t and value.
bool tempParseFromPayload(const String& payload,
                          const String& jsonKey,
                          float& outTempC);
