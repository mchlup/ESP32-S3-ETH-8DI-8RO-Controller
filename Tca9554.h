#pragma once

#include <Arduino.h>
#include <Wire.h>

// Jednoduchý driver pro TCA/PCA9554 (8bit I/O expander)
// Registr 0x00 = Input
// Registr 0x01 = Output
// Registr 0x02 = Polarity Inversion
// Registr 0x03 = Configuration (1 = input, 0 = output)

class Tca9554 {
public:
    Tca9554();

    // Inicializace – použije se předaný TwoWire a I2C adresa
    bool begin(TwoWire &wire, uint8_t address);

    // Nastavení všech výstupů najednou (bit 0..7)
    void setAllOutputs(uint8_t value);

    // Nastavení konkrétního pinu (0..7) na HIGH/LOW
    void setPin(uint8_t pin, bool level);

    // Čtení aktuálního stavu výstupního registru bitu (přes I2C)
    bool getPin(uint8_t pin);

    // Aktuální shadow výstupní hodnota (co jsme naposledy zapsali / přečetli)
    uint8_t getOutputShadow() const { return _outputState; }

private:
    TwoWire* _wire;
    uint8_t  _addr;
    uint8_t  _outputState;

    bool writeReg(uint8_t reg, uint8_t value);
    bool readReg(uint8_t reg, uint8_t &value);
};
