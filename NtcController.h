#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdint.h>

enum NtcStatus : uint8_t {
  NTC_DISABLED = 0,
  NTC_OK,
  NTC_ERROR
};

struct NtcReading {
  uint8_t gpio;
  float   temperature;
  NtcStatus status;
};

class NtcController {
public:
  static void begin();
  static void configureGpio(uint8_t gpio, bool enable);
  static void loop();
  static NtcReading get(uint8_t gpio);
};

// Legacy/simple API used across the project
void ntcInit();
void ntcLoop();
void ntcApplyConfig(const String& json);
void ntcApplyConfig(const JsonObjectConst& json);
bool ntcIsEnabled(uint8_t inputIndex);
uint8_t ntcGetGpio(uint8_t inputIndex);
bool ntcIsValid(uint8_t inputIndex);
float ntcGetTempC(uint8_t inputIndex);
int ntcGetRaw(uint8_t inputIndex);
