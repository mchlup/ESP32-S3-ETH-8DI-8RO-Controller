#pragma once

#include <Arduino.h>

// Parse temperature from either:
//  - plain number string: "21.5" (optionally with whitespace / trailing unit)
//  - simple JSON: {"tempC":21.5} or {"temperature":21.5}
//  - JSON with key/path: jsonKey may be "tempC" or "sensor.tempC" or "0.temp" (arrays)
//
// Returns true when a finite float value was extracted.
bool tempParseFromPayload(const String& payload, const String& jsonKey, float& outTempC);
