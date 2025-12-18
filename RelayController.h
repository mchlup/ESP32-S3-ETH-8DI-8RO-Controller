#pragma once

#include <Arduino.h>

// Počet relé na desce ESP32-S3-ETH-8DI-8RO
#define RELAY_COUNT 8

// ID jednotlivých relé (indexy 0..7)
enum RelayId : uint8_t {
    RELAY_1 = 0,
    RELAY_2,
    RELAY_3,
    RELAY_4,
    RELAY_5,
    RELAY_6,
    RELAY_7,
    RELAY_8
};

// Inicializace relé (I2C expander TCA9554)
void relayInit();

// Pokud někde používáš, můžeš ji volat v loopu – zatím prázdná, ponecháno pro konzistenci
void relayUpdate();

// Nastavení stavu konkrétního relé
void relaySet(RelayId id, bool on);

// Přepnutí stavu (toggle)
void relayToggle(RelayId id);

// Získání aktuálního stavu relé (true = sepnuté)
bool relayGetState(RelayId id);

// Vypsání stavů všech relé do Serialu (pro debug)
void relayPrintStates(Stream &out);
    