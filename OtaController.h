#pragma once

#include <Arduino.h>

// ArduinoOTA wrapper (neblokující)
// - OTA::init() je zachováno kvůli kompatibilitě se starším kódem
// - preferované je OTA::begin(hostname, password)

namespace OTA {

// Spustí ArduinoOTA.
// Pokud je hostname prázdný, vygeneruje se výchozí "ESP-HeatCtrl-XXXXXX" podle MAC.
void begin(const String& hostname = String(), const String& password = String());

// Kompatibilita se starším kódem (ESP-D1-HeatControl.ino volal OTA::init()).
inline void init() { begin(); }

// Volat pravidelně v loop() – obsluhuje OTA.
void loop();

} // namespace OTA
