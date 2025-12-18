#include "Tca9554.h"

Tca9554::Tca9554()
    : _wire(nullptr),
      _addr(0x20),
      _outputState(0x00)
{
}

bool Tca9554::writeReg(uint8_t reg, uint8_t value) {
    if (!_wire) return false;

    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(value);
    return (_wire->endTransmission() == 0);
}

bool Tca9554::readReg(uint8_t reg, uint8_t &value) {
    if (!_wire) return false;

    _wire->beginTransmission(_addr);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) {
        return false;
    }

    if (_wire->requestFrom((int)_addr, 1) != 1) {
        return false;
    }

    value = _wire->read();
    return true;
}

bool Tca9554::begin(TwoWire &wire, uint8_t address) {
    _wire  = &wire;
    _addr  = address;
    _outputState = 0x00;

    // Nastavíme všechny piny jako výstupy (CONFIG = 0x00)
    if (!writeReg(0x03, 0x00)) {
        return false;
    }

    // Vynulujeme výstupy
    if (!writeReg(0x01, _outputState)) {
        return false;
    }

    return true;
}

void Tca9554::setAllOutputs(uint8_t value) {
    _outputState = value;
    writeReg(0x01, _outputState);
}

void Tca9554::setPin(uint8_t pin, bool level) {
    if (pin > 7) return;

    if (level) {
        _outputState |= (1 << pin);
    } else {
        _outputState &= ~(1 << pin);
    }

    writeReg(0x01, _outputState);
}

bool Tca9554::getPin(uint8_t pin) {
    if (pin > 7) return false;

    uint8_t v = 0;
    if (!readReg(0x01, v)) {
        // když čtení selže, vrátíme stav z našeho shadowu
        return (_outputState & (1 << pin)) != 0;
    }

    _outputState = v;
    return (v & (1 << pin)) != 0;
}
