#pragma once
#include <Arduino.h>

// 8 vstupů (DI1..DI8) na desce ESP32-S3-POE-ETH-8DI-8DO
enum InputId : uint8_t {
    INPUT_1 = 0,   // původně TOPENÍ/MODE1
    INPUT_2,       // původně TOPIT/UTLUM
    INPUT_3,
    INPUT_4,
    INPUT_5,
    INPUT_6,
    INPUT_7,
    INPUT_8,
    INPUT_COUNT
};

// Inicializace modulu
void inputInit();

// Musí být voláno pravidelně v loop() (neblokující)
void inputUpdate();

// Získání aktuálního fyzického stavu (RAW z GPIO, HIGH/LOW)
bool inputGetRaw(InputId id);

// Získání aktuálního logického stavu (po aplikaci polarity z configu)
// true  = vstup je logicky AKTIVNÍ
// false = vstup je logicky NEAKTIVNÍ
bool inputGetState(InputId id);

// Callback – volaný při změně logického stavu vstupu
typedef void (*InputChangeCallback)(InputId id, bool newState);
void inputSetCallback(InputChangeCallback cb);
