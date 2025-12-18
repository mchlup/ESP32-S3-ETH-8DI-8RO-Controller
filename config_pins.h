#pragma once
#include <Arduino.h>

// ==============================
// ESP32-S3-POE-ETH-8DI-8DO – PINY
// ==============================
//
// Digitální vstupy (DI1..DI8) – podle dokumentace Waveshare:
//   DI1 → GPIO4
//   DI2 → GPIO5
//   DI3 → GPIO6
//   DI4 → GPIO7
//   DI5 → GPIO8
//   DI6 → GPIO9
//   DI7 → GPIO10
//   DI8 → GPIO11
//
// Výstupy (EXIO1..EXIO8) nejsou v dokumentaci přímo svázané s GPIO,
// proto je nechávám jako "NEPOUŽITÝ PIN" a skutečné ovládání se řeší
// v RelayControlleru (centrálně – viz komentáře tam).

// ---- Digitální vstupy ----
static const uint8_t INPUT1_PIN = 4;   // DI1
static const uint8_t INPUT2_PIN = 5;   // DI2
static const uint8_t INPUT3_PIN = 6;   // DI3
static const uint8_t INPUT4_PIN = 7;   // DI4
static const uint8_t INPUT5_PIN = 8;   // DI5
static const uint8_t INPUT6_PIN = 9;   // DI6
static const uint8_t INPUT7_PIN = 10;  // DI7
static const uint8_t INPUT8_PIN = 11;  // DI8

// WS2812 RGB LED – ovládací pin (GPIO38 podle dokumentace Waveshare)
static const uint8_t RGB_LED_PIN = 38;

// Speciální hodnota znamenající "kanál relé nemá přiřazený žádný GPIO pin",
// tzn. hardware ovládáš jiným způsobem (např. knihovnou od Waveshare).
static const uint8_t RELAY_PIN_UNUSED = 0xFF;

// ---- Digitální výstupy (relé kanály 1–8) ----
//
// ⚠️ DŮLEŽITÉ:
//   Na této desce se EXIO1..EXIO8 NEPŘIPOJUJÍ přímo na běžná GPIO jako
//   u D1 mini. Ovládání relé je typicky řešené přes IO expander / driver
//   v jejich knihovně.
//
//   Proto jsou tady piny nastavené na RELAY_PIN_UNUSED – v RelayControlleru
//   se s tím počítá a nebude se volat pinMode/digitalWrite.
//
//   Jakmile zjistíš, jak se u tebe EXIO1..8 ovládají (nebo použiješ
//   jejich demo knihovnu), můžeš:
//     - buď tyto konstanty nastavit na reálná GPIO,
//     - nebo v RelayController.cpp nahradit volání boardRelayWrite()
//       vlastní implementací a piny vůbec nepotřebovat.

static const uint8_t RELAY1_PIN = RELAY_PIN_UNUSED;
static const uint8_t RELAY2_PIN = RELAY_PIN_UNUSED;
static const uint8_t RELAY3_PIN = RELAY_PIN_UNUSED;
static const uint8_t RELAY4_PIN = RELAY_PIN_UNUSED;
static const uint8_t RELAY5_PIN = RELAY_PIN_UNUSED;
static const uint8_t RELAY6_PIN = RELAY_PIN_UNUSED;
static const uint8_t RELAY7_PIN = RELAY_PIN_UNUSED;
static const uint8_t RELAY8_PIN = RELAY_PIN_UNUSED;
