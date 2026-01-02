#pragma once
#include <Arduino.h>
// JsonObject je z ArduinoJson – musí být známý už v hlavičce
#include <ArduinoJson.h>

// Ne-blokující BUZZER modul (GPIO46)
void buzzerInit();
void buzzerLoop();

// Patterny (pro API i interní použití)
void buzzerPlayPatternByName(const String& name);
void buzzerStop();

// Systémové události (volá se z logiky / relé)
void buzzerOnControlModeChanged(bool isAuto);
void buzzerOnManualModeChanged(const String& modeName);
void buzzerOnRelayChanged(uint8_t relay, bool on);
void buzzerOnError(const String& code);

// API JSON (WebServerController)
void buzzerToJson(String& outJson);
void buzzerUpdateFromJson(const JsonObject& cfg);
bool buzzerSaveToFS();
bool buzzerLoadFromFS();