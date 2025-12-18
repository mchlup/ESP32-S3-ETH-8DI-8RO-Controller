#pragma once
#include <Arduino.h>

// Jednoduchý neblokující "driver" pro 1× WS2812 RGB LED (GPIO dle config_pins.h).
// Umožňuje přepínat předdefinované režimy (např. BLE párování/připojeno).

enum class RgbLedMode : uint8_t {
    OFF = 0,
    SOLID,          // trvalá barva (rgbLedSetColor)
    BLE_IDLE,       // slabá modrá (BLE povoleno, bez klienta)
    BLE_PAIRING,    // bliká modře (párovací okno)
    BLE_CONNECTED,  // trvalá modrá (klient připojen)
    BLE_DISABLED,   // zhasnuto
    ERROR,          // bliká červeně
};

void rgbLedInit();
void rgbLedLoop();

// Nastavení barvy (0–255) a režimu SOLID
void rgbLedSetColor(uint8_t r, uint8_t g, uint8_t b);

// Nastavení předdefinovaného režimu
void rgbLedSetMode(RgbLedMode mode);

// Pro BLE modul – helper (jen mapuje na režimy výše)
inline void rgbLedSetBleEnabled(bool enabled) {
    if (!enabled) rgbLedSetMode(RgbLedMode::BLE_DISABLED);
    else rgbLedSetMode(RgbLedMode::BLE_IDLE);
}
