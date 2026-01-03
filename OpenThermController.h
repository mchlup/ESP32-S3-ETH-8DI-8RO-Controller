#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// --- OpenTherm support (boiler comm) ---
//
// Poznámka:
// Tento modul je zatím navržen tak, aby projekt šel zkompilovat a UI mělo
// konfigurační + status API. Samotná fyzická komunikace OpenTherm vyžaduje
// transceiver (OT interface) a doplnění konkrétního driveru (ESP32-S3).
//
// Rozhraní je připravené pro budoucí rozšíření (čtení/zápis rámců, polling,...)
// a současně umožňuje navázat řízení setpointu z ekvitermu.

enum class OpenThermMode : uint8_t {
    OFF = 0,
    MANUAL = 1,
    EQUITHERM = 2,
};

struct OpenThermCfg {
    bool enabled = false;
    OpenThermMode mode = OpenThermMode::OFF;

    // GPIO piny pro OT interface (logická úroveň dle použitého transceiveru)
    int8_t inPin = -1;
    int8_t outPin = -1;

    uint32_t pollIntervalMs = 1000;

    bool chEnable = true;
    float manualSetpointC = 45.0f;

    // ochrana proti spamování (zápis setpointu)
    float minDeltaWriteC = 0.5f;
    uint32_t minWriteIntervalMs = 5000;
};

struct OpenThermStatus {
    bool enabled = false;
    OpenThermMode mode = OpenThermMode::OFF;

    bool ready = false;
    uint32_t lastPollMs = 0;
    uint32_t lastWriteMs = 0;

    // poslední nastavený setpoint (CH)
    float setpointC = NAN;

    // telemetrie (pokud dostupná)
    float boilerTempC = NAN;
    float returnTempC = NAN;
    float modulationPct = NAN;
    uint16_t faultCode = 0;

    uint32_t okFrames = 0;
    uint32_t errFrames = 0;

    String lastError;
};

void openthermInit();
void openthermApplyConfig(const String& json);
void openthermLoop();

OpenThermCfg openthermGetConfig();
OpenThermStatus openthermGetStatus();

// Fill status into already created JSON object
void openthermFillJson(JsonObject obj);

// Force setpoint write (manual override)
bool openthermRequestSetpoint(float tempC);
