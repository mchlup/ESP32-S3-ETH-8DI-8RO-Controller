#pragma once
#include <Arduino.h>
#include "InputController.h"

// Systémové režimy
// První tři jsou historicky MODE1 / TOPENÍ, další dva jsou volitelné uživatelské režimy
enum class SystemMode : uint8_t {
    MODE1 = 0,
    MODE2,
    MODE3,
    MODE4,
    MODE5
};

// Způsob řízení režimu
// AUTO   = podle vstupů (relayMap z UI) – žádné pevné mapování režimů ve firmware
// MANUAL = řídí se příkazy z WebUI / API
enum class ControlMode : uint8_t {
    AUTO = 0,
    MANUAL
};

void logicInit();
void logicUpdate();

// volá InputController při změně vstupu
void logicOnInputChanged(InputId id, bool newState);

// aktuální režim
SystemMode logicGetMode();
const char* logicModeToString(SystemMode mode);

// řízení režimu
ControlMode logicGetControlMode();
void logicSetControlMode(ControlMode mode);

// nastavení manuálního režimu (např. z WebUI / API)
// - pokud je ControlMode::MANUAL, rovnou aplikuje stavy na relé
bool logicSetManualMode(SystemMode mode);

// nastavení manuálního režimu podle textového ID ("MODE1", "MODE2", "MODE3")
bool logicSetManualModeByName(const String& name);

// aplikace relayMap na relé podle vstupů (používá se v AUTO režimu)
void logicRecomputeFromInputs();

// aplikace konfigurace z JSON (používá se pro načtení profilů režimů z /config.json)
void logicApplyConfig(const String& json);
// Diagnostika AUTO (aby UI vědělo, co AUTO právě používá)
struct AutoStatus {
    bool      triggered;       // true = aktivní spouštěcí vstup režimu
    uint8_t   triggerInput;    // 1..8, 0 = žádný
    SystemMode triggerMode;    // platí jen při triggered=true
    bool      usingRelayMap;   // true = bez triggeru (použito relayMap)
    bool      blockedByRules;  // true = Rule engine je povolený (legacy AUTO se neaplikuje)
};

AutoStatus logicGetAutoStatus();
bool logicGetAutoDefaultOffUnmapped();
