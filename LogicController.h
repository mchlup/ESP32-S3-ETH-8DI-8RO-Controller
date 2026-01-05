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

// Teplotní senzory (TEMP1..TEMP8) – index 0..7
bool  logicIsTempValid(uint8_t idx);
float logicGetTempC(uint8_t idx);

// Equitherm – stav pro UI/MQTT (výpočet cílové teploty)
struct EquithermStatus {
    bool  enabled = false;
    bool  active  = false;
    bool  night   = false;

    // Sources + measured temps
    float outdoorC = NAN;
    bool  outdoorValid = false;
    uint32_t outdoorAgeMs = 0;
    String outdoorReason = "";
    float flowC    = NAN;     // teplota otopné vody / okruhu (feedback)
    float targetFlowC = NAN;  // požadovaná teplota podle křivky
    float actualC = NAN;      // alias pro flowC (pro API)
    float targetC = NAN;      // alias pro targetFlowC (pro API)
    bool  akuSupportActive = false;
    String akuSupportReason = "";
    float akuTopC = NAN;
    float akuMidC = NAN;
    float akuBottomC = NAN;
    bool  akuTopValid = false;
    bool  akuMidValid = false;
    bool  akuBottomValid = false;

    // Control (mixing valve)
    uint8_t valveMaster = 0;      // 1..8, 0 = none
    uint8_t valvePosPct = 0;      // 0..100 (odhad pozice)
    uint8_t valveTargetPct = 0;   // 0..100 (poslední požadavek)
    bool    valveMoving = false;
    uint32_t lastAdjustMs = 0;    // millis() poslední korekce (0 = nikdy)

    // Optional diagnostic
    String reason = "";
};
EquithermStatus logicGetEquithermStatus();
String logicGetEquithermReason();

// Nastavení výstupu z UI/API (respektuje šablony, např. 3c ventil)
void logicSetRelayOutput(uint8_t relay1based, bool on);
// RAW ovládání relé (použito pro kalibrace; obejde šablony, ale drží bezpečnost A/B páru)
void logicSetRelayRaw(uint8_t relay1based, bool on);



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

// Virtual functions controlled by schedules / external demand
bool logicGetTuvEnabled();
bool logicGetNightMode();
bool logicGetHeatCallActive();

bool logicGetAutoDefaultOffUnmapped();

// TUV status (pro dashboard / UI)
struct TuvStatus {
    bool enabled = false;        // schedule enabled AND (demand active if demand input configured)
    bool active = false;
    bool scheduleEnabled = false;
    bool demandActive = false;
    bool modeActive = false;     // TUV režim aktivní (přepnutí ventilu)
    String reason = "";
    String source = "";
    bool boilerRelayOn = false;
    uint8_t eqValveMaster = 0;   // 1..8 (ekvitermní ventil)
    uint8_t eqValveTargetPct = 0;
    uint8_t eqValveSavedPct = 0;
    bool    eqValveSavedValid = false;
    uint8_t valveMaster = 0;     // 1..8 (TUV přepínací ventil)
    uint8_t valveTargetPct = 0;
    uint8_t valvePosPct = 0;
    bool    valveMoving = false;
    uint8_t bypassPct = 0;
    uint8_t chPct = 0;
    bool    bypassInvert = false;
    String  valveMode = "";
};
TuvStatus logicGetTuvStatus();

// Smart cirkulace TUV
struct RecircStatus {
    bool enabled = false;
    bool active = false;
    String mode = "";
    uint32_t untilMs = 0;
    uint32_t remainingMs = 0;
    bool stopReached = false;
    float returnTempC = NAN;
    bool returnTempValid = false;
};
RecircStatus logicGetRecircStatus();

// AKU heater status
struct AkuHeaterStatus {
    bool enabled = false;
    bool active = false;
    String mode = "";
    String reason = "";
    float topC = NAN;
    bool topValid = false;
};
AkuHeaterStatus logicGetAkuHeaterStatus();

// Trojcestný ventil – stav pro dashboard (V2)
struct ValveUiStatus {
    uint8_t master = 0;     // 1..8
    uint8_t peer = 0;       // 1..8 (0 = none)
    uint8_t posPct = 0;     // 0..100
    bool moving = false;
    bool targetB = false;   // legacy (A/B)
    uint8_t targetPct = 0;  // 0..100 (pokud firmware řídí procenty)
};

bool logicGetValveUiStatus(uint8_t relay1based, ValveUiStatus& out);
bool logicIsValvePeer(uint8_t relay1based);
bool logicIsValveMaster(uint8_t relay1based);
bool logicSetValveTargetPct(uint8_t relay1based, uint8_t targetPct);
bool logicCommandValve(uint8_t relay1based, const String& cmd);

// MQTT: seznam topiců, které je potřeba odebírat (např. Ekviterm zdroje)
// outTopics může být nullptr pro zjištění počtu.
uint8_t logicGetMqttSubscribeTopics(String* outTopics, uint8_t maxTopics);
