#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <stdint.h>

enum TempInputType : uint8_t {
    TEMP_INPUT_NONE = 0,
    TEMP_INPUT_DALLAS,
    TEMP_INPUT_NTC,
    TEMP_INPUT_AUTO
};

enum TempInputStatus : uint8_t {
    TEMP_STATUS_DISABLED = 0,
    TEMP_STATUS_OK,
    TEMP_STATUS_NO_SENSOR,
    TEMP_STATUS_ERROR
};

struct DallasDeviceInfo {
    uint64_t rom;
    float temperature;
    bool valid;
};

struct DallasGpioStatus {
    uint8_t gpio;
    TempInputStatus status;
    std::vector<DallasDeviceInfo> devices;
    uint32_t lastReadMs;
};

class DallasController {
public:
    static void begin();
    static void loop();

    static void configureGpio(uint8_t gpio, TempInputType type);
    static const DallasGpioStatus* getStatus(uint8_t gpio);

    // Helpers
    static bool gpioSupportsDallas(uint8_t gpio);
};


// Legacy/simple API used across the project
void dallasApplyConfig(const String& json);
bool dallasIsValid(uint8_t inputIndex);
float dallasGetTempC(uint8_t inputIndex);
