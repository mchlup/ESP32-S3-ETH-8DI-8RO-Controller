#include "RelayController.h"

#include <Wire.h>
#include "config_pins.h"

// ===== Hardwarové parametry TCA9554 na Waveshare ESP32-S3-ETH-8DI-8RO =====
//
// I2C piny a adresa expanderu:
//
//  SDA = GPIO42
//  SCL = GPIO41
//  adresa = 0x20
//
// Pokud jsou v config_pins.h definované I2C_SDA_PIN / I2C_SCL_PIN / TCA9554_ADDR,
// použijeme je. Jinak použijeme tyto defaulty.

#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 42
#endif

#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 41
#endif

#ifndef TCA9554_ADDR
#define TCA9554_ADDR 0x20
#endif

// Registry TCA9554 / PCA9554
static constexpr uint8_t REG_INPUT        = 0x00;
static constexpr uint8_t REG_OUTPUT       = 0x01;
static constexpr uint8_t REG_POLARITY     = 0x02;
static constexpr uint8_t REG_CONFIGURATION= 0x03;

// Shadow stavy relé (true = zapnuto)
static bool   g_relayStates[RELAY_COUNT] = { false };
static bool   g_expanderReady = false;
static uint8_t g_outputMask   = 0x00;

// ----- nízkoúrovňové I2C funkce -----

static bool tcaWriteReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return (Wire.endTransmission() == 0);
}

// ----- mapování a zápis do HW -----

// Převod RelayId -> index 0..7
static bool relayIdToIndex(RelayId id, uint8_t &index) {
    uint8_t v = static_cast<uint8_t>(id);
    if (v >= RELAY_COUNT) return false;
    index = v;
    return true;
}

// Aplikace shadow masky do expanderu
static void applyOutputs() {
    if (!g_expanderReady) return;
    tcaWriteReg(REG_OUTPUT, g_outputMask);
}

// ----- veřejné API -----

void relayInit() {
    Serial.println(F("[RELAY] Init: TCA9554 via I2C"));

    // Inicializace I2C sběrnice
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    // Všechny piny jako výstupy (0 = output)
    bool ok = true;
    ok = ok && tcaWriteReg(REG_CONFIGURATION, 0x00);

    // Bez inverze polarity (0 = normální, 1 = invert)
    ok = ok && tcaWriteReg(REG_POLARITY, 0x00);

    // Vypnout všechna relé
    g_outputMask = 0x00;
    ok = ok && tcaWriteReg(REG_OUTPUT, g_outputMask);

    g_expanderReady = ok;

    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        g_relayStates[i] = false;
    }

    if (g_expanderReady) {
        Serial.println(F("[RELAY] TCA9554 ready, all relays OFF"));
    } else {
        Serial.println(F("[RELAY] ERROR: TCA9554 init failed, relays will stay logically OFF"));
    }
}

void relayUpdate() {
    // Zatím žádná periodická logika
}

void relaySet(RelayId id, bool on) {
    uint8_t index = 0;
    if (!relayIdToIndex(id, index)) return;

    g_relayStates[index] = on;

    // Nastavení bitu v masce
    if (on) {
        g_outputMask |=  (1u << index);
    } else {
        g_outputMask &= ~(1u << index);
    }

    applyOutputs();
}

void relayToggle(RelayId id) {
    bool current = relayGetState(id);
    relaySet(id, !current);
}

bool relayGetState(RelayId id) {
    uint8_t index = 0;
    if (!relayIdToIndex(id, index)) return false;
    return g_relayStates[index];
}

void relayPrintStates(Stream &out) {
    out.print(F("[RELAY] States: "));
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        out.print(F("R"));
        out.print(i + 1);
        out.print(F("="));
        out.print(g_relayStates[i] ? F("ON") : F("OFF"));
        if (i < (RELAY_COUNT - 1)) {
            out.print(F(", "));
        }
    }
    out.println();
}
