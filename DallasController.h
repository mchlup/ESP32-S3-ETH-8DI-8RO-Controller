#pragma once

#include <Arduino.h>
#include <vector>

enum TempInputType : uint8_t {
  TEMP_INPUT_NONE = 0,
  TEMP_INPUT_AUTO,
  TEMP_INPUT_DALLAS
};

enum TempInputStatus : uint8_t {
  TEMP_STATUS_DISABLED = 0,
  TEMP_STATUS_OK,
  TEMP_STATUS_NO_SENSOR,
  TEMP_STATUS_BUS_ERROR,
  TEMP_STATUS_ERROR
};

struct DallasDeviceInfo {
  uint64_t rom = 0;
  uint64_t address = 0;
  float    temperature = NAN;
  bool     valid = false;
};

struct DallasGpioStatus {
  uint8_t gpio = 255;
  TempInputStatus status = TEMP_STATUS_DISABLED;
  std::vector<DallasDeviceInfo> devices;
  uint32_t lastReadMs = 0;
};

namespace DallasController {
  bool gpioSupportsDallas(uint8_t gpio);
  void begin();
  void configureGpio(uint8_t gpio, TempInputType type);
  void loop();
  const DallasGpioStatus* getStatus(uint8_t gpio);
}

void dallasApplyConfig(const String& jsonStr);
bool dallasIsValid(uint8_t inputIndex);
float dallasGetTempC(uint8_t inputIndex);

uint64_t dallasGetSlotRom(uint8_t inputIndex);
bool dallasTryGetSlotRom(uint8_t inputIndex, uint64_t& outRom);
String dallasGetSlotRomHex(uint8_t inputIndex);
