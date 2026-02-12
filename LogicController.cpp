#include "LogicController.h"
#include "RelayController.h"
#include "InputController.h"
#include "ConfigStore.h"
#include "Features.h"
#include "DallasController.h"
#include "MqttController.h"
// OpenTherm is pulled via Features.h (optional)

#include "ThermometerController.h"
#include "ThermoRoles.h"
#include "TempParse.h"

#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include "BleController.h"

// ============================================================================
// FIXED HW MAPPING (Waveshare ESP32-S3-POE-ETH-8DI-8DO)
//
// Požadavek projektu: mapování relé/vstupů je pevně dané ve firmwaru.
//
// Relé (R1..R8):
//  R1 = 3c směšovací ventil (směr A)
//  R2 = 3c směšovací ventil (směr B)
//  R3 = 3c přepínací ventil (0%/100%) – single relay (pružina nebo interní návrat)
//  R4 = cirkulační čerpadlo
//  R5 = požadavek ohřevu TUV do kotle (DHW request)
//  R6 = požadavek Denní/Noční křivka do kotle
//  R7 = Omezovací relé výkonu kotle (rezerva)
//  R8 = stykač topné spirály akumulační nádrže
//
// Vstupy (IN1..IN8):
//  IN1 = požadavek ohřevu TUV (DHW demand)
//  IN2 = přepnutí Denní/Noční křivka (aktivní = noční)
//  IN3 = požadavek cirkulace
//  IN4..IN8 = REZERVA
// ============================================================================
static constexpr uint8_t FIX_RELAY_MIX_A0           = 0; // R1
static constexpr uint8_t FIX_RELAY_MIX_B0           = 1; // R2
static constexpr uint8_t FIX_RELAY_TUV_VALVE0       = 2; // R3
static constexpr uint8_t FIX_RELAY_RECIRC_PUMP0     = 3; // R4
static constexpr uint8_t FIX_RELAY_BOILER_DHW_REQ0  = 4; // R5
static constexpr uint8_t FIX_RELAY_BOILER_NIGHT0    = 5; // R6
static constexpr uint8_t FIX_RELAY_RESERVED_7_0     = 6; // R7 (rezerva / omezovací relé)
static constexpr uint8_t FIX_RELAY_AKU_HEATER0      = 7; // R8

// Vstupy dle zadání projektu:
//  IN1 = požadavek ohřevu TUV (DHW demand)
//  IN2 = přepnutí Denní/Noční křivka (aktivní = noční)
//  IN3 = požadavek cirkulace
static constexpr int8_t  FIX_IN_NIGHT_MODE0         = 1; // IN2
static constexpr int8_t  FIX_IN_DHW_DEMAND0         = 0; // IN1
static constexpr int8_t  FIX_IN_RECIRC_DEMAND0      = 2; // IN3 (požadavek cirkulace)


static inline bool isFixedFunctionRelay(uint8_t r0) {
    // Pevně vyhrazené systémové relé (nelze použít pro relayMap/profily ani 3c směšovací ventil).
    // R3 = TUV přepínací ventil (single relay, fix), R4..R8 = recirc/boiler/AKU.
    return (r0 == FIX_RELAY_TUV_VALVE0 ||
            r0 == FIX_RELAY_BOILER_DHW_REQ0 || r0 == FIX_RELAY_BOILER_NIGHT0 ||
            r0 == FIX_RELAY_RECIRC_PUMP0 || r0 == FIX_RELAY_RESERVED_7_0 || r0 == FIX_RELAY_AKU_HEATER0);
}

// Každý režim má profil pro všechna relé (RELAY_COUNT = 8).

// Profil relé pro režim – 8 kanálů
struct RelayProfile {
    bool states[RELAY_COUNT];
};

// Výchozí profily (pro případ, že config.json chybí nebo je neúplný)
//  - MODE1:       R1,R2,R4 zapnuté
//  - MODE2:     R3 zapnuté
//  - MODE3:     všechna vypnutá
static RelayProfile profileMODE1 = {
    { false,  false,  false, false,  false, false, false, false }
};

static RelayProfile profileTopeniTopit = {
    { false, false, false,  false, false, false, false, false }
};

static RelayProfile profileTopeniUtlum = {
    { false, false, false, false, false, false, false, false }
};

static RelayProfile profileMode4 = {
    { false, false, false, false, false, false, false, false }
};

static RelayProfile profileMode5 = {
    { false, false, false, false, false, false, false, false }
};

// Spouštěcí vstup pro jednotlivé režimy (0 = žádný).
// Pořadí indexů odpovídá SystemMode: MODE1(0), MODE2(1), MODE3(2), MODE4(3), MODE5(4).
static uint8_t modeTriggerInput[5] = { 0, 0, 0, 0, 0 };

// Když v AUTO neplatí žádný "trigger mode" a relé nemá přiřazený vstup v relayMap:
// - true  => relé se vynutí do OFF (nebude "viset" v ON po uvolnění vstupu)
// - false => relé se ponechá ve stavu, v jakém je.
static bool s_autoDefaultOffUnmapped = true;

// Diagnostika AUTO (status pro UI)
static AutoStatus s_autoStatus = { false, 0, SystemMode::MODE1, false };

// Aktuální řízení a režim
static ControlMode currentControlMode = ControlMode::AUTO;
static SystemMode  manualMode         = SystemMode::MODE1;
static SystemMode  currentMode        = SystemMode::MODE1;



// RelayMap: z UI mapování vstupu na relé
struct RelayMapItem {
    uint8_t input;     // 1..INPUT_COUNT, 0 = none
    uint8_t polarity;  // 1 = ON když vstup ACTIVE, 0 = invert
};
static RelayMapItem relayMap[RELAY_COUNT];

// Teploty TEMP1..TEMP8 (typicky vstupy s rolí temp_*)
static bool  s_tempValid[INPUT_COUNT] = { false,false,false,false,false,false,false,false };
static float s_tempC[INPUT_COUNT] = { NAN,NAN,NAN,NAN,NAN,NAN,NAN,NAN };

// 3c ventil (2 relé A/B) – stavový automat (neblokující)
struct Valve3WayState {
    bool configured = false;
    bool singleRelay = false;
    uint8_t relayA = 0;  // 0..7 (směr A)
    uint8_t relayB = 0;  // 0..7 (směr B)
    uint32_t travelMs = 6000;   // doba přeběhu ventilu (ms)
    uint32_t pulseMs  = 600;    // krátký puls pro test/kalibraci (ms)
    uint32_t guardMs  = 300;    // pauza mezi směry (ms)

    // Min. perioda mezi starty přestavení (ochrana proti častému přepínání)
    uint32_t minSwitchMs = 0;   // 0 = bez omezení
    uint32_t lastCmdMs = 0;
    bool hasPending = false;
    uint8_t pendingTargetPct = 0;
    bool invertDir = false;     // prohození A/B
    bool defaultB = false;      // výchozí poloha po startu / applyConfig
    bool currentB = false;      // posledně požadovaná poloha (B=true, A=false)

    // pro dashboard – odhad pozice 0..100%
    uint8_t posPct = 0;
    uint8_t startPct = 0;
    uint8_t targetPct = 0;
    uint32_t moveStartMs = 0;

    bool moving = false;
    uint32_t moveEndMs = 0;
    uint32_t guardEndMs = 0;
};
static Valve3WayState s_valves[RELAY_COUNT];
static int8_t s_valvePeerOf[RELAY_COUNT] = { -1,-1,-1,-1,-1,-1,-1,-1 };

static void valveResetAll() {
    for (uint8_t i=0;i<RELAY_COUNT;i++){
        s_valves[i] = Valve3WayState{};
        s_valvePeerOf[i] = -1;
    }
}

static bool isValveMaster(uint8_t r0){ return r0 < RELAY_COUNT && s_valves[r0].configured; }
static bool isValvePeer(uint8_t r0){ return r0 < RELAY_COUNT && s_valvePeerOf[r0] >= 0; }

static float jsonGetFloat2(JsonObject obj, const char* keyA, const char* keyB, float defVal) {
    if (!obj.isNull()) {
        if (keyA && obj.containsKey(keyA)) {
            JsonVariant v = obj[keyA];
            if (!v.isNull()) return v.as<float>();
        }
        if (keyB && obj.containsKey(keyB)) {
            JsonVariant v = obj[keyB];
            if (!v.isNull()) return v.as<float>();
        }
    }
    return defVal;
}

static uint8_t valveComputePosPct(const Valve3WayState& v, uint32_t nowMs){
    // Udržujeme posPct jako poslední známou / dopočtenou pozici (0..100),
    // aby šlo ventil řídit i procenty (ekviterm).
    if (!v.moving) return v.posPct;

    if ((int32_t)(nowMs - v.guardEndMs) < 0) return v.startPct;
    if ((int32_t)(nowMs - v.moveEndMs) >= 0) return v.targetPct;

    const float travel = (float)(v.moveEndMs - v.moveStartMs);
    if (travel <= 0.0f) return v.targetPct;

    float p = (float)(nowMs - v.moveStartMs) / travel;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;

    const float val = (float)v.startPct + ((float)v.targetPct - (float)v.startPct) * p;
    int iv = (int)lroundf(val);
    if (iv < 0) iv = 0;
    if (iv > 100) iv = 100;
    return (uint8_t)iv;
}

static inline bool valveCanStartNow(const Valve3WayState &v, uint32_t nowMs){
    if (v.minSwitchMs == 0) return true;
    return (uint32_t)(nowMs - v.lastCmdMs) >= v.minSwitchMs;
}

static void valveStartMoveInternal(Valve3WayState &v, uint8_t internalTargetPct, uint32_t nowMs){
    if (internalTargetPct > 100) internalTargetPct = 100;

    const uint8_t curPct = valveComputePosPct(v, nowMs);

    // vždy nejdřív vypnout obě (bezpečné při změně směru)
    relaySet(static_cast<RelayId>(v.relayA), false);
    relaySet(static_cast<RelayId>(v.relayB), false);

    // pokud jsme (prakticky) v cíli a nic neběží, jen zafixuj stav
    if (!v.moving && curPct == internalTargetPct){
        v.moving = false;
        v.posPct = internalTargetPct;
        v.startPct = internalTargetPct;
        v.targetPct = internalTargetPct;
        v.currentB = (internalTargetPct >= 50);
        return;
    }

    v.moving = true;
    v.startPct = curPct;
    v.posPct = curPct;
    v.targetPct = internalTargetPct;

    // směr: cílová > aktuální => jedeme směrem B (sepni relayB)
    v.currentB = (internalTargetPct > curPct);

    v.guardEndMs = nowMs + v.guardMs;
    v.moveStartMs = v.guardEndMs;

    const float frac = fabsf((float)internalTargetPct - (float)curPct) / 100.0f;
    uint32_t durMs = (uint32_t)lroundf((float)v.travelMs * frac);
    if (durMs < 50) durMs = 50;
    v.moveEndMs  = v.guardEndMs + durMs;
}

static void valveCommand(uint8_t masterA0, bool wantB){
    if (!isValveMaster(masterA0)) return;
    Valve3WayState &v = s_valves[masterA0];
    // invertDir: prohození cívky A/B (nemění logickou škálu 0..100)

    const uint32_t nowMs = millis();
    const uint8_t targetPct = wantB ? 100 : 0;

    // Pokud už je v cílové poloze a nic neběží, nedrž periodu.
    if (!v.moving){
        const uint8_t curPct = valveComputePosPct(v, nowMs);
        if (curPct == targetPct) {
            v.posPct = targetPct;
            v.startPct = targetPct;
            v.targetPct = targetPct;
            v.currentB = wantB;
            v.hasPending = false;
            if (v.singleRelay) {
                // Jednocívkový přepínací ventil (spring-return apod.): drž stabilní stav relé dle cílové polohy.
                // Důležité: nesmí docházet k periodickému OFF/ON "cukání" při opakovaných povelích na stejný cíl.
                relaySet(static_cast<RelayId>(v.relayA), (wantB ^ v.invertDir));
            } else {
                relaySet(static_cast<RelayId>(v.relayA), false);
                relaySet(static_cast<RelayId>(v.relayB), false);
            }
            return;
        }
    }

    // perioda přestavení
    if (!valveCanStartNow(v, nowMs)){
        v.pendingTargetPct = targetPct;
        v.hasPending = true;
        return;
    }

    v.hasPending = false;
    v.pendingTargetPct = targetPct;
    v.lastCmdMs = nowMs;
    valveStartMoveInternal(v, targetPct, nowMs);
}

static void valveMoveToPct(uint8_t masterA0, uint8_t targetPctExt){
    if (!isValveMaster(masterA0)) return;
    Valve3WayState &v = s_valves[masterA0];

    uint8_t targetPct = targetPctExt;
    if (targetPct > 100) targetPct = 100;

    const uint32_t nowMs = millis();

    // během pohybu pouze queue
    if (v.moving){
        v.pendingTargetPct = targetPct;
        v.hasPending = true;
        return;
    }

    const uint8_t curPct = valveComputePosPct(v, nowMs);
    if (targetPct == curPct){
        v.posPct = curPct;
        v.startPct = curPct;
        v.targetPct = curPct;
        v.currentB = (curPct >= 50);
        v.hasPending = false;
        if (v.singleRelay) {
            // U jednocívkového ventilu udržuj stabilní stav relé dle cílové polohy.
            const bool holdOn = ((v.targetPct >= 50) ^ v.invertDir);
            relaySet(static_cast<RelayId>(v.relayA), holdOn);
        } else {
            relaySet(static_cast<RelayId>(v.relayA), false);
            relaySet(static_cast<RelayId>(v.relayB), false);
        }
        return;
    }

    if (!valveCanStartNow(v, nowMs)){
        v.pendingTargetPct = targetPct;
        v.hasPending = true;
        return;
    }

    v.hasPending = false;
    v.pendingTargetPct = targetPct;
    v.lastCmdMs = nowMs;
    valveStartMoveInternal(v, targetPct, nowMs);
}

static void valveTick(uint32_t nowMs){
    for (uint8_t i=0;i<RELAY_COUNT;i++){
        Valve3WayState &v = s_valves[i];
        if (!v.configured) continue;

        if (v.moving){
            // průběžná pozice pro UI
            v.posPct = valveComputePosPct(v, nowMs);

            // guard: drž obě vypnuté
            if ((int32_t)(nowMs - v.guardEndMs) < 0){
                relaySet(static_cast<RelayId>(v.relayA), false);
                relaySet(static_cast<RelayId>(v.relayB), false);
            } else {
                if (v.singleRelay) {
                    // Jednocivkovy/prepinaci ventil (spring-return apod.): ovladame pouze relayA.
                    // relayB muze byt shodne s relayA (kvuli legacy nastaveni), proto na nej nesahame,
                    // jinak by doslo k okamzitemu vypnuti vystupu.
                    relaySet(static_cast<RelayId>(v.relayA), (v.currentB ^ v.invertDir));
                } else {
                    // během pohybu je sepnutá jen správná cívka
                    const bool wantB = v.currentB;
                    const bool coilB = v.invertDir ? !wantB : wantB;
                    const uint8_t dir = coilB ? v.relayB : v.relayA;
                    const uint8_t other = coilB ? v.relayA : v.relayB;
                    relaySet(static_cast<RelayId>(other), false);
                    relaySet(static_cast<RelayId>(dir), true);
                }
            }

            // konec pohybu
            if ((int32_t)(nowMs - v.moveEndMs) >= 0){
                if (v.singleRelay) {
                    // U jednocívkového ventilu po doběhu nepřepínej relé OFF, pokud má zůstat v poloze B.
                    const bool holdOn = ((v.targetPct >= 50) ^ v.invertDir);
                    relaySet(static_cast<RelayId>(v.relayA), holdOn);
                } else {
                    relaySet(static_cast<RelayId>(v.relayA), false);
                    relaySet(static_cast<RelayId>(v.relayB), false);
                }
                v.moving = false;
                v.posPct = v.targetPct;
                v.startPct = v.targetPct;
            }
        } else {
            if (v.singleRelay) {
                const bool holdOn = ((v.targetPct >= 50) ^ v.invertDir);
                relaySet(static_cast<RelayId>(v.relayA), holdOn);
                //relaySet(static_cast<RelayId>(v.relayB), false);
            } else {
                // jistota: nic nedržet
                relaySet(static_cast<RelayId>(v.relayA), false);
                relaySet(static_cast<RelayId>(v.relayB), false);
            }
        }

        // čekající povel (po uplynutí periody)
        if (!v.moving && v.hasPending && valveCanStartNow(v, nowMs)){
            const uint8_t tgt = v.pendingTargetPct;
            v.hasPending = false;
            v.lastCmdMs = nowMs;
            valveStartMoveInternal(v, tgt, nowMs);
        }
    }
}

// Pozn.: ekviterm se aplikuje pouze v CONTROL=AUTO, ale hodnoty počítáme pro diagnostiku i v MANUAL.
struct EqSourceCfg {
    String source = "none";   // "none" | "dallas" | "temp1..temp8" | "mqtt" | "ble"
    int    gpio   = 0;        // pro dallas (0..3)
    String romHex = "";       // pro dallas (16 hex), prázdné => první validní
    String topic  = "";       // pro mqtt
    String jsonKey = "";      // pro mqtt (JSON klíč), prázdné => payload je číslo / autodetekce
    uint8_t mqttIdx = 0;      // 1..2 => použij přednastavený MQTT teploměr (z "Teploměry")
    uint32_t maxAgeMs = 0;    // 0 = bez timeoutu (pro MQTT zdroje)
    String bleId  = "";       // do budoucna
};

// TUV demand + request relay
static int8_t s_tuvDemandInput = -1;  // 0..7 or -1
static int8_t s_tuvRequestRelay = -1; // 0..7 or -1
static int8_t s_tuvEnableInput = -1;  // 0..7 or -1 (Funkce I/O -> Aktivace ohřevu TUV)
static bool s_tuvScheduleEnabled = false;
static bool s_tuvDemandActive = false;
static bool s_tuvModeActive = false;
static int8_t s_tuvValveMaster0 = -1; // 0..7 or -1 (TUV přepínací ventil)
static uint8_t s_tuvValveTargetPct = 0;
static uint8_t s_tuvEqValveTargetPct = 0;
static uint32_t s_tuvLastValveCmdMs = 0;
// Při přechodu Ekviterm -> TUV si uložíme polohu směšovacího ventilu a po ukončení TUV ji vrátíme.
static bool    s_tuvPrevModeActive    = false;
static bool    s_tuvEqValveSavedValid = false;
static uint8_t s_tuvEqValveSavedPct   = 0;
static bool    s_tuvRestoreEqValveAfter = true;
static bool    s_tuvBypassEnabled = true;
static uint8_t s_tuvBypassPct = 100;
// Pro přepínací ventil (0/100) je typicky "CH" poloha = 0% (bez napětí),
// "DHW" poloha = 100% (pod napětím). Pokud potřebuješ opak, použij invert.
static uint8_t s_tuvChPct = 0;
static bool    s_tuvBypassInvert = false;
static uint8_t s_tuvValveCurrentPct = 0;
static String  s_tuvValveMode = "ch";
static String  s_tuvReason = "off";
static String  s_tuvSource = "none";

// Smart cirkulace TUV
static bool     s_recircEnabled = false;
static String   s_recircMode = "on_demand"; // on_demand | time_windows | hybrid
static int8_t   s_recircDemandInput = -1;
static int8_t   s_recircPumpRelay = -1;
static int8_t   s_recircPumpRelayRole = -1;
static uint32_t s_recircOnDemandRunMs = 120000;
static uint32_t s_recircMinOffMs = 300000;
static uint32_t s_recircMinOnMs = 30000;
// Volitelný cyklus v rámci časových oken (time_windows/hybrid):
// například 5 min ON / 15 min OFF. 0 = vypnuto (běží kontinuálně v okně).
static uint32_t s_recircCycleOnMs = 0;
static uint32_t s_recircCycleOffMs = 0;
static EqSourceCfg
 s_recircReturnCfg;
static float    s_recircStopTempC = 42.0f;
static bool     s_recircActive = false;
static bool     s_recircStopReached = false;
static uint32_t s_recircUntilMs = 0;
static uint32_t s_recircLastOnMs = 0;
static uint32_t s_recircLastOffMs = 0;
static bool     s_recircPrevDemand = false;
static float    s_recircReturnC = NAN;
static bool     s_recircReturnValid = false;

// AKU heater
static bool     s_akuHeaterEnabled = false;
static String   s_akuHeaterMode = "manual"; // manual | schedule | thermostatic
static int8_t   s_akuHeaterRelay = -1;
static bool     s_akuHeaterManualOn = false;
static float    s_akuHeaterTargetTopC = 50.0f;
static float    s_akuHeaterHysteresisC = 2.0f;
static uint32_t s_akuHeaterMaxOnMs = 2UL * 60UL * 60UL * 1000UL;
static uint32_t s_akuHeaterMinOffMs = 10UL * 60UL * 1000UL;
static uint32_t s_akuHeaterLastOnMs = 0;
static uint32_t s_akuHeaterLastOffMs = 0;
static bool     s_akuHeaterActive = false;
static String   s_akuHeaterReason = "";

struct HeaterWindow {
    uint8_t startHour = 6;
    uint8_t startMin = 0;
    uint8_t endHour = 7;
    uint8_t endMin = 0;
    uint8_t daysMask = 0x7F;
};
static constexpr uint8_t MAX_HEATER_WINDOWS = 6;
static HeaterWindow s_akuHeaterWindows[MAX_HEATER_WINDOWS];
static uint8_t s_akuHeaterWindowCount = 0;

struct RecircWindow {
    uint8_t startHour = 6;
    uint8_t startMin = 0;
    uint8_t endHour = 7;
    uint8_t endMin = 0;
    uint8_t daysMask = 0x7F;
};
static constexpr uint8_t MAX_RECIRC_WINDOWS = 6;
static RecircWindow s_recircWindows[MAX_RECIRC_WINDOWS];
static uint8_t s_recircWindowCount = 0;

// Heat call (termostat / Nest)
static int8_t s_heatCallInput = -1;  // 0..7 or -1
static bool   s_heatCallActive = false;

// Nastavení relé s respektem k šablonám (např. 3c ventil)
// Pokud je ekviterm aktivní a používá 3c ventil, v AUTO režimu nenecháme relayMap přepisovat jeho relé.
static bool   s_eqEnabled = false;
static int8_t s_eqValveMaster0 = -1; // 0..7, -1 = none
static void relayApplyLogical(uint8_t r0, bool on){
    if (r0 >= RELAY_COUNT) return;

    // Ekviterm (AUTO) si může řídit 3c ventil přímo procenty – zabráníme konfliktům
    if (currentControlMode == ControlMode::AUTO && s_eqEnabled && s_eqValveMaster0 >= 0 && s_eqValveMaster0 < (int8_t)RELAY_COUNT) {
        const uint8_t m0 = (uint8_t)s_eqValveMaster0;
        const uint8_t p0 = s_valves[m0].configured ? s_valves[m0].relayB : 255;
        if (r0 == m0 || r0 == p0) {
            return; // nezasahovat
        }
    }
    // TUV režim: chránit ekvitermní a TUV přepínací ventil před relayMap
    if (s_tuvModeActive) {
        auto isProtectedValveRelay = [&](int8_t master0) -> bool {
            if (master0 < 0 || master0 >= (int8_t)RELAY_COUNT) return false;
            const uint8_t m0 = (uint8_t)master0;
            if (r0 == m0) return true;
            if (isValvePeer(r0) && s_valvePeerOf[r0] == master0) return true;
            return false;
        };
        if (isProtectedValveRelay(s_eqValveMaster0) || isProtectedValveRelay(s_tuvValveMaster0)) {
            return;
        }
    }

    if (isValvePeer(r0)){
        // peer relé je řízené masterem, vynucujeme OFF
        relaySet(static_cast<RelayId>(r0), false);
        return;
    }
    if (isValveMaster(r0)){
        valveCommand(r0, on);
        return;
    }
    relaySet(static_cast<RelayId>(r0), on);
}


// ---------------- Time schedules + TUV ----------------
enum class ScheduleKind : uint8_t { SET_MODE=0, SET_CONTROL_MODE=1, dhw_enable=2, NIGHT_MODE=3 };

struct ScheduleItem {
    bool enabled = true;
    uint8_t daysMask = 0x7F;     // bit0=Mon ... bit6=Sun
    uint8_t hour = 6;
    uint8_t minute = 0;
    ScheduleKind kind = ScheduleKind::SET_MODE;
    SystemMode modeValue = SystemMode::MODE1;
    ControlMode controlValue = ControlMode::MANUAL;
    bool enableValue = false;
    uint32_t lastFiredMinuteKey = 0; // YYYYMMDDHHMM simplified key to avoid double-fire
};

static constexpr uint8_t MAX_SCHEDULES = 16;
static ScheduleItem s_schedules[MAX_SCHEDULES];
static uint8_t s_scheduleCount = 0;
static bool s_nightMode = false;
static int8_t s_nightModeInput = -1; // 0..7 or -1 (Funkce I/O -> Aktivace nočního útlumu)
static int8_t s_boilerNightRelay = -1;
static int8_t s_boilerDhwRelay = -1;

// Equitherm konfigurace + stav
// Pozn.: ekviterm se aplikuje pouze v CONTROL=AUTO, ale hodnoty počítáme pro diagnostiku i v MANUAL.


static EqSourceCfg s_eqOutdoorCfg; // venkovní
static EqSourceCfg s_eqBoilerInCfg; // boiler_in (feedback)
static EqSourceCfg s_eqFlowCfg;    // legacy alias (flow)
static EqSourceCfg s_eqAkuTopCfg;  // AKU top (ekonomika)
static EqSourceCfg s_eqAkuMidCfg;  // AKU mid (diagnostika)
static EqSourceCfg s_eqAkuBottomCfg; // AKU bottom (diagnostika)

// Křivka (stejný vzorec jako UI): Tflow = (20 - Tout) * slope + 20 + shift
// Defaulty jsou zvolené tak, aby dávaly smysl bez okamžité saturace na max.
// (typicky: 55°C při -10°C venku a ~30°C při +15°C venku)
static float  s_eqSlopeDay   = 1.00f;
static float  s_eqShiftDay   = 5.00f;
static float  s_eqSlopeNight = 1.00f;
static float  s_eqShiftNight = 0.00f;

// Legacy refs (fallback, když slope/shift nejsou v configu)
static float  s_eqHeatTout1 = -10, s_eqHeatTflow1 = 55, s_eqHeatTout2 = 15, s_eqHeatTflow2 = 30;
static float  s_eqNightTout1 = -10, s_eqNightTflow1 = 50, s_eqNightTout2 = 15, s_eqNightTflow2 = 25;

// limity teploty otopné vody
static float  s_eqMinFlow = 25, s_eqMaxFlow = 55;

// Řízení 3c ventilu (B varianta = plynulá korekce po krocích)
static float    s_eqDeadbandC = 0.5f;      // necitlivost (°C)
static uint8_t  s_eqStepPct   = 0;         // krok změny pozice (%) (0 = AUTO z pulsu/travel)
static uint32_t s_eqPeriodMs  = 30000;     // minimální perioda korekcí (ms)
static uint8_t  s_eqMinPct    = 0;         // clamp pozice
static uint8_t  s_eqMaxPctDay    = 100;
static uint8_t  s_eqMaxPctNight  = 100;
static float    s_eqAkuMinTopCDay = 40.0f;
static float    s_eqAkuMinTopCNight = 45.0f;
static float    s_eqAkuMinDeltaToTargetCDay = 2.0f;
static float    s_eqAkuMinDeltaToTargetCNight = 3.0f;
static float    s_eqAkuMinDeltaToBoilerInCDay = 3.0f;
static float    s_eqAkuMinDeltaToBoilerInCNight = 4.0f;
static bool     s_eqAkuSupportEnabled = true;
static String   s_eqAkuNoSupportBehavior = "close"; // close | hold
static float    s_eqCurveOffsetC = 0.0f;
static float    s_eqMaxBoilerInC = 55.0f;
static bool     s_eqNoFlowDetectEnabled = true;
static uint32_t s_eqNoFlowTimeoutMs = 180000;
static uint32_t s_eqNoFlowTestPeriodMs = 180000;
static float    s_eqLastFlowC = NAN;
static uint32_t s_eqLastFlowChangeMs = 0;
static bool     s_eqNoFlowActive = false;
static uint32_t s_eqNoFlowLastTestMs = 0;

static float    s_eqFallbackOutdoorC = 0.0f;
static bool     s_eqHomingEnabled = false;
static bool     s_eqHomingOnBoot = true;
static bool     s_eqHomingOnConfigChange = true;
static uint32_t s_eqHomingPeriodMs = 86400000UL; // 24h
static uint8_t  s_eqHomingTargetPct = 0;         // typically 0% (A)
static bool     s_eqHomingPending = false;
static uint32_t s_eqLastHomingMs = 0;
static bool     s_eqBootHomingArmed = true;
static bool     s_eqValveConfigChanged = false;
static uint32_t s_eqOutdoorMaxAgeMs = 900000;
static float    s_eqLastOutdoorC = NAN;
static uint32_t s_eqLastOutdoorMs = 0;

static String   s_systemProfile = "standard";
static String   s_nightModeSource = "heat_call"; // heat_call | input | schedule | manual
static bool     s_nightModeManual = false;

// Equitherm config validation (minimální podmínky pro AUTO řízení)
static bool     s_eqConfigOk = true;
static String   s_eqConfigReason = "";
static String   s_eqConfigWarning = "";

static uint32_t s_eqLastAdjustMs = 0;
static String   s_eqReason = "";

static EquithermStatus s_eqStatus = {};

static bool isValidTimeNow(struct tm &outTm, time_t &outEpoch) {
    outEpoch = time(nullptr);
    if (outEpoch < 1700000000) return false;
    localtime_r(&outEpoch, &outTm);
    return true;
}


static float lerpClamped(float x, float x1, float y1, float x2, float y2){
    if (!isfinite(x1) || !isfinite(x2) || fabsf(x2-x1) < 0.0001f) return NAN;
    float t = (x - x1) / (x2 - x1);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return y1 + (y2 - y1) * t;
}

static bool tryGetDallasTempC(uint8_t gpio, const String& romHex, float &outC){
    outC = NAN;
    const DallasGpioStatus* st = DallasController::getStatus(gpio);
    if (!st) return false;
    if (st->devices.empty()) return false;

    uint64_t wantRom = 0;
    bool haveRom = false;
    if (romHex.length()){
        char *endp = nullptr;
        wantRom = strtoull(romHex.c_str(), &endp, 16);
        haveRom = (endp && endp != romHex.c_str());
    }

    for (const auto &d : st->devices){
        if (!d.valid) continue;
        if (haveRom && d.rom != wantRom) continue;
        outC = d.temperature;
        return isfinite(outC);
    }
    // Pokud je rom zadaný, ale nenalezen, zkus aspoň první validní
    if (haveRom){
        for (const auto &d : st->devices){
            if (!d.valid) continue;
            outC = d.temperature;
            return isfinite(outC);
        }
    }
    return false;
}

struct EqSourceDiag {
    bool valid = false;
    uint32_t ageMs = 0;
    String reason = "";
};

static bool tryGetTempFromSource(const EqSourceCfg& src, float &outC, EqSourceDiag* diag = nullptr){
    outC = NAN;
    if (diag) {
        diag->valid = false;
        diag->ageMs = 0;
        diag->reason = "";
    }

    const String s = src.source;
    if (!s.length() || s == "none") {
        if (diag) diag->reason = "source none";
        return false;
    }

    if (s == "dallas"){
        const uint8_t gpio = (uint8_t)src.gpio;
        if (gpio > 3) return false;
        // pro jistotu průběžně udržuj Dallas loop
        DallasController::loop();
        const bool ok = tryGetDallasTempC(gpio, src.romHex, outC);
        if (diag) {
            diag->valid = ok && isfinite(outC);
            if (!diag->valid) diag->reason = "dallas invalid";
        }
        return ok;
    }

    if (s.startsWith("temp")){
        int idx = s.substring(4).toInt(); // temp1..8
        if (idx < 1 || idx > (int)INPUT_COUNT) return false;
        const uint8_t i0 = (uint8_t)(idx - 1);
        if (!s_tempValid[i0]) return false;
        outC = s_tempC[i0];
        if (diag) diag->valid = isfinite(outC);
        return isfinite(outC);
    }

    if (s == "mqtt"){
        String topic = src.topic;
        String jsonKey = src.jsonKey;

        // Pokud je nastavené mqttIdx (přednastavený MQTT teploměr z "Teploměry"),
        // vždy preferujeme aktuální topic/jsonKey z tohoto nastavení.
        if (src.mqttIdx >= 1 && src.mqttIdx <= 2) {
            const MqttThermometerCfg &mc = thermometersGetMqtt((uint8_t)(src.mqttIdx - 1));
            if (mc.topic.length()) topic = mc.topic;
            if (mc.jsonKey.length()) jsonKey = mc.jsonKey;
        }

        if (!topic.length()) {
            if (diag) diag->reason = "mqtt no topic";
            return false;
        }
        String payload;
        uint32_t lastMs = 0;
        if (!mqttGetLastValueInfo(topic, &payload, &lastMs)) {
            if (diag) diag->reason = "mqtt no data";
            return false;
        }
        const uint32_t nowMs = millis();
        const uint32_t ageMs = (uint32_t)(nowMs - lastMs);
        if (diag) diag->ageMs = ageMs;
        if (src.maxAgeMs > 0 && ageMs > src.maxAgeMs) {
            if (diag) diag->reason = "mqtt stale";
            return false;
        }

        float tC = NAN;
        if (!tempParseFromPayload(payload, jsonKey, tC)) {
            if (diag) diag->reason = "mqtt parse";
            return false;
        }
        outC = tC;
        if (diag) diag->valid = isfinite(outC);
        return isfinite(outC);
    }

    if (s == "ble"){
    // BLE: default "meteo.tempC".
    // bleId může být prázdné (default) nebo např. "meteo", "meteo.tempC".
    // maxAgeMs nyní platí i pro BLE (override proti ble.maxAgeMs).
    const String id = src.bleId;
    uint32_t ageMs = 0;
    const uint32_t maxAge = src.maxAgeMs; // 0 = použij ble.maxAgeMs
    const bool ok = bleGetTempCByIdEx(id, outC, maxAge, diag ? &ageMs : nullptr);
    if (diag) {
        diag->ageMs = ageMs;
        diag->valid = ok && isfinite(outC);
        if (!diag->valid) diag->reason = ok ? "ble non-finite" : "ble invalid/stale";
    }
    return ok;
}


    if (s.startsWith("opentherm")){
        #if defined(FEATURE_OPENTHERM)
        OpenThermStatus ot = openthermGetStatus();
        if (!ot.ready) {
            if (diag) diag->reason = "OT not ready";
            return false;
        }
        if (s == "opentherm_boiler") outC = ot.boilerTempC;
        else if (s == "opentherm_return") outC = ot.returnTempC;
        else if (s == "opentherm_outdoor") outC = NAN;
        else outC = NAN;

        if (!isfinite(outC)) {
            if (diag) diag->reason = "OT no data";
            return false;
        }
        if (diag) diag->valid = true;
        return true;
        #else
        if (diag) diag->reason = "OT disabled";
        return false;
        #endif
    }

    // BLE do budoucna
    return false;
}

static float eqComputeTargetFromSlopeShift(float tout, bool night){
    const float slope = night ? s_eqSlopeNight : s_eqSlopeDay;
    const float shift = night ? s_eqShiftNight : s_eqShiftDay;

    float tflow = (20.0f - tout) * slope + 20.0f + shift;
    if (!isfinite(tflow)) return NAN;

    // limity (min/max)
    float mn = s_eqMinFlow, mx = s_eqMaxFlow;
    if (isfinite(mn) && isfinite(mx)) {
        if (mn > mx) { float tmp=mn; mn=mx; mx=tmp; }
        if (tflow < mn) tflow = mn;
        if (tflow > mx) tflow = mx;
    }
    return tflow;
}

static float eqComputeTargetFromRefs(float tout, bool night){
    const float x1 = night ? s_eqNightTout1 : s_eqHeatTout1;
    const float y1 = night ? s_eqNightTflow1 : s_eqHeatTflow1;
    const float x2 = night ? s_eqNightTout2 : s_eqHeatTout2;
    const float y2 = night ? s_eqNightTflow2 : s_eqHeatTflow2;

    float tflow = lerpClamped(tout, x1, y1, x2, y2);
    if (!isfinite(tflow)) return NAN;

    float mn = s_eqMinFlow, mx = s_eqMaxFlow;
    if (isfinite(mn) && isfinite(mx)) {
        if (mn > mx) { float tmp=mn; mn=mx; mx=tmp; }
        if (tflow < mn) tflow = mn;
        if (tflow > mx) tflow = mx;
    }
    return tflow;
}

static bool isAkuMixingAllowed(float targetFlowC, float boilerInC, bool night, String* outReason = nullptr){
    if (!s_eqAkuSupportEnabled) return true;
    if (!s_eqAkuTopCfg.source.length() || s_eqAkuTopCfg.source == "none") return true;
    float top = NAN;
    EqSourceDiag diag;
    if (!tryGetTempFromSource(s_eqAkuTopCfg, top, &diag) || !isfinite(top)) {
        if (outReason) *outReason = diag.reason.length() ? diag.reason : "aku top invalid";
        return false;
    }
    const float minTop = night ? s_eqAkuMinTopCNight : s_eqAkuMinTopCDay;
    const float minDeltaTarget = night ? s_eqAkuMinDeltaToTargetCNight : s_eqAkuMinDeltaToTargetCDay;
    const float minDeltaBoiler = night ? s_eqAkuMinDeltaToBoilerInCNight : s_eqAkuMinDeltaToBoilerInCDay;
    if (minTop > 0.0f && top < minTop) {
        if (outReason) *outReason = "aku top low";
        return false;
    }
    if (minDeltaTarget > 0.0f && isfinite(targetFlowC) && top < (targetFlowC + minDeltaTarget)) {
        if (outReason) *outReason = "aku delta target low";
        return false;
    }
    if (minDeltaBoiler > 0.0f && isfinite(boilerInC) && top < (boilerInC + minDeltaBoiler)) {
        if (outReason) *outReason = "aku delta boiler low";
        return false;
    }
    return true;
}

static void equithermRecompute(){
    s_eqStatus = EquithermStatus{};
    s_eqStatus.enabled = s_eqEnabled;
    s_eqStatus.night   = s_nightMode;
    s_eqStatus.valveMaster = (s_eqValveMaster0 >= 0) ? (uint8_t)(s_eqValveMaster0 + 1) : 0;
    s_eqStatus.configOk = s_eqConfigOk;
    s_eqStatus.configReason = s_eqConfigReason;
    s_eqStatus.configWarning = s_eqConfigWarning;
    s_eqReason = "";
    s_eqStatus.reason = "";
    s_eqStatus.akuSupportActive = false;

    if (!s_eqEnabled) { s_eqReason = "disabled"; s_eqStatus.reason = s_eqReason; return; }

    // Pokud konfigurace není OK, ekviterm může stále počítat cílovou teplotu pro diagnostiku,
    // ale v AUTO řízení nezasahuje (bez ventilu/feedbacku by to nebylo bezpečné).
    if (!s_eqConfigOk) {
        s_eqReason = s_eqConfigReason.length() ? s_eqConfigReason : "config invalid";
        s_eqStatus.reason = s_eqReason;
    }

    // Outdoor temperature (venek)
    float tout = NAN;
    EqSourceDiag outDiag;
    bool outdoorOk = tryGetTempFromSource(s_eqOutdoorCfg, tout, &outDiag);

    // Auto-map: pokud není venkovní teplota nastavená (source none) a je k dispozici BLE meteo, použij ji.
    if ((!outdoorOk || !isfinite(tout)) && (!s_eqOutdoorCfg.source.length() || s_eqOutdoorCfg.source == "none")) {
        float tBle = NAN;
        if (bleGetMeteoTempC(tBle) && isfinite(tBle)) {
            tout = tBle;
            outdoorOk = true;
            outDiag.valid = true;
            outDiag.ageMs = 0;
            outDiag.reason = "auto BLE meteo";
        }
    }
    if (!outdoorOk || !isfinite(tout)) {
        // fallback: poslední validní hodnota (maxAge), poté fixní fallback
        const uint32_t nowMs = millis();
        const bool hasRecent = (s_eqLastOutdoorMs != 0) && (uint32_t)(nowMs - s_eqLastOutdoorMs) <= s_eqOutdoorMaxAgeMs;
        if (hasRecent && isfinite(s_eqLastOutdoorC)) {
            tout = s_eqLastOutdoorC;
            outDiag.reason = "fallback last";
        } else {
            tout = s_eqFallbackOutdoorC;
            outDiag.reason = "fallback fixed";
        }
    } else {
        s_eqLastOutdoorC = tout;
        s_eqLastOutdoorMs = millis();
    }
    s_eqStatus.outdoorValid = outdoorOk && isfinite(tout);
    s_eqStatus.outdoorAgeMs = outDiag.ageMs;
    s_eqStatus.outdoorReason = outDiag.reason;

    // Target flow temp from curve
    float curveOffset = s_eqCurveOffsetC;
    if (s_systemProfile == "comfort") curveOffset += 1.0f;
    else if (s_systemProfile == "eco") curveOffset -= 1.0f;

    float target = eqComputeTargetFromSlopeShift(tout, s_nightMode);
    if (!isfinite(target)) {
        // fallback for older configs
        target = eqComputeTargetFromRefs(tout, s_nightMode);
    }
    if (!isfinite(target)) {
        s_eqReason = "invalid curve";
        s_eqStatus.reason = s_eqReason;
        return;
    }
    target += curveOffset;

    // Flow temperature (feedback) + BoilerIn (limit/diagnostic)
    float flow = NAN;
    EqSourceDiag flowDiag;
    const bool flowOk = tryGetTempFromSource(s_eqFlowCfg, flow, &flowDiag) && isfinite(flow);

    float boilerIn = NAN;
    EqSourceDiag boilerDiag;
    const bool boilerOk = tryGetTempFromSource(s_eqBoilerInCfg, boilerIn, &boilerDiag) && isfinite(boilerIn);

    const bool hasFlow = flowOk;

    s_eqStatus.active      = true;
    s_eqStatus.outdoorC    = tout;
    s_eqStatus.flowC       = flow;
    s_eqStatus.boilerInC   = boilerIn;
    s_eqStatus.boilerInValid = boilerOk;
    s_eqStatus.targetFlowC = target;
    s_eqStatus.actualC = flow;
    s_eqStatus.targetC = target;
    s_eqStatus.lastAdjustMs = s_eqLastAdjustMs;

    if (!s_eqStatus.outdoorValid) {
        s_eqReason = "outdoor invalid";
        s_eqStatus.reason = s_eqReason;
        s_eqStatus.akuSupportActive = false;
        s_eqStatus.akuSupportReason = s_eqReason;
        return;
    }

    // AKU temperatures (diagnostic)
    float akuTop = NAN;
    float akuMid = NAN;
    float akuBottom = NAN;
    EqSourceDiag akuDiag;
    s_eqStatus.akuTopValid = tryGetTempFromSource(s_eqAkuTopCfg, akuTop, &akuDiag) && isfinite(akuTop);
    s_eqStatus.akuTopC = akuTop;
    s_eqStatus.akuMidValid = tryGetTempFromSource(s_eqAkuMidCfg, akuMid, nullptr) && isfinite(akuMid);
    s_eqStatus.akuMidC = akuMid;
    s_eqStatus.akuBottomValid = tryGetTempFromSource(s_eqAkuBottomCfg, akuBottom, nullptr) && isfinite(akuBottom);
    s_eqStatus.akuBottomC = akuBottom;

    // Valve snapshot for UI
    if (s_eqValveMaster0 >= 0 && s_eqValveMaster0 < (int8_t)RELAY_COUNT && isValveMaster((uint8_t)s_eqValveMaster0)) {
        const Valve3WayState &v = s_valves[(uint8_t)s_eqValveMaster0];
        const uint32_t nowMs = millis();
        s_eqStatus.valvePosPct = valveComputePosPct(v, nowMs);
        s_eqStatus.valveTargetPct = v.targetPct;
        s_eqStatus.valveMoving = v.moving;
    }

    if (s_tuvModeActive) {
        s_eqReason = "tuv mode";
        s_eqStatus.reason = s_eqReason;
        return;
    }

    // If enabled, but not in AUTO, expose reason (řízení se aplikuje jen v AUTO)
    if (currentControlMode != ControlMode::AUTO) {
        s_eqReason = "needs AUTO";
        s_eqStatus.reason = s_eqReason;
        return;
    }

    // V AUTO režimu běží řízení jen pokud máme feedback + nakonfigurovaný ventil
    if (!hasFlow) {
        s_eqReason = "no flow temp";
        s_eqStatus.reason = s_eqReason;
        return;
    }
    if (s_eqValveMaster0 < 0 || s_eqValveMaster0 >= (int8_t)RELAY_COUNT || !isValveMaster((uint8_t)s_eqValveMaster0)) {
        s_eqReason = "no 3-way valve";
        s_eqStatus.reason = s_eqReason;
        return;
    }

    if (s_eqNoFlowActive && s_eqNoFlowDetectEnabled) {
        s_eqReason = "no_flow_or_sensor_stuck";
        s_eqStatus.reason = s_eqReason;
        return;
    }

    const float boilerForLimits = boilerOk ? boilerIn : flow;
    if (isfinite(s_eqMaxBoilerInC) && s_eqMaxBoilerInC > 0.0f && isfinite(boilerForLimits) && boilerForLimits > s_eqMaxBoilerInC) {
        s_eqReason = "boiler in high";
        s_eqStatus.reason = s_eqReason;
        return;
    }

    String akuReason;
    if (!isAkuMixingAllowed(target, boilerForLimits, s_nightMode, &akuReason)) {
        s_eqReason = akuReason.length() ? akuReason : "aku limit";
        s_eqStatus.reason = s_eqReason;
        s_eqStatus.akuSupportActive = false;
        s_eqStatus.akuSupportReason = s_eqReason;
        return;
    }
    s_eqStatus.akuSupportActive = true;
    s_eqStatus.akuSupportReason = "";

    s_eqReason = "";
    s_eqStatus.reason = ""; // OK
}

static uint8_t equithermAutoStepPct(const Valve3WayState& v) {
    if (v.travelMs == 0) return 4;
    float stepPctF = ((float)v.pulseMs * 100.0f) / (float)v.travelMs;
    if (!isfinite(stepPctF) || stepPctF < 1.0f) stepPctF = 1.0f;
    if (stepPctF > 25.0f) stepPctF = 25.0f;
    return (uint8_t)lroundf(stepPctF);
}

static uint8_t equithermEffectiveStepPct(const Valve3WayState& v) {
    if (s_eqStepPct == 0) return equithermAutoStepPct(v);
    uint8_t step = s_eqStepPct;
    if (step < 1) step = 1;
    if (step > 25) step = 25;
    return step;
}


static void equithermRequestHoming(const char* why){
    if (!s_eqHomingEnabled) return;
    if (!s_eqEnabled) return;
    if (s_eqValveMaster0 < 0 || s_eqValveMaster0 >= (int8_t)RELAY_COUNT) return;
    if (!s_valves[(uint8_t)s_eqValveMaster0].configured) return;
    if (s_tuvModeActive) return;
    // avoid stacking multiple requests
    s_eqHomingPending = true;
    // optional: store reason into warning (non-fatal)
    if (why && strlen(why)) {
        s_eqConfigWarning = String("homing: ") + why;
    }
}

static void equithermControlTick(uint32_t nowMs){
    if (!s_eqEnabled) return;
    if (currentControlMode != ControlMode::AUTO) return;
    if (s_tuvModeActive) return;

    
// --- Homing (synchronizace modelu polohy ventilu se skutečností) ---
// Arm on boot: pokud je povoleno, proveď 1x homing po startu (až když je ventil nakonfigurován).
if (s_eqBootHomingArmed && s_eqHomingEnabled && s_eqHomingOnBoot) {
    if (s_eqValveMaster0 >= 0 && s_eqValveMaster0 < (int8_t)RELAY_COUNT) {
        const uint8_t m0 = (uint8_t)s_eqValveMaster0;
        if (s_valves[m0].configured) {
            equithermRequestHoming("boot");
            s_eqBootHomingArmed = false;
        }
    }
}

// Periodický homing (např. 1x denně) – jen pokud systém běží v AUTO a není aktivní TUV.
if (s_eqHomingEnabled && s_eqHomingPeriodMs > 0 && s_eqLastHomingMs != 0) {
    if ((uint32_t)(nowMs - s_eqLastHomingMs) >= s_eqHomingPeriodMs) {
        equithermRequestHoming("periodic");
    }
}

// Pokud je homing pending, proveď ho před regulací.
if (s_eqHomingPending) {
    if (s_eqValveMaster0 >= 0 && s_eqValveMaster0 < (int8_t)RELAY_COUNT) {
        valveMoveToPct((uint8_t)s_eqValveMaster0, (int)s_eqHomingTargetPct);
        s_eqLastHomingMs = nowMs;
        s_eqHomingPending = false;
        // po homingu nastavíme model polohy konzistentně
        s_valves[(uint8_t)s_eqValveMaster0].posPct = s_eqHomingTargetPct;
    }
}

// musí být spočtený target
    if (!s_eqStatus.active || !isfinite(s_eqStatus.targetFlowC)) return;
    if (!s_eqStatus.outdoorValid) {
        if (s_eqValveMaster0 >= 0 && s_eqValveMaster0 < (int8_t)RELAY_COUNT) {
            valveMoveToPct((uint8_t)s_eqValveMaster0, 0);
            s_eqLastAdjustMs = nowMs;
        }
        return;
    }

    // nutný feedback senzor (FLOW)
    float flow = NAN;
    if (!tryGetTempFromSource(s_eqFlowCfg, flow)) {
        // Bez feedbacku je bezpečnější ventil zavřít (0%), než ho nechat v poslední poloze.
        if (s_eqValveMaster0 >= 0 && s_eqValveMaster0 < (int8_t)RELAY_COUNT) {
            const uint8_t m0 = (uint8_t)s_eqValveMaster0;
            if (isValveMaster(m0)) {
                // respektuj periodu korekcí (stejná pojistka proti cvakání)
                if (s_eqLastAdjustMs == 0 || (uint32_t)(nowMs - s_eqLastAdjustMs) >= s_eqPeriodMs) {
                    Valve3WayState &v = s_valves[m0];
                    if (!v.moving) {
                        valveMoveToPct(m0, 0);
                        s_eqLastAdjustMs = nowMs;
                    }
                }
            }
        }
        return;
    }
    if (isfinite(flow)) {
        if (!isfinite(s_eqLastFlowC) || fabsf(flow - s_eqLastFlowC) >= 0.1f) {
            s_eqLastFlowC = flow;
            s_eqLastFlowChangeMs = nowMs;
            s_eqNoFlowActive = false;
        } else if (s_eqNoFlowDetectEnabled && s_eqLastFlowChangeMs != 0 &&
                   (uint32_t)(nowMs - s_eqLastFlowChangeMs) > s_eqNoFlowTimeoutMs) {
            s_eqNoFlowActive = true;
        }
    }
    bool allowTest = false;
    if (s_eqNoFlowActive && s_eqNoFlowDetectEnabled) {
        if (s_eqNoFlowLastTestMs == 0 || (uint32_t)(nowMs - s_eqNoFlowLastTestMs) >= s_eqNoFlowTestPeriodMs) {
            allowTest = true;
            s_eqNoFlowLastTestMs = nowMs;
        } else {
            return;
        }
    }

    // nutný ventil
    if (s_eqValveMaster0 < 0 || s_eqValveMaster0 >= (int8_t)RELAY_COUNT) return;
    const uint8_t master0 = (uint8_t)s_eqValveMaster0;
    if (!isValveMaster(master0)) return;

    // perioda korekcí
    if (s_eqLastAdjustMs != 0 && (uint32_t)(nowMs - s_eqLastAdjustMs) < s_eqPeriodMs) return;

    Valve3WayState &v = s_valves[master0];
    if (v.moving) return;

    // limit / ochrana (BOILER_IN). Když není k dispozici, použij flow.
    float boilerIn = NAN;
    const bool boilerOk = tryGetTempFromSource(s_eqBoilerInCfg, boilerIn) && isfinite(boilerIn);
    const float boilerForLimits = (boilerOk && isfinite(boilerIn)) ? boilerIn : flow;

    const float target = s_eqStatus.targetFlowC;
    String akuReason;
    if (!isAkuMixingAllowed(target, boilerForLimits, s_nightMode, &akuReason)) {
        if (s_eqAkuNoSupportBehavior == "close") {
            valveMoveToPct(master0, 0);
            s_eqLastAdjustMs = nowMs;
        }
        return;
    }
    if (isfinite(s_eqMaxBoilerInC) && s_eqMaxBoilerInC > 0.0f && isfinite(boilerForLimits) && boilerForLimits > s_eqMaxBoilerInC) {
        valveMoveToPct(master0, 0);
        s_eqLastAdjustMs = nowMs;
        return;
    }
    const float err = target - flow;
    if (!isfinite(err)) return;

    if (fabsf(err) <= s_eqDeadbandC) return;

    const uint8_t curPct = valveComputePosPct(v, nowMs);
    const uint8_t stepPct = equithermEffectiveStepPct(v);
    int nextPct = (int)curPct + ((err > 0) ? (int)stepPct : -(int)stepPct);
    if (nextPct < (int)s_eqMinPct) nextPct = (int)s_eqMinPct;
    const uint8_t maxPct = s_nightMode ? s_eqMaxPctNight : s_eqMaxPctDay;
    if (nextPct > (int)maxPct) nextPct = (int)maxPct;

    if (nextPct == (int)curPct) return;

    if (!allowTest || nextPct != (int)curPct) {
        valveMoveToPct(master0, (uint8_t)nextPct);
    }
    s_eqLastAdjustMs = nowMs;

    // aktualizuj status pro UI
    s_eqStatus.flowC = flow;
    s_eqStatus.boilerInC = boilerIn;
    s_eqStatus.boilerInValid = boilerOk;
    s_eqStatus.valvePosPct = curPct;
    s_eqStatus.valveTargetPct = (uint8_t)nextPct;
    s_eqStatus.valveMoving = true;
    s_eqStatus.lastAdjustMs = s_eqLastAdjustMs;
}

static uint8_t dowToMask(const struct tm &t) {
    // tm_wday: 0=Sun..6=Sat -> convert to Mon=bit0..Sun=bit6
    int w = t.tm_wday; 
    int monBased = (w == 0) ? 6 : (w - 1); // Sun->6, Mon->0 ...
    return (uint8_t)(1U << monBased);
}

static uint32_t minuteKey(const struct tm &t) {
    // Build key YYYYMMDDHHMM (fits in uint32 for 2025 etc)
    uint32_t y = (uint32_t)(t.tm_year + 1900);
    uint32_t m = (uint32_t)(t.tm_mon + 1);
    uint32_t d = (uint32_t)t.tm_mday;
    uint32_t hh = (uint32_t)t.tm_hour;
    uint32_t mm = (uint32_t)t.tm_min;
    return (y*100000000UL) + (m*1000000UL) + (d*10000UL) + (hh*100UL) + mm;
}

static bool timeInRecircWindow(const struct tm &t, const RecircWindow &w) {
    const uint8_t dowMask = dowToMask(t);
    if ((w.daysMask & dowMask) == 0) return false;
    const uint16_t cur = (uint16_t)(t.tm_hour * 60 + t.tm_min);
    const uint16_t start = (uint16_t)(w.startHour * 60 + w.startMin);
    const uint16_t end = (uint16_t)(w.endHour * 60 + w.endMin);
    if (start <= end) {
        return (cur >= start && cur <= end);
    }
    // okno přes půlnoc
    return (cur >= start || cur <= end);
}

static uint32_t recircWindowRemainingMs(const struct tm &t, const RecircWindow &w) {
    const uint16_t cur = (uint16_t)(t.tm_hour * 60 + t.tm_min);
    const uint16_t start = (uint16_t)(w.startHour * 60 + w.startMin);
    const uint16_t end = (uint16_t)(w.endHour * 60 + w.endMin);
    uint16_t remainingMin = 0;
    if (start <= end) {
        if (cur > end) return 0;
        remainingMin = (uint16_t)(end - cur);
    } else {
        // okno přes půlnoc
        if (cur <= end) remainingMin = (uint16_t)(end - cur);
        else remainingMin = (uint16_t)((1440 - cur) + end);
    }
    return (uint32_t)remainingMin * 60000UL;
}

static bool timeInHeaterWindow(const struct tm &t, const HeaterWindow &w) {
    const uint8_t dowMask = dowToMask(t);
    if ((w.daysMask & dowMask) == 0) return false;
    const uint16_t cur = (uint16_t)(t.tm_hour * 60 + t.tm_min);
    const uint16_t start = (uint16_t)(w.startHour * 60 + w.startMin);
    const uint16_t end = (uint16_t)(w.endHour * 60 + w.endMin);
    if (start == end) return true;
    if (start < end) return (cur >= start && cur < end);
    return (cur >= start || cur < end);
}

static void recircApplyRelay(bool on) {
    if (s_recircPumpRelay < 0 || s_recircPumpRelay >= (int8_t)RELAY_COUNT) return;
    relaySet(static_cast<RelayId>(s_recircPumpRelay), on);
}

static void recircStart(uint32_t nowMs, uint32_t runMs) {
    s_recircActive = true;
    s_recircStopReached = false;
    s_recircUntilMs = nowMs + runMs;
    s_recircLastOnMs = nowMs;
    recircApplyRelay(true);
}

static void recircStop(uint32_t nowMs) {
    s_recircActive = false;
    s_recircUntilMs = 0;
    s_recircLastOffMs = nowMs;
    recircApplyRelay(false);
}
static void updateInputBasedModes() {
    if (s_tuvEnableInput >= 0 && s_tuvEnableInput < (int8_t)INPUT_COUNT) {
        s_tuvScheduleEnabled = inputGetState(static_cast<InputId>(s_tuvEnableInput));
    }
    if (s_heatCallInput >= 0 && s_heatCallInput < (int8_t)INPUT_COUNT) {
        s_heatCallActive = inputGetState(static_cast<InputId>(s_heatCallInput));
    } else {
        s_heatCallActive = true;
    }
    if (s_tuvDemandInput >= 0 && s_tuvDemandInput < (int8_t)INPUT_COUNT) {
        s_tuvDemandActive = inputGetState(static_cast<InputId>(s_tuvDemandInput));
    } else {
        s_tuvDemandActive = false;
    }
    if (s_nightModeSource == "input" && s_nightModeInput >= 0 && s_nightModeInput < (int8_t)INPUT_COUNT) {
        s_nightMode = inputGetState(static_cast<InputId>(s_nightModeInput));
        equithermRecompute();
    } else if (s_nightModeSource == "manual") {
        s_nightMode = s_nightModeManual;
    } else if (s_nightModeSource == "heat_call") {
        s_nightMode = !s_heatCallActive;
    }
}

static bool isTuvEnabledEffective() {
    // 'enabled' = DHW feature armed (schedule/input/config), 'demand' = external DHW call.
    // DHW mode becomes active only when BOTH are true (per role 'dhw_demand').
    // NOTE: if no demand input is configured, we fall back to schedule-enabled behavior.
    const bool hasDemand = (s_tuvDemandInput >= 0 && s_tuvDemandInput < (int8_t)INPUT_COUNT);
    if (!hasDemand) return s_tuvScheduleEnabled;
    return s_tuvScheduleEnabled && s_tuvDemandActive;
}

static void applyTuvRequest() {
    #if defined(FEATURE_OPENTHERM)
    const OpenThermConfig otCfg = openthermGetConfig();
    if (otCfg.enabled && otCfg.mapDhw && otCfg.boilerControl == OpenThermBoilerControl::OPENTHERM) {
        // In full OpenTherm mode, DHW request is expressed via OT flags/setpoints.
        return;
    }
    #endif

    const int8_t relayIdx = (s_boilerDhwRelay >= 0) ? s_boilerDhwRelay : s_tuvRequestRelay;
    if (relayIdx < 0 || relayIdx >= (int8_t)RELAY_COUNT) return;
    relaySet(static_cast<RelayId>(relayIdx), s_tuvModeActive);
}

static void applyNightModeRelay() {
    #if defined(FEATURE_OPENTHERM)
    const OpenThermConfig otCfg = openthermGetConfig();
    if (otCfg.enabled && otCfg.mapNightMode && otCfg.boilerControl == OpenThermBoilerControl::OPENTHERM) {
        // In full OpenTherm mode, day/night mapping is handled by Ekviterm setpoints.
        return;
    }
    #endif

    if (s_boilerNightRelay < 0 || s_boilerNightRelay >= (int8_t)RELAY_COUNT) return;
    relaySet(static_cast<RelayId>(s_boilerNightRelay), s_nightMode);
}

static void applyOpenThermMapping(uint32_t nowMs) {
    (void)nowMs;
    #if !defined(FEATURE_OPENTHERM)
    return;
    #else
    const OpenThermConfig cfg = openthermGetConfig();
    if (!cfg.enabled) return;
    if (cfg.boilerControl == OpenThermBoilerControl::RELAY) return; // read-only / not integrated

    static float lastCh = NAN;
    static float lastDhw = NAN;
    static bool lastChEn = false;
    static bool lastDhwEn = false;
    static uint32_t lastSendMs = 0;

    // Don't spam commands when nothing changes.
    const uint32_t minPeriodMs = 1500;
    if (lastSendMs != 0 && (uint32_t)(nowMs - lastSendMs) < minPeriodMs) {
        // allow faster when value changed a lot (handled below)
    }

    // DHW has priority
    if (s_tuvModeActive && cfg.mapDhw) {
        const bool chEn = true;
        const bool dhwEn = true;
        const float dhwSp = cfg.dhwSetpointC;

        // Optional boost: if we have Ekviterm target, add boost to CH setpoint during DHW.
        float chSp = NAN;
        if (isfinite(s_eqStatus.targetFlowC)) chSp = s_eqStatus.targetFlowC + cfg.dhwBoostChSetpointC;

        const bool chChanged = (!isfinite(lastCh) && isfinite(chSp)) || (isfinite(chSp) && (!isfinite(lastCh) || fabsf(chSp - lastCh) >= 0.5f));
        const bool dhwChanged = (!isfinite(lastDhw) && isfinite(dhwSp)) || (isfinite(dhwSp) && (!isfinite(lastDhw) || fabsf(dhwSp - lastDhw) >= 0.5f));
        const bool flagsChanged = (chEn != lastChEn) || (dhwEn != lastDhwEn);

        if (flagsChanged || dhwChanged || chChanged || lastSendMs == 0 || (uint32_t)(nowMs - lastSendMs) >= 5000) {
            if (cfg.boilerControl == OpenThermBoilerControl::OPENTHERM) {
                openthermCmdSetEnable(chEn, dhwEn);
            }
            openthermCmdSetDhwSetpoint(dhwSp);
            if (isfinite(chSp)) openthermCmdSetChSetpoint(chSp);

            lastChEn = chEn;
            lastDhwEn = dhwEn;
            lastDhw = dhwSp;
            lastCh = chSp;
            lastSendMs = nowMs;
        }
        return;
    }

    // CH / Ekviterm mapping
    if (cfg.mapEquithermChSetpoint && s_eqStatus.active && isfinite(s_eqStatus.targetFlowC)) {
        const bool chEn = true;
        const bool dhwEn = false;
        const float chSp = s_eqStatus.targetFlowC;
        const bool chChanged = (!isfinite(lastCh) && isfinite(chSp)) || (isfinite(chSp) && (!isfinite(lastCh) || fabsf(chSp - lastCh) >= 0.5f));
        const bool flagsChanged = (chEn != lastChEn) || (dhwEn != lastDhwEn);

        if (flagsChanged || chChanged || lastSendMs == 0 || (uint32_t)(nowMs - lastSendMs) >= 5000) {
            if (cfg.boilerControl == OpenThermBoilerControl::OPENTHERM) {
                openthermCmdSetEnable(chEn, dhwEn);
            }
            openthermCmdSetChSetpoint(chSp);
            lastChEn = chEn;
            lastDhwEn = dhwEn;
            lastCh = chSp;
            lastSendMs = nowMs;
        }
    }
    #endif
}

static uint8_t clampPctInt(int v) {
    if (v < 0) return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}

static uint8_t effectiveTuvValvePct(bool active) {
    uint8_t pct = active ? s_tuvBypassPct : s_tuvChPct;
    if (!s_tuvBypassEnabled) pct = s_tuvValveTargetPct;
    if (s_tuvBypassInvert) pct = (uint8_t)(100 - pct);
    return clampPctInt(pct);
}

static void applyTuvValvePct(bool active) {
    if (s_tuvValveMaster0 < 0 || s_tuvValveMaster0 >= (int8_t)RELAY_COUNT) return;

    const uint8_t pct = effectiveTuvValvePct(active);
    s_tuvValveCurrentPct = pct;
    s_tuvValveMode = active ? "dhw" : "ch";

    const uint8_t m0 = (uint8_t)s_tuvValveMaster0;
    // If the valve is configured as a 3-way master but is actually single-relay (switching valve),
    // do NOT run the motorized timing state machine. We must directly drive the relay state,
    // otherwise the configured invertDir/defaultPos can keep the output latched ON.
    if (isValveMaster(m0) && !s_valves[m0].singleRelay) {
        valveMoveToPct(m0, pct);
        return;
    }

    bool on = (pct >= 50);
    // keep support for per-valve invertDir if user set it (single-relay valve config)
    if (isValveMaster(m0) && s_valves[m0].singleRelay) {
        on = (on ^ s_valves[m0].invertDir);
    }
    relaySet((RelayId)m0, on);
}

static void applyTuvModeValves(uint32_t nowMs) {
    if (!s_tuvModeActive) return;
    if (s_tuvLastValveCmdMs != 0 && (uint32_t)(nowMs - s_tuvLastValveCmdMs) < 500) return;

    bool issued = false;
    if (s_eqValveMaster0 >= 0 && s_eqValveMaster0 < (int8_t)RELAY_COUNT && isValveMaster((uint8_t)s_eqValveMaster0)) {
        valveMoveToPct((uint8_t)s_eqValveMaster0, s_tuvEqValveTargetPct);
        issued = true;
    }
    if (s_tuvValveMaster0 >= 0 && s_tuvValveMaster0 < (int8_t)RELAY_COUNT) {
        applyTuvValvePct(true);
        issued = true;
    }
    if (issued) s_tuvLastValveCmdMs = nowMs;
}

static void updateTuvModeState(uint32_t nowMs) {
    const bool newActive = isTuvEnabledEffective();
    // Status explanation for UI
    if (s_tuvScheduleEnabled && s_tuvDemandActive) {
        s_tuvReason = "active";
        s_tuvSource = "demand";
    } else if (s_tuvScheduleEnabled) {
        // DHW feature armed, but no demand yet
        s_tuvReason = "armed";
        s_tuvSource = "schedule";
    } else if (s_tuvDemandActive) {
        // Demand present, but DHW feature disabled
        s_tuvReason = "demand_ignored";
        s_tuvSource = "demand";
    } else {
        s_tuvReason = "off";
        s_tuvSource = "none";
    }

    // hrany: uložit/vrátit polohu směšovacího ventilu (ekviterm)
    if (newActive && !s_tuvPrevModeActive) {
        // právě začal ohřev TUV
        if (s_eqValveMaster0 >= 0 && isValveMaster((uint8_t)s_eqValveMaster0)) {
            s_tuvEqValveSavedPct = clampPctInt((int)valveComputePosPct(s_valves[(uint8_t)s_eqValveMaster0], nowMs));
            s_tuvEqValveSavedValid = true;
        } else {
            s_tuvEqValveSavedValid = false;
        }
    }

    if (!newActive && s_tuvPrevModeActive) {
        // právě skončil ohřev TUV -> vrať směšovací ventil do předchozí polohy
        if (s_tuvRestoreEqValveAfter && s_tuvEqValveSavedValid && s_eqValveMaster0 >= 0 && isValveMaster((uint8_t)s_eqValveMaster0)) {
            valveMoveToPct((uint8_t)s_eqValveMaster0, s_tuvEqValveSavedPct);
            // zabráníme tomu, aby ekviterm hned v tom samém okamžiku přepočetl a přepsal návrat
            s_eqLastAdjustMs = nowMs;
        }
        s_tuvEqValveSavedValid = false;
        if (s_tuvValveMaster0 >= 0 && s_tuvValveMaster0 < (int8_t)RELAY_COUNT) {
            // Force valve to CH position immediately when DHW ends.
            applyTuvValvePct(false);
        }
    }

    s_tuvModeActive = newActive;
    s_tuvPrevModeActive = newActive;
    applyTuvModeValves(nowMs);

    // When DHW is inactive, ensure the fixed switching valve (R3) is not left latched ON.
    // This makes R3 effectively follow the same active state as the DHW request (R5).
    if (!s_tuvModeActive) {
        applyTuvValvePct(false);
    }
}

static void recircUpdate(uint32_t nowMs) {
    if (!s_recircEnabled) {
        if (s_recircActive) recircStop(nowMs);
        return;
    }

    bool timeValid = false;
    struct tm t;
    time_t epoch;
    if (isValidTimeNow(t, epoch)) {
        timeValid = true;
    }

    bool windowActive = false;
    uint32_t windowRemainingMs = 0;
    if ((s_recircMode == "time_windows" || s_recircMode == "hybrid") && timeValid) {
        for (uint8_t i = 0; i < s_recircWindowCount; i++) {
            if (timeInRecircWindow(t, s_recircWindows[i])) {
                windowActive = true;
                windowRemainingMs = recircWindowRemainingMs(t, s_recircWindows[i]);
                break;
            }
        }
    }

    const bool cycleEnabled = (s_recircCycleOnMs > 0 && s_recircCycleOffMs > 0 &&
                               (s_recircMode == "time_windows" || s_recircMode == "hybrid"));

    bool demandEdge = false;
    if (s_recircDemandInput >= 0 && s_recircDemandInput < (int8_t)INPUT_COUNT) {
        const bool cur = inputGetState(static_cast<InputId>(s_recircDemandInput));
        demandEdge = cur && !s_recircPrevDemand;
        s_recircPrevDemand = cur;
    }

    if ((s_recircMode == "on_demand" || s_recircMode == "hybrid") && demandEdge) {
        if (s_recircLastOffMs == 0 || (uint32_t)(nowMs - s_recircLastOffMs) >= s_recircMinOffMs) {
            // Explicitní požadavek (edge) má prioritu: dovol restart i když byl před chvílí dosažen
            // stopTemp (jinak by uživatel nemohl znovu vyžádat cirkulaci během okna).
            s_recircStopReached = false;
            recircStart(nowMs, s_recircOnDemandRunMs);
        }
    }

    if (!s_recircActive && windowActive && windowRemainingMs > 0 && !s_recircStopReached) {
        // V oknech lze volitelně cyklovat ON/OFF (např. 5/15). OFF pauza se přičte k minOff.
        const uint32_t offGuardMs = cycleEnabled ? max(s_recircMinOffMs, s_recircCycleOffMs) : s_recircMinOffMs;
        if (s_recircLastOffMs == 0 || (uint32_t)(nowMs - s_recircLastOffMs) >= offGuardMs) {
            const uint32_t runMs = cycleEnabled ? min(windowRemainingMs, s_recircCycleOnMs) : windowRemainingMs;
            recircStart(nowMs, runMs);
        }
    }

    // aktualizace návratové teploty
    s_recircReturnValid = tryGetTempFromSource(s_recircReturnCfg, s_recircReturnC, nullptr) && isfinite(s_recircReturnC);

    if (s_recircActive) {
        const bool minOnOk = (s_recircLastOnMs == 0) || (uint32_t)(nowMs - s_recircLastOnMs) >= s_recircMinOnMs;
        if (s_recircReturnValid && s_recircStopTempC > 0.0f && s_recircReturnC >= s_recircStopTempC && minOnOk) {
            s_recircStopReached = true;
            recircStop(nowMs);
            return;
        }

        // pokud běžíme v cyklu, respektujeme untilMs i když je okno stále aktivní
        const bool allowStopOnUntil = cycleEnabled || !windowActive;
        if (s_recircUntilMs != 0 && (int32_t)(nowMs - s_recircUntilMs) >= 0 && minOnOk && allowStopOnUntil) {
            recircStop(nowMs);
            return;
        }
        if (s_recircUntilMs == 0 && !windowActive && minOnOk) {
            recircStop(nowMs);
            return;
        }
    } else if (!windowActive) {
        s_recircStopReached = false;
    }
}

static void akuHeaterApplyRelay(bool on) {
    if (s_akuHeaterRelay < 0 || s_akuHeaterRelay >= (int8_t)RELAY_COUNT) return;
    relaySet(static_cast<RelayId>(s_akuHeaterRelay), on);
}

static void akuHeaterStart(uint32_t nowMs, const String &reason) {
    s_akuHeaterActive = true;
    s_akuHeaterLastOnMs = nowMs;
    s_akuHeaterReason = reason;
    akuHeaterApplyRelay(true);
}

static void akuHeaterStop(uint32_t nowMs, const String &reason) {
    s_akuHeaterActive = false;
    s_akuHeaterLastOffMs = nowMs;
    s_akuHeaterReason = reason;
    akuHeaterApplyRelay(false);
}

static void akuHeaterUpdate(uint32_t nowMs) {
    if (!s_akuHeaterEnabled || s_akuHeaterRelay < 0 || s_akuHeaterRelay >= (int8_t)RELAY_COUNT) {
        if (s_akuHeaterActive) akuHeaterStop(nowMs, "disabled");
        return;
    }

    bool wantOn = false;
    String reason = "";

    float topC = NAN;
    EqSourceDiag diag;
    const bool topValid = tryGetTempFromSource(s_eqAkuTopCfg, topC, &diag) && isfinite(topC);

    if (s_akuHeaterMode == "manual") {
        wantOn = s_akuHeaterManualOn;
        reason = wantOn ? "manual" : "manual off";
    } else if (s_akuHeaterMode == "schedule") {
        struct tm t;
        time_t epoch;
        bool windowActive = false;
        if (isValidTimeNow(t, epoch)) {
            for (uint8_t i = 0; i < s_akuHeaterWindowCount; i++) {
                if (timeInHeaterWindow(t, s_akuHeaterWindows[i])) {
                    windowActive = true;
                    break;
                }
            }
        }
        wantOn = windowActive;
        reason = "schedule";
    } else if (s_akuHeaterMode == "thermostatic") {
        if (!topValid) {
            wantOn = false;
            reason = diag.reason.length() ? diag.reason : "sensor invalid";
        } else {
            const float onThr = s_akuHeaterTargetTopC - s_akuHeaterHysteresisC;
            const float offThr = s_akuHeaterTargetTopC + s_akuHeaterHysteresisC;
            if (s_akuHeaterActive) {
                wantOn = (topC < offThr);
            } else {
                wantOn = (topC <= onThr);
            }
            reason = "thermostatic";
        }
    } else {
        wantOn = false;
        reason = "mode invalid";
    }

    if (s_akuHeaterActive) {
        if (s_akuHeaterMaxOnMs > 0 && s_akuHeaterLastOnMs != 0 &&
            (uint32_t)(nowMs - s_akuHeaterLastOnMs) >= s_akuHeaterMaxOnMs) {
            wantOn = false;
            reason = "max_on";
        }
    } else if (wantOn) {
        if (s_akuHeaterMinOffMs > 0 && s_akuHeaterLastOffMs != 0 &&
            (uint32_t)(nowMs - s_akuHeaterLastOffMs) < s_akuHeaterMinOffMs) {
            wantOn = false;
            reason = "min_off";
        }
    }

    if (wantOn) {
        if (!s_akuHeaterActive) akuHeaterStart(nowMs, reason);
        else s_akuHeaterReason = reason;
    } else {
        if (s_akuHeaterActive) akuHeaterStop(nowMs, reason);
        else s_akuHeaterReason = reason;
    }
}

static const RelayProfile* profileForMode(SystemMode mode) {
    switch (mode) {
        case SystemMode::MODE1:   return &profileMODE1;
        case SystemMode::MODE2: return &profileTopeniTopit;
        case SystemMode::MODE3: return &profileTopeniUtlum;
        case SystemMode::MODE4: return &profileMode4;
        case SystemMode::MODE5: return &profileMode5;
        default:                return &profileMODE1;
    }
}

static void updateRelaysForMode(SystemMode mode) {
    const RelayProfile* p = profileForMode(mode);
    for (uint8_t r = 0; r < RELAY_COUNT; r++) {
        // R1..R3 + R5..R8 jsou systémové (ventily/boiler/recirc/AKU) a řídí je
        // specializovaná logika. Režimy (MODE1..MODE5) a relay profily se na ně
        // nesmí aplikovat, aby nedocházelo ke konfliktům.
        if (isFixedFunctionRelay(r) || isValveMaster(r) || isValvePeer(r)) continue;
        relayApplyLogical(r, p->states[r]);
    }
}

static bool getTriggeredAutoMode(SystemMode& outMode) {
    for (uint8_t i = 0; i < 5; i++) {
        const uint8_t in = modeTriggerInput[i];
        if (in == 0 || in > INPUT_COUNT) continue;

        const bool inputActive = inputGetState(static_cast<InputId>(in - 1));
        if (inputActive) {
            outMode = static_cast<SystemMode>(i);
            return true;
        }
    }
    return false;
}

const char* logicModeToString(SystemMode mode) {
    switch (mode) {
        case SystemMode::MODE1:   return "MODE1";
        case SystemMode::MODE2: return "MODE2";
        case SystemMode::MODE3: return "MODE3";
        case SystemMode::MODE4: return "MODE4";
        case SystemMode::MODE5: return "MODE5";
        default:                return "UNKNOWN";
    }
}

SystemMode logicGetMode() {
    return currentMode;
}

ControlMode logicGetControlMode() {
    return currentControlMode;
}

AutoStatus logicGetAutoStatus() {
    return s_autoStatus;
}

bool logicGetTuvEnabled() {
    return s_tuvScheduleEnabled;
}

bool logicGetNightMode() {
    return s_nightMode;
}

bool logicGetHeatCallActive() {
    return s_heatCallActive;
}

TuvStatus logicGetTuvStatus() {
    TuvStatus st;
    st.enabled = s_tuvScheduleEnabled;
    st.active = s_tuvModeActive;
    st.scheduleEnabled = s_tuvScheduleEnabled;
    st.demandActive = s_tuvDemandActive;
    st.modeActive = s_tuvModeActive;
    st.reason = s_tuvReason;
    st.source = s_tuvSource;
    st.boilerRelayOn = false;
    {
        const int8_t relayIdx = (s_boilerDhwRelay >= 0) ? s_boilerDhwRelay : s_tuvRequestRelay;
        if (relayIdx >= 0 && relayIdx < (int8_t)RELAY_COUNT) {
            st.boilerRelayOn = relayGetState(static_cast<RelayId>(relayIdx));
        }
    }
    st.eqValveMaster = (s_eqValveMaster0 >= 0) ? (uint8_t)(s_eqValveMaster0 + 1) : 0;
    st.eqValveTargetPct = s_tuvEqValveTargetPct;
    st.eqValveSavedPct = s_tuvEqValveSavedPct;
    st.eqValveSavedValid = s_tuvEqValveSavedValid;
    st.valveMaster = (s_tuvValveMaster0 >= 0) ? (uint8_t)(s_tuvValveMaster0 + 1) : 0;
    st.valveTargetPct = s_tuvValveCurrentPct;
    st.valvePosPct = 0;
    st.valveMoving = false;
    if (s_tuvValveMaster0 >= 0 && s_tuvValveMaster0 < (int8_t)RELAY_COUNT && isValveMaster((uint8_t)s_tuvValveMaster0)) {
        const uint32_t nowMs = millis();
        st.valvePosPct = valveComputePosPct(s_valves[(uint8_t)s_tuvValveMaster0], nowMs);
        st.valveMoving = s_valves[(uint8_t)s_tuvValveMaster0].moving;
    }
    st.bypassPct = s_tuvBypassPct;
    st.chPct = s_tuvChPct;
    st.bypassInvert = s_tuvBypassInvert;
    st.valveMode = s_tuvValveMode;
    return st;
}

RecircStatus logicGetRecircStatus() {
    RecircStatus st;
    st.enabled = s_recircEnabled;
    st.active = s_recircActive;
    st.mode = s_recircMode;
    st.untilMs = s_recircUntilMs;
    if (s_recircUntilMs > 0) {
        const uint32_t nowMs = millis();
        st.remainingMs = (s_recircUntilMs > nowMs) ? (s_recircUntilMs - nowMs) : 0;
    } else {
        st.remainingMs = 0;
    }
    st.stopReached = s_recircStopReached;
    st.returnTempC = s_recircReturnC;
    st.returnTempValid = s_recircReturnValid;
    return st;
}

AkuHeaterStatus logicGetAkuHeaterStatus() {
    AkuHeaterStatus st;
    st.enabled = s_akuHeaterEnabled;
    st.active = s_akuHeaterActive;
    st.mode = s_akuHeaterMode;
    st.reason = s_akuHeaterReason;
    float top = NAN;
    EqSourceDiag diag;
    st.topValid = tryGetTempFromSource(s_eqAkuTopCfg, top, &diag) && isfinite(top);
    st.topC = top;
    return st;
}


bool logicGetAutoDefaultOffUnmapped() {
    return s_autoDefaultOffUnmapped;
}


// Aplikace relayMap na základě aktuálních logických stavů vstupů
static void applyRelayMapFromInputs(bool defaultOffUnmapped) {
    for (uint8_t r = 0; r < RELAY_COUNT; r++) {
        // FIXED mapping: systémové relé nejsou řízené relayMap (AUTO) – pouze
        // jejich dedikovanou logikou (ventily, boiler signály, recirkulace, AKU).
        if (isFixedFunctionRelay(r) || isValveMaster(r) || isValvePeer(r)) continue;
        const uint8_t in = relayMap[r].input;
        if (in == 0 || in > INPUT_COUNT) {
            // bez přiřazení
            if (defaultOffUnmapped) {
                relayApplyLogical(r, false);
            }
            continue;
        }

        const bool inputActive = inputGetState(static_cast<InputId>(in - 1));
        const bool wantOn = (relayMap[r].polarity ? inputActive : !inputActive);
        relayApplyLogical(r, wantOn);
    }
}

void logicRecomputeFromInputs() {
    // V AUTO režimu: pokud je aktivní spouštěcí vstup některého režimu,
    // použijeme profil režimu. Jinak použijeme relayMap (konfigurace z UI).
    if (currentControlMode != ControlMode::AUTO) {
        return;
    }

    SystemMode triggered;
    if (getTriggeredAutoMode(triggered)) {
        if (currentMode != triggered) {
            currentMode = triggered;
            Serial.print(F("[LOGIC] AUTO: mode triggered -> "));
            Serial.println(logicModeToString(currentMode));
        }
        updateRelaysForMode(currentMode);
        relayPrintStates(Serial);
        return;
    }

    s_autoStatus.triggered = false;
    s_autoStatus.triggerInput = 0;
    s_autoStatus.triggerMode = currentMode;
    s_autoStatus.usingRelayMap = true;
    applyRelayMapFromInputs(s_autoDefaultOffUnmapped);
    Serial.println(F("[LOGIC] AUTO: relayMap applied from inputs (no mode trigger)"));
    relayPrintStates(Serial);
}

void logicSetControlMode(ControlMode mode) {
    if (mode == currentControlMode) return;

    currentControlMode = mode;

    if (currentControlMode == ControlMode::AUTO) {
        Serial.println(F("[LOGIC] Control mode -> AUTO"));
        #if defined(FEATURE_BUZZER)
        buzzerOnControlModeChanged(true);
        #endif
        logicRecomputeFromInputs();
    } else {
        Serial.println(F("[LOGIC] Control mode -> MANUAL"));
        #if defined(FEATURE_BUZZER)
        buzzerOnControlModeChanged(false);
        #endif
        currentMode = manualMode;
        updateRelaysForMode(currentMode);
        Serial.print(F("[LOGIC] MANUAL mode = "));
        Serial.println(logicModeToString(currentMode));
        relayPrintStates(Serial);
    }
}

bool logicSetManualMode(SystemMode mode) {
    manualMode = mode;

    if (currentControlMode == ControlMode::MANUAL) {
        currentMode = manualMode;
        updateRelaysForMode(currentMode);

        Serial.print(F("[LOGIC] MANUAL mode set to "));
        Serial.println(logicModeToString(currentMode));
        relayPrintStates(Serial);
    }
    #if defined(FEATURE_BUZZER)
    buzzerOnManualModeChanged(logicModeToString(mode));
    #endif
    return true;
}

bool logicSetManualModeByName(const String& name) {
    String s = name;
    s.toUpperCase();

    if (s == "MODE0") {
        return logicSetManualMode(SystemMode::MODE1);
    } else if (s == "MODE1") {
        return logicSetManualMode(SystemMode::MODE1);
    } else if (s == "MODE2") {
        return logicSetManualMode(SystemMode::MODE2);
    } else if (s == "MODE3") {
        return logicSetManualMode(SystemMode::MODE3);
    } else if (s == "MODE4") {
        return logicSetManualMode(SystemMode::MODE4);
    } else if (s == "MODE5") {
        return logicSetManualMode(SystemMode::MODE5);
    }

    return false;
}

void logicInit() {
    // Výchozí stav po startu: AUTO (řídí vstupy/relayMap, pokud nejsou aktivní pravidla)
    currentControlMode = ControlMode::AUTO;
    manualMode         = SystemMode::MODE1;
    currentMode        = manualMode;

    s_autoStatus = AutoStatus{ false, 0, currentMode, false };
    s_eqBootHomingArmed = true;

    // V AUTO rovnou dopočítej výstupy podle vstupů (ať po bootu odpovídá skutečnému stavu).
    logicRecomputeFromInputs();

    Serial.print(F("[LOGIC] Init, control=AUTO, mode="));
    Serial.println(logicModeToString(currentMode));
}

void logicUpdate() {
    // časové funkce (scheduler + TUV)
    static uint32_t lastTickMs = 0;
    const uint32_t nowMs = millis();
    valveTick(nowMs);
    // Aktualizace teplot (TEMP1..TEMP8): Dallas
    for (uint8_t i=0;i<INPUT_COUNT;i++){
        if (dallasIsValid(i)) { s_tempValid[i]=true; s_tempC[i]=dallasGetTempC(i); }
        else { s_tempValid[i]=false; /* keep last value to show on dashboard */ }
    }
    updateInputBasedModes();
    equithermRecompute();
    equithermControlTick(nowMs);
    updateTuvModeState(nowMs);
    applyOpenThermMapping(nowMs);
    recircUpdate(nowMs);
    akuHeaterUpdate(nowMs);
    applyNightModeRelay();
    if (nowMs - lastTickMs < 250) {
        // still enforce TUV output frequently (no flicker)
        applyTuvRequest();
        applyOpenThermMapping(nowMs);
        return;
    }
    lastTickMs = nowMs;

    struct tm t;
    time_t epoch;
    if (isValidTimeNow(t, epoch)) {
        const uint32_t key = minuteKey(t);
        const uint8_t dowMask = dowToMask(t);
        const bool hasTuvEnableInput = (s_tuvEnableInput >= 0 && s_tuvEnableInput < (int8_t)INPUT_COUNT);
        const bool hasNightModeInput = (s_nightModeSource == "input" && s_nightModeInput >= 0 && s_nightModeInput < (int8_t)INPUT_COUNT);

        for (uint8_t i=0;i<s_scheduleCount;i++) {
            ScheduleItem &s = s_schedules[i];
            if (!s.enabled) continue;
            if ((s.daysMask & dowMask) == 0) continue;
            if (t.tm_hour != s.hour || t.tm_min != s.minute) continue;
            if (s.lastFiredMinuteKey == key) continue; // already fired this minute

            s.lastFiredMinuteKey = key;

            switch (s.kind) {
                case ScheduleKind::SET_MODE:
                    // jednotne chovani (buzzer + pripadna okamzita aplikace v MANUAL)
                    logicSetManualMode(s.modeValue);
                    Serial.printf("[SCHED] set_mode -> %s\n", logicModeToString(manualMode));
                    break;

                case ScheduleKind::SET_CONTROL_MODE:
                    // pouzij setter kvuli konzistenci (buzzer + side effects)
                    logicSetControlMode(s.controlValue);
                    Serial.printf("[SCHED] control_mode -> %s\n", currentControlMode == ControlMode::AUTO ? "AUTO" : "MANUAL");
                    break;

                case ScheduleKind::dhw_enable:
                    if (hasTuvEnableInput) break;
                    s_tuvScheduleEnabled = s.enableValue;
                    updateTuvModeState(nowMs);
                    Serial.printf("[SCHED] TUV -> %s\n", s_tuvScheduleEnabled ? "ON" : "OFF");
                    break;

                case ScheduleKind::NIGHT_MODE:
                    if (hasNightModeInput || s_nightModeSource != "schedule") break;
                    s_nightMode = s.enableValue;
                    Serial.printf("[SCHED] night_mode -> %s\n", s_nightMode ? "ON" : "OFF");
                    break;
            }
        }
    } else {
        // No valid time -> still update TUV from input
        updateTuvModeState(nowMs);
    }

    // Always enforce TUV request relay after mode updates have run
    applyTuvRequest();
    applyNightModeRelay();
}

void logicOnInputChanged(InputId id, bool newState) {
    (void)newState;

    if (s_tuvEnableInput >= 0 && id == static_cast<InputId>(s_tuvEnableInput)) {
        s_tuvScheduleEnabled = inputGetState(id);
        updateTuvModeState(millis());
        applyTuvRequest();
    }
    if (s_tuvDemandInput >= 0 && id == static_cast<InputId>(s_tuvDemandInput)) {
        s_tuvDemandActive = inputGetState(id);
        updateTuvModeState(millis());
        applyTuvRequest();
    }
    if (s_nightModeSource == "input" && s_nightModeInput >= 0 && id == static_cast<InputId>(s_nightModeInput)) {
        s_nightMode = inputGetState(id);
        equithermRecompute();
    }
    if (s_heatCallInput >= 0 && id == static_cast<InputId>(s_heatCallInput)) {
        s_heatCallActive = inputGetState(id);
        if (s_nightModeSource == "heat_call") {
            s_nightMode = !s_heatCallActive;
            equithermRecompute();
        }
    }
    applyNightModeRelay();

    if (currentControlMode != ControlMode::AUTO) {
        return;
    }

    logicRecomputeFromInputs();
}

// --- Config apply (ArduinoJson) stability ---

static constexpr size_t LOGIC_CONFIG_JSON_HARD_CAP_BYTES = 64UL * 1024UL;
// Max capacity for filtered config doc. Sized to handle typical 20-40 KB configs with margin.
static constexpr size_t LOGIC_CONFIG_DOC_CAP_BYTES = 64UL * 1024UL;
// Filter is intentionally tiny: we include whole supported sections instead of enumerating every key.
static constexpr size_t LOGIC_CONFIG_FILTER_CAP_BYTES = 2048;

static DynamicJsonDocument g_logicApplyDoc(LOGIC_CONFIG_DOC_CAP_BYTES);
static StaticJsonDocument<LOGIC_CONFIG_FILTER_CAP_BYTES> g_logicApplyFilter;

static uint32_t g_logicApplyOk = 0;
static uint32_t g_logicApplyFail = 0;
static uint32_t g_logicApplyNoMem = 0;
static uint32_t g_logicApplyFilterOverflow = 0;
static uint32_t g_logicApplyOversize = 0;
static uint32_t g_logicApplyLastFailMs = 0;
static uint32_t g_logicApplyLastInputLen = 0;
static char     g_logicApplyLastErr[160] = {0};

static void logicSetApplyErr(const char* msg, uint32_t inputLen) {
    g_logicApplyFail++;
    g_logicApplyLastFailMs = millis();
    g_logicApplyLastInputLen = inputLen;
    if (msg) {
        snprintf(g_logicApplyLastErr, sizeof(g_logicApplyLastErr), "%s", msg);
    } else {
        g_logicApplyLastErr[0] = 0;
    }
}

static void logicLogApplyCapInsufficient(const char* which, uint32_t inputLen, size_t filterCap, size_t docCap) {
    Serial.print(F("[LOGIC] logicApplyConfig: filter/doc capacity insufficient ("));
    Serial.print(which ? which : "?");
    Serial.print(F(") inLen="));
    Serial.print(inputLen);
    Serial.print(F(" jsonHardCap="));
    Serial.print((uint32_t)LOGIC_CONFIG_JSON_HARD_CAP_BYTES);
    Serial.print(F(" filterCap="));
    Serial.print((uint32_t)filterCap);
    Serial.print(F(" docCap="));
    Serial.println((uint32_t)docCap);
}

// ===== Aplikace konfigurace z JSON =====
//
// Očekávaný tvar v config.json:
//
//  "inputActiveLevels": [1,1,1,1,1,1,1,1]
//
//  "relayMap": [
//    {"input":0..8, "polarity":0|1}, ... (8 položek)
//  ]
//
//  "modes": [
//    { "id":"MODE1",   "name":"...", "description":"...", "triggerInput":0..8, "relayStates":[...] },
//    { "id":"MODE2", "name":"...", "description":"...", "triggerInput":0..8, "relayStates":[...] },
//    ...
//  ]
//
void logicApplyConfig(const String& json) {
    const uint32_t inputLen = (uint32_t)json.length();
    if (inputLen == 0) {
        Serial.println(F("[LOGIC] logicApplyConfig: empty JSON"));
        return;
    }

    if (inputLen > LOGIC_CONFIG_JSON_HARD_CAP_BYTES) {
        g_logicApplyOversize++;
        logicLogApplyCapInsufficient("oversize", inputLen, LOGIC_CONFIG_FILTER_CAP_BYTES, g_logicApplyDoc.capacity());
        logicSetApplyErr("config too large", inputLen);
        return;
    }

    // Build a *small* filter: include whole supported sections instead of listing every nested key.
    StaticJsonDocument<LOGIC_CONFIG_FILTER_CAP_BYTES>& filter = g_logicApplyFilter;
    filter.clear();

    filter["inputActiveLevels"] = true;
    filter["inputs"] = true;
    filter["relayMap"] = true;
    filter["autoDefaultOffUnmapped"] = true;
    filter["auto_default_off_unmapped"] = true;
    filter["modes"] = true;

    // Whole sections (robust against future key additions)
    filter["iofunc"] = true;
    filter["tempRoles"] = true;
    filter["equitherm"] = true;
    filter["nightMode"] = true;
    filter["system"] = true;
    filter["dhwRecirc"] = true;
    filter["tuv"] = true;
    filter["boiler"] = true;
    filter["akuHeater"] = true;
    filter["schedules"] = true;
    filter["sensors"] = true;

    // Legacy top-level keys used for backward compatibility
    filter["tuvDemandInput"] = true;
    filter["tuv_demand_input"] = true;
    filter["tuvRelay"] = true;
    filter["tuv_relay"] = true;
    filter["tuvEnabled"] = true;
    filter["tuvValveTargetPct"] = true;
    filter["tuv_valve_target_pct"] = true;
    filter["tuvEqValveTargetPct"] = true;
    filter["tuv_eq_valve_target_pct"] = true;

    if (filter.overflowed()) {
        g_logicApplyFilterOverflow++;
        logicLogApplyCapInsufficient("filter overflow", inputLen, filter.capacity(), g_logicApplyDoc.capacity());
        logicSetApplyErr("filter overflow", inputLen);
        return;
    }

    DynamicJsonDocument& doc = g_logicApplyDoc;
    doc.clear();

    DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
    if (err) {
        if (err == DeserializationError::NoMemory) {
            g_logicApplyNoMem++;
            logicLogApplyCapInsufficient("doc NoMemory", inputLen, filter.capacity(), doc.capacity());
        } else {
            Serial.print(F("[LOGIC] Config JSON parse error: "));
            Serial.println(err.c_str());
        }
        logicSetApplyErr(err.c_str(), inputLen);
        return;
    }

    s_eqValveConfigChanged = false;
// success
    g_logicApplyOk++;
    g_logicApplyLastErr[0] = 0;
    // reset relayMap + triggers (umožní "smazat" mapování v UI)
    for (uint8_t r = 0; r < RELAY_COUNT; r++) {
        relayMap[r].input = 0;
        relayMap[r].polarity = 1;
    }
    for (uint8_t i = 0; i < 5; i++) {
        modeTriggerInput[i] = 0;
    }

    // reset templates/sensors
    // Pozn.: při změně konfigurace 3c ventilů dříve docházelo k resetu
    // odhadované polohy na default, i když fyzicky ventil zůstal jinde.
    // Proto si před resetem uložíme odhad polohy a po opětovném nastavení
    // stejného master relé ji obnovíme.
    struct ValveSnapshot {
        bool valid = false;
        uint8_t posPct = 0;
    } snap[RELAY_COUNT];

    const uint32_t nowMsSnap = millis();
    for (uint8_t i=0; i<RELAY_COUNT; i++) {
        if (s_valves[i].configured) {
            snap[i].valid = true;
            snap[i].posPct = valveComputePosPct(s_valves[i], nowMsSnap);
        }
    }

    valveResetAll();

    // inputActiveLevels -> ConfigStore (pokud existuje)
    JsonArray ial = doc["inputActiveLevels"].as<JsonArray>();
    if (!ial.isNull()) {
        uint8_t levels[INPUT_COUNT];
        uint8_t cnt = ial.size();
        if (cnt > INPUT_COUNT) cnt = INPUT_COUNT;

        for (uint8_t i = 0; i < cnt; i++) {
            int v = ial[i] | 1;
            levels[i] = (v ? 1 : 0);
        }
        ConfigStore::setInputActiveLevels(levels, cnt);
    } else {
        // Kompatibilita s WebUI: cfg.inputs[i].activeLevel = "LOW" | "HIGH"
        JsonArray inputs = doc["inputs"].as<JsonArray>();
        if (!inputs.isNull()) {
            uint8_t levels[INPUT_COUNT];
            uint8_t cnt = inputs.size();
            if (cnt > INPUT_COUNT) cnt = INPUT_COUNT;

            for (uint8_t i = 0; i < cnt; i++) {
                JsonObject io = inputs[i].as<JsonObject>();
                if (io.isNull()) {
                    levels[i] = 0; // default LOW active
                    continue;
                }

                const char* al = io["activeLevel"] | io["active_level"] | "LOW";
                String s(al);
                s.toUpperCase();
                // 0 = LOW aktivní, 1 = HIGH aktivní
                levels[i] = (s == "HIGH") ? 1 : 0;
            }

            ConfigStore::setInputActiveLevels(levels, cnt);
        }
    }

    
    // --------------------------------------------------------------------
    // FIXED mapping pro tento projekt/řídicí jednotku (viz konstanty nahoře)
    //
    // Důvod: aby se zařízení nerozbilo špatným nastavením v UI/configu.
    // --------------------------------------------------------------------
    s_tuvEnableInput = -1;                 // zůstává volitelné (ale ne mapované na IN1..3)
    s_nightModeInput = (int8_t)FIX_IN_NIGHT_MODE0;        // IN2
    s_heatCallInput = -1;                  // nepoužíváme jako zdroj nightMode v tomto projektu
    s_recircDemandInput = (int8_t)FIX_IN_RECIRC_DEMAND0;  // IN3
    s_boilerDhwRelay = (int8_t)FIX_RELAY_BOILER_DHW_REQ0; // R5
    s_boilerNightRelay = (int8_t)FIX_RELAY_BOILER_NIGHT0; // R6
    s_recircPumpRelayRole = (int8_t)FIX_RELAY_RECIRC_PUMP0; // R7
    s_akuHeaterRelay = (int8_t)FIX_RELAY_AKU_HEATER0;        // R8

    // ---------------- I/O funkce (roles/templates) ----------------
    JsonObject iof = doc["iofunc"].as<JsonObject>();
    if (!iof.isNull()) {

        // Inputs: např. vstupní role
        JsonArray in = iof["inputs"].as<JsonArray>();
        if (!in.isNull()) {
            const uint8_t cnt = (in.size() > INPUT_COUNT) ? INPUT_COUNT : (uint8_t)in.size();
            for (uint8_t i=0;i<cnt;i++){
                JsonObject io = in[i].as<JsonObject>();
                const String role = String((const char*)(io["role"] | "none"));

                // FIXED mapping: následující role jsou v tomto firmware pevně dané
                // (IN1..IN3) a konfigurací se nemění.
                if (role == "dhw_enable" || role == "night_mode" || role == "recirc_demand") {
                    continue;
                } else if (role == "thermostat" || role == "heat_call") {
                    s_heatCallInput = (int8_t)i;
                }
            }
        }

        // Outputs: iofunc.outputs + 3c ventily
        JsonArray out = iof["outputs"].as<JsonArray>();
        if (!out.isNull()) {
            const uint8_t cnt = (out.size() > RELAY_COUNT) ? RELAY_COUNT : (uint8_t)out.size();
            bool mixConfigured = false;

            auto configureValve2Rel = [&](uint8_t master0, uint8_t peer0, JsonObject params) {
                if (master0 >= RELAY_COUNT || peer0 >= RELAY_COUNT || peer0 == master0) return;
                if (isFixedFunctionRelay(master0) || isFixedFunctionRelay(peer0)) return;

                // peer nesmí být už použitý
                if (s_valvePeerOf[peer0] >= 0) return;
                // peer nesmí být zároveň master jiného ventilu
                if (s_valves[peer0].configured) return;

                Valve3WayState &v = s_valves[master0];
const bool oldConfigured = v.configured;
const uint32_t oldTravelMs = v.travelMs;
const uint32_t oldPulseMs  = v.pulseMs;
const uint32_t oldGuardMs  = v.guardMs;
const uint32_t oldMinSwitchMs = v.minSwitchMs;
const bool oldInvert = v.invertDir;
const bool oldDefaultB = v.defaultB;
const uint8_t oldRelayB = v.relayB;

                v.configured = true;
                v.singleRelay = false;
                v.relayA = master0;
                v.relayB = peer0;

                // Defaulty musí být konzistentní s UI.
                // Směšovací ventil v tomto projektu: A→B cca 6 s, pulz ~0.6 s.
                v.travelMs = (uint32_t)(jsonGetFloat2(params, "travelTime", "travelTimeS", 6.0f) * 1000.0f);
                v.pulseMs  = (uint32_t)(jsonGetFloat2(params, "pulseTime",  "pulseTimeS",  0.6f) * 1000.0f);
                v.guardMs  = (uint32_t)(jsonGetFloat2(params, "guardTime",  "guardTimeS",  0.3f) * 1000.0f);
                v.minSwitchMs = (uint32_t)(jsonGetFloat2(params, "minSwitchS", "minSwitch", 1.0f) * 1000.0f);
                if (v.minSwitchMs > 3600000UL) v.minSwitchMs = 3600000UL;

                v.lastCmdMs = 0;
                v.hasPending = false;
                v.invertDir = (bool)(params["invertDir"] | false);
                const String defPos = String((const char*)(params["defaultPos"] | "A"));
                v.defaultB = (defPos.equalsIgnoreCase("B"));

// detect config change (used for homing)
if (master0 == FIX_RELAY_MIX_A0) {
    const bool changed =
        (!oldConfigured) ||
        (oldRelayB != v.relayB) ||
        (oldTravelMs != v.travelMs) ||
        (oldPulseMs  != v.pulseMs) ||
        (oldGuardMs  != v.guardMs) ||
        (oldMinSwitchMs != v.minSwitchMs) ||
        (oldInvert != v.invertDir) ||
        (oldDefaultB != v.defaultB);
    if (changed) s_eqValveConfigChanged = true;
}

                uint8_t initPct = v.defaultB ? 100 : 0;
                if (snap[master0].valid) initPct = snap[master0].posPct;

                v.posPct = initPct;
                v.startPct = initPct;
                v.targetPct = initPct;
                v.currentB = (initPct >= 50);
                v.pendingTargetPct = initPct;
                v.hasPending = false;

                v.moving = false;
                v.moveStartMs = 0;
                v.moveEndMs = 0;
                v.guardEndMs = 0;

                s_valvePeerOf[peer0] = (int8_t)master0;

                relaySet(static_cast<RelayId>(v.relayA), false);
                relaySet(static_cast<RelayId>(v.relayB), false);
            };

            auto configureValveSingle = [&](uint8_t master0, JsonObject params) {
                if (master0 >= RELAY_COUNT) return;

                Valve3WayState &v = s_valves[master0];
const bool oldConfigured = v.configured;
const uint32_t oldTravelMs = v.travelMs;
const uint32_t oldPulseMs  = v.pulseMs;
const uint32_t oldGuardMs  = v.guardMs;
const uint32_t oldMinSwitchMs = v.minSwitchMs;
const bool oldInvert = v.invertDir;
const bool oldDefaultB = v.defaultB;
const uint8_t oldRelayB = v.relayB;

                v.configured = true;
                v.singleRelay = true;
                v.relayA = master0;
                v.relayB = master0;

                // Přepínací ventil (single relay): A→B cca 6 s.
                v.travelMs = (uint32_t)(jsonGetFloat2(params, "travelTime", "travelTimeS", 6.0f) * 1000.0f);
                v.pulseMs  = (uint32_t)(jsonGetFloat2(params, "pulseTime",  "pulseTimeS",  0.8f) * 1000.0f);
                v.guardMs  = (uint32_t)(jsonGetFloat2(params, "guardTime",  "guardTimeS",  0.3f) * 1000.0f);
                v.minSwitchMs = (uint32_t)(jsonGetFloat2(params, "minSwitchS", "minSwitch", 1.0f) * 1000.0f);
                if (v.minSwitchMs > 3600000UL) v.minSwitchMs = 3600000UL;

                v.lastCmdMs = 0;
                v.hasPending = false;
                v.invertDir = (bool)(params["invertDir"] | false);
                const String defPos = String((const char*)(params["defaultPos"] | "A"));
                v.defaultB = (defPos.equalsIgnoreCase("B"));

// detect config change (used for homing)
if (master0 == FIX_RELAY_MIX_A0) {
    const bool changed =
        (!oldConfigured) ||
        (oldRelayB != v.relayB) ||
        (oldTravelMs != v.travelMs) ||
        (oldPulseMs  != v.pulseMs) ||
        (oldGuardMs  != v.guardMs) ||
        (oldMinSwitchMs != v.minSwitchMs) ||
        (oldInvert != v.invertDir) ||
        (oldDefaultB != v.defaultB);
    if (changed) s_eqValveConfigChanged = true;
}

                uint8_t initPct = v.defaultB ? 100 : 0;
                if (snap[master0].valid) initPct = snap[master0].posPct;

                v.posPct = initPct;
                v.startPct = initPct;
                v.targetPct = initPct;
                v.currentB = (initPct >= 50);
                v.pendingTargetPct = initPct;
                v.hasPending = false;

                v.moving = false;
                v.moveStartMs = 0;
                v.moveEndMs = 0;
                v.guardEndMs = 0;

                relaySet(static_cast<RelayId>(v.relayA), false);
            };

            for (uint8_t i=0;i<cnt;i++){
                JsonObject oo = out[i].as<JsonObject>();
                JsonObject params = oo["params"].as<JsonObject>();
                const String role = String((const char*)(oo["role"] | "none"));

                // Směšovací ventil (Ekviterm) je v tomto projektu pevně na R1+R2.
                // Konfigurovatelné jsou pouze parametry (travel/pulse/guard/minSwitch/invert/defaultPos).
                if (i == (uint8_t)FIX_RELAY_MIX_A0 && (role == "valve_3way_mix" || role == "valve_3way_2rel")) {
                    configureValve2Rel(FIX_RELAY_MIX_A0, FIX_RELAY_MIX_B0, params);
                    if (s_valves[FIX_RELAY_MIX_A0].configured) mixConfigured = true;
                    continue;
                }
                // Pokud je role směšovacího ventilu omylem uložená jinde, ignoruj ji.
                if (role == "valve_3way_mix" || role == "valve_3way_2rel") {
                    continue;
                }

                // TUV přepínací ventil je pevně R3 (single relay). Případné role v configu ignorujeme.
                if (role == "valve_3way_tuv" || role == "valve_3way_dhw" || role == "valve_3way_spring") {
                    continue;
                }

                if (role == "boiler_enable_dhw") {
                    s_boilerDhwRelay = (int8_t)i;
                } else if (role == "boiler_enable_nm") {
                    s_boilerNightRelay = (int8_t)i;
                } else if (role == "heater_aku") {
                    s_akuHeaterRelay = (int8_t)i;
                } else if (role == "dhw_recirc_pump" || role == "circ_pump") {
                    s_recircPumpRelayRole = (int8_t)i;
                }
            }

            StaticJsonDocument<8> emptyDoc;
            JsonObject emptyParams = emptyDoc.to<JsonObject>();

            // Pevně nastavený TUV přepínací ventil: R3 (single relay)
            JsonObject r3Params;
            if (out.size() > FIX_RELAY_TUV_VALVE0) {
                JsonObject r3Obj = out[FIX_RELAY_TUV_VALVE0].as<JsonObject>();
                r3Params = r3Obj["params"].as<JsonObject>();
            }
            configureValveSingle(FIX_RELAY_TUV_VALVE0, r3Params.isNull() ? emptyParams : r3Params);

            // Fallback (pokud není v configu žádný směšovací ventil): R1+R2
            if (!mixConfigured) {
                JsonObject r1Params;
                if (out.size() > FIX_RELAY_MIX_A0) {
                    JsonObject r1Obj = out[FIX_RELAY_MIX_A0].as<JsonObject>();
                    r1Params = r1Obj["params"].as<JsonObject>();
                }
                configureValve2Rel(FIX_RELAY_MIX_A0, FIX_RELAY_MIX_B0, r1Params.isNull() ? emptyParams : r1Params);
            }
        }
    }


// Safety fallback: ensure the Ekviterm mixing 3-way valve (R1+R2) is configured
// even if the config.json is missing iofunc.outputs (e.g. partial PATCH save from UI).
if (!s_valves[FIX_RELAY_MIX_A0].configured) {
    Valve3WayState &v = s_valves[FIX_RELAY_MIX_A0];
    v.configured = true;
    v.singleRelay = false;
    v.relayA = FIX_RELAY_MIX_A0; // R1
    v.relayB = FIX_RELAY_MIX_B0; // R2

    // Use defaults consistent with UI (can be overridden when iofunc.outputs is present).
    v.travelMs = 6000;
    v.pulseMs  = 600;
    v.guardMs  = 300;
    v.minSwitchMs = 1000;

    v.lastCmdMs = 0;
    v.hasPending = false;
    v.invertDir = false;
    v.defaultB = false;

    uint8_t initPct = 0;
    v.posPct = initPct;
    v.startPct = initPct;
    v.targetPct = initPct;
    v.currentB = false;
    v.pendingTargetPct = initPct;
    v.hasPending = false;

    v.moving = false;
    v.moveStartMs = 0;
    v.moveEndMs = 0;
    v.guardEndMs = 0;

    s_valvePeerOf[FIX_RELAY_MIX_B0] = (int8_t)FIX_RELAY_MIX_A0;

    relaySet(static_cast<RelayId>(v.relayA), false);
    relaySet(static_cast<RelayId>(v.relayB), false);
}

    // Enforce fixed mapping (nesmí být přepsáno konfigurací)
    s_nightModeInput = (int8_t)FIX_IN_NIGHT_MODE0;
    s_recircDemandInput = (int8_t)FIX_IN_RECIRC_DEMAND0;
    s_boilerDhwRelay = (int8_t)FIX_RELAY_BOILER_DHW_REQ0;
    s_boilerNightRelay = (int8_t)FIX_RELAY_BOILER_NIGHT0;
    s_recircPumpRelayRole = (int8_t)FIX_RELAY_RECIRC_PUMP0;
    s_akuHeaterRelay = (int8_t)FIX_RELAY_AKU_HEATER0;


    // ---------------- Equitherm konfigurace ----------------
    JsonObject eq = doc["equitherm"].as<JsonObject>();
    if (!eq.isNull()) {
        s_eqEnabled = (bool)(eq["enabled"] | false);

        // defaults
        s_eqOutdoorCfg = EqSourceCfg{};
        s_eqFlowCfg    = EqSourceCfg{};
        s_eqBoilerInCfg = EqSourceCfg{};
        s_eqAkuTopCfg  = EqSourceCfg{};
        s_eqAkuMidCfg  = EqSourceCfg{};
        s_eqAkuBottomCfg = EqSourceCfg{};

        auto parseSrc = [](JsonObject o, EqSourceCfg& dst){
            if (o.isNull()) return;
            dst.source = String((const char*)(o["source"] | dst.source.c_str()));
            dst.gpio   = (int)(o["gpio"] | dst.gpio);
            dst.romHex = String((const char*)(o["rom"] | o["addr"] | dst.romHex.c_str()));
            dst.topic  = String((const char*)(o["topic"] | dst.topic.c_str()));
            dst.jsonKey = String((const char*)(o["jsonKey"] | o["key"] | o["field"] | dst.jsonKey.c_str()));
            dst.mqttIdx = (uint8_t)(o["mqttIdx"] | o["preset"] | dst.mqttIdx);
            dst.maxAgeMs = (uint32_t)(o["maxAgeMs"] | o["max_age_ms"] | dst.maxAgeMs);
            dst.bleId  = String((const char*)(o["bleId"] | o["id"] | dst.bleId.c_str()));
        };

        JsonObject outdoor = eq["outdoor"].as<JsonObject>();
        parseSrc(outdoor, s_eqOutdoorCfg);

        JsonObject flow = eq["flow"].as<JsonObject>();
        parseSrc(flow, s_eqFlowCfg);
        JsonObject boilerIn = eq["boilerIn"].as<JsonObject>();
        parseSrc(boilerIn, s_eqBoilerInCfg);

        JsonObject akuTop = eq["akuTop"].as<JsonObject>();
        parseSrc(akuTop, s_eqAkuTopCfg);

        JsonObject akuMid = eq["akuMid"].as<JsonObject>();
        parseSrc(akuMid, s_eqAkuMidCfg);

        JsonObject akuBottom = eq["akuBottom"].as<JsonObject>();
        parseSrc(akuBottom, s_eqAkuBottomCfg);

        // ---------------- Role fallback (TEMP1..8 / BLE / MQTT) ----------------
        // Základní konfigurace probíhá na stránce "Teploměry" (role pro TEMP1..8,
        // BLE a MQTT). Pokud není v equitherm.*.source explicitně nastaven zdroj,
        // odvodíme ho z rolí.
        JsonArray tempRoles = doc["tempRoles"].as<JsonArray>();

        auto findTempIdxByRole = [&](const String& wantedNorm) -> int {
            if (tempRoles.isNull()) return -1;
            int i = 0;
            for (JsonVariant v : tempRoles) {
                if (i >= (int)INPUT_COUNT) break;
                const char* raw = v.as<const char*>();
                if (thermoNormalizeRole(String(raw ? raw : "")) == wantedNorm) return i;
                i++;
            }
            return -1;
        };

        auto applyThermoRoleFallback = [&](EqSourceCfg& dst, const char* roleKey) {
            if (dst.source.length() && dst.source != "none") return;

            const String wantedNorm = thermoNormalizeRole(String(roleKey ? roleKey : ""));
            if (!wantedNorm.length()) return;

            // 1) Prefer GPIO TEMP1..8 roles first
            const int idx = findTempIdxByRole(wantedNorm);
            if (idx >= 0) {
                dst.source = String("temp") + String(idx + 1);
                return;
            }

            // 2) BLE virtual thermometer by role ("Teploměry" -> BLE)
            const BleThermometerCfg& bc = thermometersGetBle();
            const String bleRoleNorm = thermoNormalizeRole(bc.role);
            if (bleRoleNorm.length() && bleRoleNorm == wantedNorm) {
                dst.source = "ble";
                // bleId může být prázdné -> bleGetTempCById("", ...) použije default meteo
                dst.bleId = bc.id;
                return;
            }

            // 3) MQTT virtual thermometers by role ("Teploměry" -> MQTT sloty)
            for (uint8_t mi = 0; mi < 2; mi++) {
                const MqttThermometerCfg& mc = thermometersGetMqtt(mi);
                if (!mc.topic.length()) continue;
                const String mqttRoleNorm = thermoNormalizeRole(mc.role);
                if (mqttRoleNorm.length() && mqttRoleNorm == wantedNorm) {
                    dst.source = "mqtt";
                    dst.mqttIdx = (uint8_t)(mi + 1);
                    return;
                }
            }
        };

        applyThermoRoleFallback(s_eqOutdoorCfg, "outdoor");
        applyThermoRoleFallback(s_eqFlowCfg, "flow");
        applyThermoRoleFallback(s_eqAkuTopCfg, "aku_top");
        applyThermoRoleFallback(s_eqAkuMidCfg, "aku_mid");
        applyThermoRoleFallback(s_eqAkuBottomCfg, "aku_bottom");

        // kompatibilita se staršími konfiguracemi (jen MQTT venek)
        if (!s_eqOutdoorCfg.source.length()) s_eqOutdoorCfg.source = "none";
        if (s_eqOutdoorCfg.source == "mqtt" && !s_eqOutdoorCfg.topic.length()) {
            s_eqOutdoorCfg.topic = String((const char*)(outdoor["topic"] | ""));
        }
        if (!s_eqBoilerInCfg.source.length() || s_eqBoilerInCfg.source == "none") {
            s_eqBoilerInCfg = s_eqFlowCfg;
        }
        if (s_eqOutdoorCfg.maxAgeMs > 3600000UL) s_eqOutdoorCfg.maxAgeMs = 3600000UL;
        if (s_eqFlowCfg.maxAgeMs > 3600000UL) s_eqFlowCfg.maxAgeMs = 3600000UL;
        if (s_eqBoilerInCfg.maxAgeMs > 3600000UL) s_eqBoilerInCfg.maxAgeMs = 3600000UL;
        if (s_eqAkuTopCfg.maxAgeMs > 3600000UL) s_eqAkuTopCfg.maxAgeMs = 3600000UL;
        if (s_eqAkuMidCfg.maxAgeMs > 3600000UL) s_eqAkuMidCfg.maxAgeMs = 3600000UL;
        if (s_eqAkuBottomCfg.maxAgeMs > 3600000UL) s_eqAkuBottomCfg.maxAgeMs = 3600000UL;

        s_eqMinFlow = (float)(eq["minFlow"] | s_eqMinFlow);
        s_eqMaxFlow = (float)(eq["maxFlow"] | s_eqMaxFlow);
        const float legacyTop = (float)(eq["akuMinTopC"] | s_eqAkuMinTopCDay);
        const float legacyDeltaTarget = (float)(eq["akuMinDeltaToTargetC"] | s_eqAkuMinDeltaToTargetCDay);
        const float legacyDeltaBoiler = (float)(eq["akuMinDeltaToBoilerInC"] | s_eqAkuMinDeltaToBoilerInCDay);
        s_eqAkuMinTopCDay = (float)(eq["akuMinTopC_day"] | eq["akuMinTopCDay"] | legacyTop);
        s_eqAkuMinTopCNight = (float)(eq["akuMinTopC_night"] | eq["akuMinTopCNight"] | legacyTop);
        s_eqAkuMinDeltaToTargetCDay = (float)(eq["akuMinDeltaToTargetC_day"] | eq["akuMinDeltaToTargetCDay"] | legacyDeltaTarget);
        s_eqAkuMinDeltaToTargetCNight = (float)(eq["akuMinDeltaToTargetC_night"] | eq["akuMinDeltaToTargetCNight"] | legacyDeltaTarget);
        s_eqAkuMinDeltaToBoilerInCDay = (float)(eq["akuMinDeltaToBoilerInC_day"] | eq["akuMinDeltaToBoilerInCDay"] | legacyDeltaBoiler);
        s_eqAkuMinDeltaToBoilerInCNight = (float)(eq["akuMinDeltaToBoilerInC_night"] | eq["akuMinDeltaToBoilerInCNight"] | legacyDeltaBoiler);
        s_eqAkuSupportEnabled = (bool)(eq["akuSupportEnabled"] | s_eqAkuSupportEnabled);
        s_eqAkuNoSupportBehavior = String((const char*)(eq["akuNoSupportBehavior"] | s_eqAkuNoSupportBehavior.c_str()));
        s_eqCurveOffsetC = (float)(eq["curveOffsetC"] | s_eqCurveOffsetC);
        s_eqMaxBoilerInC = (float)(eq["maxBoilerInC"] | s_eqMaxBoilerInC);
        s_eqNoFlowDetectEnabled = (bool)(eq["noFlowDetectEnabled"] | s_eqNoFlowDetectEnabled);
        s_eqNoFlowTimeoutMs = (uint32_t)(eq["noFlowTimeoutMs"] | s_eqNoFlowTimeoutMs);
        s_eqNoFlowTestPeriodMs = (uint32_t)(eq["noFlowTestPeriodMs"] | s_eqNoFlowTestPeriodMs);
        s_eqFallbackOutdoorC = (float)(eq["fallbackOutdoorC"] | s_eqFallbackOutdoorC);

        s_eqAkuNoSupportBehavior.toLowerCase();

        JsonObject sensors = doc["sensors"].as<JsonObject>();
        if (!sensors.isNull()) {
            JsonObject outdoor = sensors["outdoor"].as<JsonObject>();
            if (!outdoor.isNull()) {
                s_eqOutdoorMaxAgeMs = (uint32_t)(outdoor["maxAgeMs"] | s_eqOutdoorMaxAgeMs);
            }
        }
        if (s_eqOutdoorCfg.maxAgeMs == 0 && s_eqOutdoorMaxAgeMs > 0) {
            s_eqOutdoorCfg.maxAgeMs = s_eqOutdoorMaxAgeMs;
        }

        // curve: slope/shift preferované (UI)
        const bool hasSlope = eq.containsKey("slopeDay") || eq.containsKey("shiftDay") ||
                              eq.containsKey("slopeNight") || eq.containsKey("shiftNight");
        if (hasSlope) {
            s_eqSlopeDay   = (float)(eq["slopeDay"]   | s_eqSlopeDay);
            s_eqShiftDay   = (float)(eq["shiftDay"]   | s_eqShiftDay);
            s_eqSlopeNight = (float)(eq["slopeNight"] | s_eqSlopeNight);
            s_eqShiftNight = (float)(eq["shiftNight"] | s_eqShiftNight);
        }

        // refs fallback (legacy)
        JsonObject refs = eq["refs"].as<JsonObject>();
        if (!refs.isNull()) {
            JsonObject day = refs["day"].as<JsonObject>();
            if (!day.isNull()) {
                s_eqHeatTout1  = (float)(day["tout1"] | s_eqHeatTout1);
                s_eqHeatTflow1 = (float)(day["tflow1"] | s_eqHeatTflow1);
                s_eqHeatTout2  = (float)(day["tout2"] | s_eqHeatTout2);
                s_eqHeatTflow2 = (float)(day["tflow2"] | s_eqHeatTflow2);
            }
            JsonObject night = refs["night"].as<JsonObject>();
            if (!night.isNull()) {
                s_eqNightTout1  = (float)(night["tout1"] | s_eqNightTout1);
                s_eqNightTflow1 = (float)(night["tflow1"] | s_eqNightTflow1);
                s_eqNightTout2  = (float)(night["tout2"] | s_eqNightTout2);
                s_eqNightTflow2 = (float)(night["tflow2"] | s_eqNightTflow2);
            }
        }

        // pokud slope/shift není, dopočti z refs (aby i staré configy fungovaly stejně jako UI)
        if (!hasSlope) {
            auto derive = [](float tout1, float tflow1, float tout2, float tflow2, float &slopeOut, float &shiftOut){
                const float a1 = 20.0f - tout1;
                const float a2 = 20.0f - tout2;
                const float den = (a1 - a2);
                if (fabsf(den) < 0.0001f) return false;
                slopeOut = (tflow1 - tflow2) / den;
                shiftOut = tflow1 - a1 * slopeOut - 20.0f;
                return isfinite(slopeOut) && isfinite(shiftOut);
            };
            (void)derive(s_eqHeatTout1, s_eqHeatTflow1, s_eqHeatTout2, s_eqHeatTflow2, s_eqSlopeDay, s_eqShiftDay);
            (void)derive(s_eqNightTout1, s_eqNightTflow1, s_eqNightTout2, s_eqNightTflow2, s_eqSlopeNight, s_eqShiftNight);
        }

        // 3c směšovací ventil pro Ekviterm je v tomto projektu pevně na R1 (master) + R2 (peer).
        // Konfigurovatelné jsou jen parametry ventilu, ne výběr relé.
        s_eqValveMaster0 = -1;
        if (isValveMaster(FIX_RELAY_MIX_A0) && s_valves[FIX_RELAY_MIX_A0].configured && !s_valves[FIX_RELAY_MIX_A0].singleRelay) {
            s_eqValveMaster0 = (int8_t)FIX_RELAY_MIX_A0;
        } else {
            // Pojistka – pokud ventil není nakonfigurovaný, ekviterm se nebude řídit v AUTO.
            // (Necháme ho zapnutý kvůli diagnostice v UI.)
            s_eqValveMaster0 = -1;
        }


        // control params
        JsonObject ctrl = eq["control"].as<JsonObject>();
        if (!ctrl.isNull()) {
            s_eqDeadbandC = (float)(ctrl["deadbandC"] | ctrl["deadband"] | s_eqDeadbandC);
            s_eqStepPct   = (uint8_t)(ctrl["stepPct"] | ctrl["step"] | s_eqStepPct);
            s_eqPeriodMs  = (uint32_t)(ctrl["periodMs"] | ctrl["period"] | s_eqPeriodMs);
            s_eqMinPct    = (uint8_t)(ctrl["minPct"] | s_eqMinPct);
            const uint8_t legacyMax = (uint8_t)(ctrl["maxPct"] | 0);
            if (legacyMax > 0) {
                s_eqMaxPctDay = legacyMax;
                s_eqMaxPctNight = legacyMax;
            }
            s_eqMaxPctDay = (uint8_t)(ctrl["maxPct_day"] | ctrl["maxPctDay"] | s_eqMaxPctDay);
            s_eqMaxPctNight = (uint8_t)(ctrl["maxPct_night"] | ctrl["maxPctNight"] | s_eqMaxPctNight);
        }
        s_eqDeadbandC = (float)(eq["deadbandC"] | s_eqDeadbandC);
        s_eqStepPct = (uint8_t)(eq["stepPct"] | s_eqStepPct);
        s_eqPeriodMs = (uint32_t)(eq["controlPeriodMs"] | s_eqPeriodMs);
        s_eqMaxPctDay = (uint8_t)(eq["maxPct_day"] | eq["maxPctDay"] | s_eqMaxPctDay);
        s_eqMaxPctNight = (uint8_t)(eq["maxPct_night"] | eq["maxPctNight"] | s_eqMaxPctNight);
        // clamp
        if (s_eqStepPct > 25) s_eqStepPct = 25;
        if (!isfinite(s_eqDeadbandC) || s_eqDeadbandC < 0.0f) s_eqDeadbandC = 0.0f;
        if (s_eqDeadbandC > 5.0f) s_eqDeadbandC = 5.0f;
        if (s_eqPeriodMs < 500) s_eqPeriodMs = 500;
        if (s_eqPeriodMs > 600000) s_eqPeriodMs = 600000;
        if (s_eqMinPct > 100) s_eqMinPct = 100;
        if (s_eqMaxPctDay > 100) s_eqMaxPctDay = 100;
        if (s_eqMaxPctNight > 100) s_eqMaxPctNight = 100;
        if (s_eqMinPct > s_eqMaxPctDay) s_eqMinPct = s_eqMaxPctDay;
        if (s_eqMinPct > s_eqMaxPctNight) s_eqMinPct = s_eqMaxPctNight;
        if (!isfinite(s_eqAkuMinTopCDay) || s_eqAkuMinTopCDay < 0.0f) s_eqAkuMinTopCDay = 0.0f;
        if (!isfinite(s_eqAkuMinTopCNight) || s_eqAkuMinTopCNight < 0.0f) s_eqAkuMinTopCNight = 0.0f;
        if (!isfinite(s_eqAkuMinDeltaToTargetCDay) || s_eqAkuMinDeltaToTargetCDay < 0.0f) s_eqAkuMinDeltaToTargetCDay = 0.0f;
        if (!isfinite(s_eqAkuMinDeltaToTargetCNight) || s_eqAkuMinDeltaToTargetCNight < 0.0f) s_eqAkuMinDeltaToTargetCNight = 0.0f;
        if (!isfinite(s_eqAkuMinDeltaToBoilerInCDay) || s_eqAkuMinDeltaToBoilerInCDay < 0.0f) s_eqAkuMinDeltaToBoilerInCDay = 0.0f;
        if (!isfinite(s_eqAkuMinDeltaToBoilerInCNight) || s_eqAkuMinDeltaToBoilerInCNight < 0.0f) s_eqAkuMinDeltaToBoilerInCNight = 0.0f;
        if (!isfinite(s_eqCurveOffsetC)) s_eqCurveOffsetC = 0.0f;
        if (s_eqNoFlowTimeoutMs < 10000) s_eqNoFlowTimeoutMs = 10000;
        if (s_eqNoFlowTimeoutMs > 3600000UL) s_eqNoFlowTimeoutMs = 3600000UL;
        if (s_eqNoFlowTestPeriodMs < 10000) s_eqNoFlowTestPeriodMs = 10000;
        if (s_eqNoFlowTestPeriodMs > 3600000UL) s_eqNoFlowTestPeriodMs = 3600000UL;
        if (s_eqOutdoorMaxAgeMs < 1000) s_eqOutdoorMaxAgeMs = 1000;
        if (s_eqOutdoorMaxAgeMs > 3600000UL) s_eqOutdoorMaxAgeMs = 3600000UL;
        s_eqNoFlowActive = false;
        s_eqLastFlowChangeMs = 0;
        s_eqLastFlowC = NAN;
        s_eqNoFlowLastTestMs = 0;

        // pokud vybraný ventil není nakonfigurovaný jako 3c master, ignoruj
        if (s_eqValveMaster0 >= 0 && (!isValveMaster((uint8_t)s_eqValveMaster0) || s_valves[(uint8_t)s_eqValveMaster0].singleRelay)) s_eqValveMaster0 = -1;

        
// homing config (volitelné)
JsonObject hom = eq["homing"].as<JsonObject>();
if (!hom.isNull()) {
    s_eqHomingEnabled = (bool)(hom["enabled"] | s_eqHomingEnabled);
    s_eqHomingOnBoot = (bool)(hom["onBoot"] | s_eqHomingOnBoot);
    s_eqHomingOnConfigChange = (bool)(hom["onConfigChange"] | hom["on_config_change"] | s_eqHomingOnConfigChange);
    s_eqHomingPeriodMs = (uint32_t)(hom["periodMs"] | hom["period_ms"] | s_eqHomingPeriodMs);
    const int pct = (int)(hom["positionPct"] | hom["position_pct"] | (int)s_eqHomingTargetPct);
    s_eqHomingTargetPct = (uint8_t)clampPctInt(pct);
}

// Pokud se změnila konfigurace směšovacího ventilu (IOFUNC), můžeme provést homing,
// abychom sladili skutečný stav s interním modelem (bez koncáků jde o nejlepší dostupnou synchronizaci).
if (s_eqValveConfigChanged && s_eqHomingEnabled && s_eqHomingOnConfigChange) {
    equithermRequestHoming("config");
    // pokud jsme ještě nikdy nehomovali, nastav poslední čas na 0 – první tick provede homing hned
    s_eqLastHomingMs = 0;
}

// --- Konfigurační validace (pro UI/diagnostiku) ---
        s_eqConfigOk = true;
        s_eqConfigReason = "";
        s_eqConfigWarning = "";

        const bool flowCfgOk = s_eqFlowCfg.source.length() && s_eqFlowCfg.source != "none";
        const bool valveOk = (s_eqValveMaster0 >= 0) && isValveMaster((uint8_t)s_eqValveMaster0) && s_valves[(uint8_t)s_eqValveMaster0].configured;
        const bool outdoorCfgSet = s_eqOutdoorCfg.source.length() && s_eqOutdoorCfg.source != "none";

        if (!flowCfgOk) {
            s_eqConfigOk = false;
            s_eqConfigReason = "flow source not configured";
        } else if (!valveOk) {
            s_eqConfigOk = false;
            s_eqConfigReason = "3-way valve not configured";
        }

        if (!outdoorCfgSet) {
            // Ekviterm může použít auto BLE meteo fallback (pokud je k dispozici). Bereme jako warning.
            s_eqConfigWarning = "outdoor source not set (auto BLE fallback if available)";
        }

        equithermRecompute();
    } else {
        s_eqEnabled = false;
        s_eqValveMaster0 = -1;
        s_eqConfigOk = true;
        s_eqConfigReason = "";
        s_eqConfigWarning = "";
    }
    JsonObject sys = doc["system"].as<JsonObject>();
    if (!sys.isNull()) {
        s_systemProfile = String((const char*)(sys["profile"] | s_systemProfile.c_str()));
        s_nightModeSource = String((const char*)(sys["nightModeSource"] | s_nightModeSource.c_str()));
        s_nightModeManual = (bool)(sys["nightModeManual"] | s_nightModeManual);
    }
    JsonObject nightCfg = doc["nightMode"].as<JsonObject>();
    if (!nightCfg.isNull()) {
        s_nightModeSource = String((const char*)(nightCfg["source"] | s_nightModeSource.c_str()));
        s_nightModeManual = (bool)(nightCfg["manual"] | s_nightModeManual);
    }
    s_systemProfile.toLowerCase();
    s_nightModeSource.toLowerCase();

    // FIXED mapping: Denní/Noční režim je řízený vstupem IN2.
    // (Nastavení v UI/configu se ignoruje.)
    s_nightModeSource = "input";
    s_nightModeInput = (int8_t)FIX_IN_NIGHT_MODE0;

    if (s_nightModeInput >= 0 && s_nightModeInput < (int8_t)INPUT_COUNT && s_nightModeSource == "input") {
        s_nightMode = inputGetState(static_cast<InputId>(s_nightModeInput));
    } else if (s_nightModeSource == "manual") {
        s_nightMode = s_nightModeManual;
    } else if (s_nightModeSource == "heat_call") {
        s_nightMode = !s_heatCallActive;
    }
// relayMap
    JsonArray rm = doc["relayMap"].as<JsonArray>();
    if (!rm.isNull()) {
        uint8_t cnt = rm.size();
        if (cnt > RELAY_COUNT) cnt = RELAY_COUNT;

        for (uint8_t r = 0; r < cnt; r++) {
            JsonObject o = rm[r].as<JsonObject>();
            if (o.isNull()) continue;

            int in = o["input"] | 0;
            int pol = o["polarity"] | 1;

            if (in < 0) in = 0;
            if (in > INPUT_COUNT) in = 0;

            relayMap[r].input = static_cast<uint8_t>(in);
            relayMap[r].polarity = (pol ? 1 : 0);
        }
    }

    // AUTO fallback chování pro relé bez přiřazení (relayMap.input == 0)
    // default: true (relé se v AUTO při "žádný trigger" a "bez mapování" vypne)
    // Klíče podporujeme ve více variantách (kompatibilita):
    //  - autoDefaultOffUnmapped
    //  - auto_default_off_unmapped
    if (doc.containsKey("autoDefaultOffUnmapped") || doc.containsKey("auto_default_off_unmapped")) {
        s_autoDefaultOffUnmapped =
            (bool)(doc["autoDefaultOffUnmapped"] | doc["auto_default_off_unmapped"] | true);
    }

    // modes profiles + triggers
    JsonArray modes = doc["modes"].as<JsonArray>();
    if (!modes.isNull()) {
        for (JsonVariant mv : modes) {
            JsonObject v = mv.as<JsonObject>();
            if (v.isNull()) continue;

            const char* id = v["id"] | "";
            String sid(id);
            sid.toUpperCase();

            JsonArray rs = v["relayStates"].as<JsonArray>();
            if (rs.isNull()) continue;

            RelayProfile* target = nullptr;
            int modeIndex = -1;

            if (sid == "MODE1") {
                target = &profileMODE1;
                modeIndex = 0;
            } else if (sid == "MODE2") {
                target = &profileTopeniTopit;
                modeIndex = 1;
            } else if (sid == "MODE3") {
                target = &profileTopeniUtlum;
                modeIndex = 2;
            } else if (sid == "MODE4") {
                target = &profileMode4;
                modeIndex = 3;
            } else if (sid == "MODE5") {
                target = &profileMode5;
                modeIndex = 4;
            } else {
                continue;
            }

            // triggerInput
            if (modeIndex >= 0) {
                int trig = v["triggerInput"] | 0;
                if (trig < 0) trig = 0;
                if (trig > INPUT_COUNT) trig = 0;
                modeTriggerInput[modeIndex] = static_cast<uint8_t>(trig);
            }

            // začneme z aktuálního profilu (výchozí hodnoty)
            RelayProfile newProfile = *target;

            uint8_t cnt = rs.size();
            if (cnt > RELAY_COUNT) cnt = RELAY_COUNT;

            for (uint8_t i = 0; i < cnt; i++) {
                // UI posílá relayStates typicky jako true/false.
                // Čtení jako bool je nejrobustnější (funguje i pro 0/1).
                bool val = rs[i] | newProfile.states[i];
                newProfile.states[i] = val;
            }

            *target = newProfile;

            Serial.print(F("[LOGIC] Profile updated from config: "));
            Serial.println(sid);
        }
    }

    
    // --- TUV config (schedule / dhw_enable -> DO request) ---
    // FIXED mapping: IN2 = DHW demand
    s_tuvDemandInput = (int8_t)FIX_IN_DHW_DEMAND0;
    s_tuvRequestRelay = -1;
    // FIXED mapping: R3 = přepínací ventil (0/100)
    s_tuvValveMaster0 = (int8_t)FIX_RELAY_TUV_VALVE0;
    s_tuvValveTargetPct = 0;
    s_tuvEqValveTargetPct = 0;
    s_tuvBypassEnabled = true;
    // Project requirement: DHW active -> 0%, inactive -> 100%
    s_tuvBypassPct = 0;
    s_tuvChPct = 100;
    s_tuvBypassInvert = false;
    s_tuvValveCurrentPct = 0;
    s_tuvValveMode = "ch";
    s_tuvScheduleEnabled = false;
    s_tuvDemandActive = false;
    s_tuvModeActive = false;
    s_tuvLastValveCmdMs = 0;
    s_tuvPrevModeActive = false;
    s_tuvEqValveSavedValid = false;
    s_tuvEqValveSavedPct = 0;
    s_tuvRestoreEqValveAfter = true;

    JsonObject tuv = doc["tuv"].as<JsonObject>();
    if (!tuv.isNull()) {
        int rel = tuv["relay"] | tuv["requestRelay"] | tuv["request_relay"] | 0;
        if (rel >= 1 && rel <= RELAY_COUNT) s_tuvRequestRelay = (int8_t)(rel - 1);
        if (tuv.containsKey("enabled")) s_tuvScheduleEnabled = (bool)tuv["enabled"];
        // FIXED mapping: IN2 = DHW demand (konfigurační hodnoty ignorujeme)
        s_tuvDemandInput = (int8_t)FIX_IN_DHW_DEMAND0;

        // FIXED mapping: R3 = přepínací ventil (0/100)
        int valve1 = (int)(FIX_RELAY_TUV_VALVE0 + 1);
        JsonObject bypass = tuv["bypassValve"].as<JsonObject>();
        if (!bypass.isNull()) {
            s_tuvBypassEnabled = (bool)(bypass["enabled"] | s_tuvBypassEnabled);
            s_tuvBypassPct = clampPctInt((int)(bypass["bypassPct"] | s_tuvBypassPct));
            s_tuvChPct = clampPctInt((int)(bypass["chPct"] | s_tuvChPct));
            s_tuvBypassInvert = (bool)(bypass["invert"] | s_tuvBypassInvert);
        }
        if (valve1 >= 1 && valve1 <= RELAY_COUNT) s_tuvValveMaster0 = (int8_t)(valve1 - 1);
        s_tuvValveTargetPct = clampPctInt((int)(tuv["valveTargetPct"] | tuv["targetPct"] | s_tuvValveTargetPct));
        s_tuvEqValveTargetPct = clampPctInt((int)(tuv["eqValveTargetPct"] | tuv["mixValveTargetPct"] | s_tuvEqValveTargetPct));
        if (tuv.containsKey("restoreEqValveAfter")) s_tuvRestoreEqValveAfter = (bool)tuv["restoreEqValveAfter"];
    } else {
        // legacy keys
        int rel = doc["tuvRelay"] | doc["tuv_relay"] | 0;
        if (rel >= 1 && rel <= RELAY_COUNT) s_tuvRequestRelay = (int8_t)(rel - 1);
        if (doc.containsKey("tuvEnabled")) s_tuvScheduleEnabled = (bool)doc["tuvEnabled"];

        // FIXED mapping: R3 = přepínací ventil (0/100)
        s_tuvValveMaster0 = (int8_t)FIX_RELAY_TUV_VALVE0;
        s_tuvValveTargetPct = clampPctInt((int)(doc["tuvValveTargetPct"] | doc["tuv_valve_target_pct"] | s_tuvValveTargetPct));
        s_tuvEqValveTargetPct = clampPctInt((int)(doc["tuvEqValveTargetPct"] | doc["tuv_eq_valve_target_pct"] | s_tuvEqValveTargetPct));
    }
    // NOTE: In this project R3 is a *switching* 3-way valve (binary). It does not have to be
    // configured as a "valve master" (2-relay motorized valve). Therefore we keep the relay index
    // even if it is not configured in s_valves[]. The logic below will fall back to direct relay
    // control when the valve master isn't configured.
    s_tuvDemandActive = false;
    updateInputBasedModes();
    s_tuvValveCurrentPct = effectiveTuvValvePct(s_tuvModeActive);
    updateTuvModeState(millis());
    applyTuvRequest();
    applyNightModeRelay();

    // --- Smart recirculation ---
    s_recircEnabled = false;
    s_recircMode = "on_demand";
    // FIXED mapping (R7 / IN3)
    s_recircPumpRelay = (int8_t)FIX_RELAY_RECIRC_PUMP0;
    s_recircDemandInput = (int8_t)FIX_IN_RECIRC_DEMAND0;
    s_recircOnDemandRunMs = 120000;
    s_recircMinOffMs = 300000;
    s_recircMinOnMs = 30000;
    s_recircCycleOnMs = 0;
    s_recircCycleOffMs = 0;
    s_recircStopTempC = 42.0f;
    s_recircReturnCfg = EqSourceCfg{};
    s_recircActive = false;
    s_recircStopReached = false;
    s_recircUntilMs = 0;
    s_recircLastOnMs = 0;
    s_recircLastOffMs = 0;
    s_recircPrevDemand = false;
    s_recircWindowCount = 0;

    JsonObject rec = doc["dhwRecirc"].as<JsonObject>();
    if (!rec.isNull()) {
        s_recircEnabled = (bool)(rec["enabled"] | s_recircEnabled);
        s_recircMode = String((const char*)(rec["mode"] | s_recircMode.c_str()));
        // FIXED mapping: demand input + pump relay jsou pevně dané (IN3 / R7)
        // (konfigurační hodnoty ignorujeme, aby se zařízení nerozbilo špatným nastavením).
        s_recircDemandInput = (int8_t)FIX_IN_RECIRC_DEMAND0;
        s_recircPumpRelay = (int8_t)FIX_RELAY_RECIRC_PUMP0;
        s_recircOnDemandRunMs = (uint32_t)(rec["onDemandRunMs"] | s_recircOnDemandRunMs);
        s_recircMinOffMs = (uint32_t)(rec["minOffMs"] | s_recircMinOffMs);
        s_recircMinOnMs = (uint32_t)(rec["minOnMs"] | s_recircMinOnMs);
        // Volitelné cyklování uvnitř okna: cycleOn/cycleOff (lze zadat v ms, s nebo min)
        {
            uint32_t onMs = (uint32_t)(rec["cycleOnMs"] | 0);
            if (onMs == 0) onMs = (uint32_t)(rec["cycleOnS"] | 0) * 1000UL;
            if (onMs == 0) onMs = (uint32_t)(rec["cycleOnMin"] | 0) * 60000UL;

            uint32_t offMs = (uint32_t)(rec["cycleOffMs"] | 0);
            if (offMs == 0) offMs = (uint32_t)(rec["cycleOffS"] | 0) * 1000UL;
            if (offMs == 0) offMs = (uint32_t)(rec["cycleOffMin"] | 0) * 60000UL;

            // sanity clamp: maximálně 24h (ať nedojde k overflow chování při extrémních číslech)
            const uint32_t maxCycleMs = 24UL * 60UL * 60UL * 1000UL;
            if (onMs > maxCycleMs) onMs = maxCycleMs;
            if (offMs > maxCycleMs) offMs = maxCycleMs;

            s_recircCycleOnMs = onMs;
            s_recircCycleOffMs = offMs;
        }
        s_recircStopTempC = (float)(rec["stopTempC"] | s_recircStopTempC);

        JsonObject ret = rec["tempReturnSource"].as<JsonObject>();
        if (!ret.isNull()) {
            s_recircReturnCfg = EqSourceCfg{};
            s_recircReturnCfg.source = String((const char*)(ret["source"] | s_recircReturnCfg.source.c_str()));
            s_recircReturnCfg.gpio = (int)(ret["gpio"] | s_recircReturnCfg.gpio);
            s_recircReturnCfg.romHex = String((const char*)(ret["rom"] | ret["addr"] | s_recircReturnCfg.romHex.c_str()));
            s_recircReturnCfg.topic = String((const char*)(ret["topic"] | s_recircReturnCfg.topic.c_str()));
            s_recircReturnCfg.jsonKey = String((const char*)(ret["jsonKey"] | ret["key"] | ret["field"] | s_recircReturnCfg.jsonKey.c_str()));
            s_recircReturnCfg.mqttIdx = (uint8_t)(ret["mqttIdx"] | ret["preset"] | s_recircReturnCfg.mqttIdx);
            s_recircReturnCfg.maxAgeMs = (uint32_t)(ret["maxAgeMs"] | ret["max_age_ms"] | s_recircReturnCfg.maxAgeMs);
            s_recircReturnCfg.bleId = String((const char*)(ret["bleId"] | ret["id"] | s_recircReturnCfg.bleId.c_str()));
        }

        // Fallback: if return temperature source isn't set, try to derive it from roles (TEMP1..8 / BLE / MQTT).
        if (!s_recircReturnCfg.source.length() || s_recircReturnCfg.source == "none") {
            const String wantedNorm = thermoNormalizeRole("return");

            // 1) TEMP1..8 roles
            JsonArray tempRoles = doc["tempRoles"].as<JsonArray>();
            if (!tempRoles.isNull()) {
                int idx = 0;
                for (JsonVariant v : tempRoles) {
                    if (idx >= (int)INPUT_COUNT) break;
                    const char* raw = v.as<const char*>();
                    if (thermoNormalizeRole(String(raw ? raw : "")) == wantedNorm) {
                        s_recircReturnCfg.source = String("temp") + String(idx + 1);
                        break;
                    }
                    idx++;
                }
            }

            // 2) BLE thermometer by role
            if (!s_recircReturnCfg.source.length() || s_recircReturnCfg.source == "none") {
                const BleThermometerCfg& bc = thermometersGetBle();
                const String bleRoleNorm = thermoNormalizeRole(bc.role);
                if (bleRoleNorm.length() && bleRoleNorm == wantedNorm) {
                    s_recircReturnCfg.source = "ble";
                    s_recircReturnCfg.bleId = bc.id;
                }
            }

            // 3) MQTT thermometer slots by role
            if (!s_recircReturnCfg.source.length() || s_recircReturnCfg.source == "none") {
                for (uint8_t mi = 0; mi < 2; mi++) {
                    const MqttThermometerCfg& mc = thermometersGetMqtt(mi);
                    if (!mc.topic.length()) continue;
                    const String mqttRoleNorm = thermoNormalizeRole(mc.role);
                    if (mqttRoleNorm.length() && mqttRoleNorm == wantedNorm) {
                        s_recircReturnCfg.source = "mqtt";
                        s_recircReturnCfg.mqttIdx = (uint8_t)(mi + 1);
                        break;
                    }
                }
            }
        }

        JsonArray win = rec["windows"].as<JsonArray>();
        if (!win.isNull()) {
            for (JsonVariant wv : win) {
                if (s_recircWindowCount >= MAX_RECIRC_WINDOWS) break;
                JsonObject w = wv.as<JsonObject>();
                if (w.isNull()) continue;
                RecircWindow rw;
                const char* start = w["start"] | "06:00";
                const char* end = w["end"] | "07:00";
                if (start) { rw.startHour = (uint8_t)atoi(start); const char* c = strchr(start, ':'); if (c) rw.startMin = (uint8_t)atoi(c+1); }
                if (end) { rw.endHour = (uint8_t)atoi(end); const char* c = strchr(end, ':'); if (c) rw.endMin = (uint8_t)atoi(c+1); }
                uint8_t mask = 0;
                JsonArray days = w["days"].as<JsonArray>();
                if (!days.isNull()) {
                    for (JsonVariant dv : days) {
                        int d = dv | 0;
                        if (d >= 1 && d <= 7) mask |= (uint8_t)(1U << (d - 1));
                    }
                }
                rw.daysMask = (mask == 0) ? 0x7F : mask;
                s_recircWindows[s_recircWindowCount++] = rw;
            }
        }
    }
    s_recircMode.toLowerCase();

    // --- Boiler relays ---
    JsonObject boiler = doc["boiler"].as<JsonObject>();
    if (!boiler.isNull()) {
        int dhwRel = (int)(boiler["dhwRequestRelay"] | boiler["dhw_request_relay"] | 0);
        if (dhwRel >= 1 && dhwRel <= RELAY_COUNT) s_boilerDhwRelay = (int8_t)(dhwRel - 1);
        int nmRel = (int)(boiler["nightModeRelay"] | boiler["night_mode_relay"] | 0);
        if (nmRel >= 1 && nmRel <= RELAY_COUNT) s_boilerNightRelay = (int8_t)(nmRel - 1);
    }

    // FIXED mapping (R5/R6) – konfigurace se ignoruje.
    s_boilerDhwRelay = (int8_t)FIX_RELAY_BOILER_DHW_REQ0;
    s_boilerNightRelay = (int8_t)FIX_RELAY_BOILER_NIGHT0;

    // --- AKU heater ---
    s_akuHeaterEnabled = false;
    s_akuHeaterMode = "manual";
    s_akuHeaterManualOn = false;
    s_akuHeaterTargetTopC = 50.0f;
    s_akuHeaterHysteresisC = 2.0f;
    s_akuHeaterMaxOnMs = 2UL * 60UL * 60UL * 1000UL;
    s_akuHeaterMinOffMs = 10UL * 60UL * 1000UL;
    s_akuHeaterWindowCount = 0;

    JsonObject heater = doc["akuHeater"].as<JsonObject>();
    if (!heater.isNull()) {
        s_akuHeaterEnabled = (bool)(heater["enabled"] | s_akuHeaterEnabled);
        s_akuHeaterMode = String((const char*)(heater["mode"] | s_akuHeaterMode.c_str()));
        s_akuHeaterManualOn = (bool)(heater["manualOn"] | heater["manual_on"] | s_akuHeaterManualOn);
        s_akuHeaterTargetTopC = (float)(heater["targetTopC"] | s_akuHeaterTargetTopC);
        s_akuHeaterHysteresisC = (float)(heater["hysteresisC"] | s_akuHeaterHysteresisC);
        s_akuHeaterMaxOnMs = (uint32_t)(heater["maxOnMs"] | s_akuHeaterMaxOnMs);
        s_akuHeaterMinOffMs = (uint32_t)(heater["minOffMs"] | s_akuHeaterMinOffMs);
        int relay = (int)(heater["relay"] | heater["relayIndex"] | 0);
        if (relay >= 1 && relay <= RELAY_COUNT) s_akuHeaterRelay = (int8_t)(relay - 1);

        JsonArray windows = heater["windows"].as<JsonArray>();
        if (!windows.isNull()) {
            for (JsonVariant vv : windows) {
                if (s_akuHeaterWindowCount >= MAX_HEATER_WINDOWS) break;
                JsonObject w = vv.as<JsonObject>();
                if (w.isNull()) continue;
                HeaterWindow hw{};
                const char* start = w["start"] | "06:00";
                const char* end = w["end"] | "07:00";
                int sh=6, sm=0, eh=7, em=0;
                if (start && sscanf(start, "%d:%d", &sh, &sm) >= 1) {
                    if (sh < 0) sh = 0; if (sh > 23) sh = 23;
                    if (sm < 0) sm = 0; if (sm > 59) sm = 59;
                }
                if (end && sscanf(end, "%d:%d", &eh, &em) >= 1) {
                    if (eh < 0) eh = 0; if (eh > 23) eh = 23;
                    if (em < 0) em = 0; if (em > 59) em = 59;
                }
                hw.startHour = (uint8_t)sh;
                hw.startMin = (uint8_t)sm;
                hw.endHour = (uint8_t)eh;
                hw.endMin = (uint8_t)em;
                uint8_t mask = 0;
                JsonArray days = w["days"].as<JsonArray>();
                if (!days.isNull()) {
                    for (JsonVariant dv : days) {
                        int d = dv.as<int>();
                        if (d >= 1 && d <= 7) mask |= (1U << (d - 1));
                    }
                } else {
                    mask = 0x7F;
                }
                hw.daysMask = mask ? mask : 0x7F;
                s_akuHeaterWindows[s_akuHeaterWindowCount++] = hw;
            }
        }
    }
    s_akuHeaterMode.toLowerCase();

    // FIXED mapping (R8) – konfigurace relé se ignoruje.
    s_akuHeaterRelay = (int8_t)FIX_RELAY_AKU_HEATER0;

    // --- schedules (UI stores cfg.schedules[]) ---
    s_scheduleCount = 0;
    JsonArray sched = doc["schedules"].as<JsonArray>();
    if (!sched.isNull()) {
        for (JsonVariant sv : sched) {
            if (s_scheduleCount >= MAX_SCHEDULES) break;
            JsonObject s = sv.as<JsonObject>();
            if (s.isNull()) continue;

            ScheduleItem it;
            it.enabled = (bool)(s["enabled"] | true);

            // days array: 1..7 (Mon..Sun)
            uint8_t mask = 0;
            JsonArray days = s["days"].as<JsonArray>();
            if (!days.isNull()) {
                for (JsonVariant dv : days) {
                    int d = dv | 0;
                    if (d >= 1 && d <= 7) mask |= (uint8_t)(1U << (d - 1));
                }
            }
            it.daysMask = (mask == 0) ? 0x7F : mask;

            // at "HH:MM"
            const char* at = s["at"] | s["time"] | "06:00";
            int hh = 6, mm = 0;
            if (at && strlen(at) >= 4) {
                hh = atoi(at);
                const char* c = strchr(at, ':');
                if (c) mm = atoi(c + 1);
            }
            if (hh < 0 || hh > 23) hh = 6;
            if (mm < 0 || mm > 59) mm = 0;
            it.hour = (uint8_t)hh;
            it.minute = (uint8_t)mm;

            String kind = String((const char*)(s["kind"] | s["type"] | "set_mode"));
            kind.toLowerCase();

            JsonObject val = s["value"].as<JsonObject>();
            if (val.isNull()) val = s["params"].as<JsonObject>();

            if (kind == "set_control_mode") {
                it.kind = ScheduleKind::SET_CONTROL_MODE;
                String c = String((const char*)(val["control"] | val["mode"] | "auto"));
                c.toLowerCase();
                it.controlValue = (c == "auto") ? ControlMode::AUTO : ControlMode::MANUAL;
            } else if (kind == "dhw_enable") {
                it.kind = ScheduleKind::dhw_enable;
                it.enableValue = (bool)(val["enable"] | val["enabled"] | true);
            } else if (kind == "night_mode") {
                it.kind = ScheduleKind::NIGHT_MODE;
                it.enableValue = (bool)(val["enable"] | val["enabled"] | true);
            } else { // default set_mode
                it.kind = ScheduleKind::SET_MODE;
                String m = String((const char*)(val["mode"] | "MODE1"));
                m.toUpperCase();
                if (m == "MODE1") it.modeValue = SystemMode::MODE1;
                else if (m == "MODE2") it.modeValue = SystemMode::MODE2;
                else if (m == "MODE3") it.modeValue = SystemMode::MODE3;
                else if (m == "MODE4") it.modeValue = SystemMode::MODE4;
                else if (m == "MODE5") it.modeValue = SystemMode::MODE5;
                else it.modeValue = SystemMode::MODE1;
            }

            s_schedules[s_scheduleCount++] = it;
        }
    }
// Po aplikaci konfigurace srovnáme relé podle zvoleného způsobu řízení
    if (currentControlMode == ControlMode::AUTO) {
        logicRecomputeFromInputs();
    } else {
        currentMode = manualMode;
        updateRelaysForMode(currentMode);
    }
    relayPrintStates(Serial);
}


bool logicIsTempValid(uint8_t idx){
    if (idx >= INPUT_COUNT) return false;
    return s_tempValid[idx];
}
float logicGetTempC(uint8_t idx){
    if (idx >= INPUT_COUNT) return NAN;
    return s_tempC[idx];
}

EquithermStatus logicGetEquithermStatus(){
    return s_eqStatus;
}

String logicGetEquithermReason(){
    return s_eqReason;
}

void logicSetRelayOutput(uint8_t relay1based, bool on){
    if (relay1based < 1 || relay1based > RELAY_COUNT) return;
    relayApplyLogical((uint8_t)(relay1based-1), on);
}


void logicSetRelayRaw(uint8_t relay1based, bool on){
    if (relay1based < 1 || relay1based > RELAY_COUNT) return;
    const uint8_t idx = (uint8_t)(relay1based-1);

    // pokud jde o 3c ventil (master nebo peer), ukonči případný pohyb a hlídej aby nikdy nebyly sepnuté obě cívky
    const int8_t master = isValvePeer(idx) ? s_valvePeerOf[idx] : (isValveMaster(idx) ? (int8_t)idx : -1);
    if (master >= 0 && master < (int8_t)RELAY_COUNT) {
        Valve3WayState &v = s_valves[(uint8_t)master];
        v.moving = false;
        relaySet(static_cast<RelayId>(v.relayA), false);
        relaySet(static_cast<RelayId>(v.relayB), false);
        if (on) {
            // sepnutí jedné cívky => druhá musí být OFF
            if (idx == v.relayA) relaySet(static_cast<RelayId>(v.relayB), false);
            if (idx == v.relayB) relaySet(static_cast<RelayId>(v.relayA), false);
        }
    }

    relaySet(static_cast<RelayId>(idx), on);
}

// ===== Manual / calibration control for 3-way valves (Pulse +/-) =====

static uint8_t valveExtFromInt(const Valve3WayState& /*v*/, uint8_t intPct) {
    return intPct;
}

static uint8_t valveIntFromExt(const Valve3WayState& /*v*/, uint8_t extPct) {
    return extPct;
}

bool logicValveManualConfigure(uint8_t masterRelay1based,
                               uint8_t peerRelay1based,
                               bool singleRelay,
                               bool invertDir,
                               float travelTimeS,
                               float pulseTimeS,
                               float guardTimeS,
                               float minSwitchS,
                               char defaultPos) {
    if (masterRelay1based < 1 || masterRelay1based > RELAY_COUNT) return false;
    const uint8_t master0 = (uint8_t)(masterRelay1based - 1);

    // Master nemůže být současně peer jiného ventilu
    if (isValvePeer(master0)) return false;

    uint8_t peer0 = master0;
    if (!singleRelay) {
        if (peerRelay1based < 1 || peerRelay1based > RELAY_COUNT) return false;
        peer0 = (uint8_t)(peerRelay1based - 1);
        if (peer0 == master0) return false;

        // Peer nesmí být master jiného ventilu ani peer jiného masteru
        if (isValveMaster(peer0)) return false;
        if (s_valvePeerOf[peer0] >= 0 && s_valvePeerOf[peer0] != (int8_t)master0) return false;
    }

    Valve3WayState &v = s_valves[master0];
    const bool wasConfigured = v.configured;
    const uint32_t nowMs = millis();

    uint8_t oldInt = 0;
    if (wasConfigured) {
        oldInt = valveComputePosPct(v, nowMs);
    }

    // Pokud master už měl dříve jiný peer, vyčisti mapování
    for (uint8_t r = 0; r < RELAY_COUNT; r++) {
        if (s_valvePeerOf[r] == (int8_t)master0) s_valvePeerOf[r] = -1;
    }

    v.configured = true;
    v.singleRelay = singleRelay;
    v.relayA = master0;
    v.relayB = singleRelay ? master0 : peer0;
    v.invertDir = invertDir;

    if (isfinite(travelTimeS) && travelTimeS > 0.2f && travelTimeS < 1200.0f) {
        v.travelMs = (uint32_t)(travelTimeS * 1000.0f);
    }
    if (isfinite(pulseTimeS) && pulseTimeS > 0.05f && pulseTimeS < 10.0f) {
        v.pulseMs = (uint32_t)(pulseTimeS * 1000.0f);
    }
    if (isfinite(guardTimeS) && guardTimeS >= 0.0f && guardTimeS < 10.0f) {
        v.guardMs = (uint32_t)(guardTimeS * 1000.0f);
    }
    if (isfinite(minSwitchS) && minSwitchS >= 0.0f && minSwitchS < 3600.0f) {
        v.minSwitchMs = (uint32_t)(minSwitchS * 1000.0f);
        if (v.minSwitchMs > 3600000UL) v.minSwitchMs = 3600000UL;
    }

    v.defaultB = (defaultPos == 'B' || defaultPos == 'b');
    // Zachovej odhad polohy; invertDir už pouze prohazuje cívky, neškáluje procenta.
    uint8_t initInt = 0;
    if (wasConfigured) {
        initInt = oldInt;
    } else {
        initInt = v.defaultB ? 100 : 0;
    }

    v.posPct = initInt;
    v.startPct = initInt;
    v.targetPct = initInt;
    v.pendingTargetPct = initInt;
    v.currentB = (initInt >= 50);
    v.moving = false;
    v.hasPending = false;
    v.moveStartMs = 0;
    v.moveEndMs = 0;
    v.guardEndMs = 0;

    if (!v.singleRelay) {
        s_valvePeerOf[peer0] = (int8_t)master0;
    }

    // vypni relé (bezpečné při kalibraci)
    relaySet(static_cast<RelayId>(v.relayA), false);
    relaySet(static_cast<RelayId>(v.relayB), false);
    return true;
}

static bool valvePulseInternal(uint8_t master0, int8_t dir, uint32_t nowMs) {
    if (master0 >= RELAY_COUNT) return false;
    if (!isValveMaster(master0)) return false;
    Valve3WayState &v = s_valves[master0];
    if (v.moving) return false;

    const uint8_t curInt = valveComputePosPct(v, nowMs);
    const uint8_t curExt = valveExtFromInt(v, curInt);

    float stepPctF = 1.0f;
    if (v.travelMs > 0) {
        stepPctF = ((float)v.pulseMs * 100.0f) / (float)v.travelMs;
    }
    if (!isfinite(stepPctF) || stepPctF < 1.0f) stepPctF = 1.0f;
    if (stepPctF > 100.0f) stepPctF = 100.0f;
    const int stepPct = (int)lroundf(stepPctF);

    int nextExt = (int)curExt + ((dir >= 0) ? stepPct : -stepPct);
    if (nextExt < 0) nextExt = 0;
    if (nextExt > 100) nextExt = 100;

    valveMoveToPct(master0, (uint8_t)nextExt);
    return true;
}

bool logicValvePulse(uint8_t masterRelay1based, int8_t dir) {
    if (masterRelay1based < 1 || masterRelay1based > RELAY_COUNT) return false;
    const uint8_t master0 = (uint8_t)(masterRelay1based - 1);
    const uint32_t nowMs = millis();
    return valvePulseInternal(master0, dir, nowMs);
}

bool logicValveStop(uint8_t masterRelay1based) {
    if (masterRelay1based < 1 || masterRelay1based > RELAY_COUNT) return false;
    const uint8_t master0 = (uint8_t)(masterRelay1based - 1);
    if (!isValveMaster(master0)) return false;
    Valve3WayState &v = s_valves[master0];
    const uint32_t nowMs = millis();

    // zamraz aktuální odhad a vypni relé
    const uint8_t cur = valveComputePosPct(v, nowMs);
    v.posPct = cur;
    v.startPct = cur;
    v.targetPct = cur;
    v.pendingTargetPct = cur;
    v.currentB = (cur >= 50);
    v.moving = false;
    v.hasPending = false;
    v.moveStartMs = 0;
    v.moveEndMs = 0;
    v.guardEndMs = nowMs + v.guardMs;
    v.lastCmdMs = nowMs;

    relaySet(static_cast<RelayId>(v.relayA), false);
    relaySet(static_cast<RelayId>(v.relayB), false);
    return true;
}

bool logicValveGotoPct(uint8_t masterRelay1based, uint8_t pct) {
    if (masterRelay1based < 1 || masterRelay1based > RELAY_COUNT) return false;
    const uint8_t master0 = (uint8_t)(masterRelay1based - 1);
    if (!isValveMaster(master0)) return false;
    valveMoveToPct(master0, pct);
    return true;
}


bool logicGetValveUiStatus(uint8_t relay1based, ValveUiStatus& out){
    if (relay1based < 1 || relay1based > RELAY_COUNT) return false;
    const uint8_t r0 = relay1based - 1;
    if (!isValveMaster(r0)) return false;
    const Valve3WayState &v = s_valves[r0];

    out.master = relay1based;
    out.peer   = v.singleRelay ? 0 : ((v.relayB < RELAY_COUNT) ? (uint8_t)(v.relayB + 1) : 0);
    out.posPct = v.posPct;
    out.moving = v.moving;

    out.targetPct = v.targetPct;
    out.targetB   = (v.targetPct >= 50); // legacy (pro starší UI)
    return true;
}

// MQTT: seznam topiců, které je potřeba odebírat (např. Ekviterm zdroje)
uint8_t logicGetMqttSubscribeTopics(String* outTopics, uint8_t maxTopics) {
    uint8_t n = 0;

    auto add = [&](const String& t){
        if (!t.length()) return;
        // dedup
        for (uint8_t i = 0; i < n; i++) {
            if (outTopics && outTopics[i] == t) return;
        }
        if (outTopics && n < maxTopics) outTopics[n] = t;
        n++;
    };

    // Ekviterm topicy
    if (s_eqEnabled) {
        if (s_eqOutdoorCfg.source == "mqtt") {
            if (s_eqOutdoorCfg.mqttIdx >= 1 && s_eqOutdoorCfg.mqttIdx <= 2) {
                add(thermometersGetMqtt((uint8_t)(s_eqOutdoorCfg.mqttIdx - 1)).topic);
            } else {
                add(s_eqOutdoorCfg.topic);
            }
        }
        if (s_eqFlowCfg.source == "mqtt") {
            if (s_eqFlowCfg.mqttIdx >= 1 && s_eqFlowCfg.mqttIdx <= 2) {
                add(thermometersGetMqtt((uint8_t)(s_eqFlowCfg.mqttIdx - 1)).topic);
            } else {
                add(s_eqFlowCfg.topic);
            }
        }
        if (s_eqBoilerInCfg.source == "mqtt") {
            if (s_eqBoilerInCfg.mqttIdx >= 1 && s_eqBoilerInCfg.mqttIdx <= 2) {
                add(thermometersGetMqtt((uint8_t)(s_eqBoilerInCfg.mqttIdx - 1)).topic);
            } else {
                add(s_eqBoilerInCfg.topic);
            }
        }
        if (s_eqAkuTopCfg.source == "mqtt") {
            if (s_eqAkuTopCfg.mqttIdx >= 1 && s_eqAkuTopCfg.mqttIdx <= 2) {
                add(thermometersGetMqtt((uint8_t)(s_eqAkuTopCfg.mqttIdx - 1)).topic);
            } else {
                add(s_eqAkuTopCfg.topic);
            }
        }
        if (s_eqAkuMidCfg.source == "mqtt") {
            if (s_eqAkuMidCfg.mqttIdx >= 1 && s_eqAkuMidCfg.mqttIdx <= 2) {
                add(thermometersGetMqtt((uint8_t)(s_eqAkuMidCfg.mqttIdx - 1)).topic);
            } else {
                add(s_eqAkuMidCfg.topic);
            }
        }
        if (s_eqAkuBottomCfg.source == "mqtt") {
            if (s_eqAkuBottomCfg.mqttIdx >= 1 && s_eqAkuBottomCfg.mqttIdx <= 2) {
                add(thermometersGetMqtt((uint8_t)(s_eqAkuBottomCfg.mqttIdx - 1)).topic);
            } else {
                add(s_eqAkuBottomCfg.topic);
            }
        }
    }
    if (s_recircReturnCfg.source == "mqtt") {
        if (s_recircReturnCfg.mqttIdx >= 1 && s_recircReturnCfg.mqttIdx <= 2) {
            add(thermometersGetMqtt((uint8_t)(s_recircReturnCfg.mqttIdx - 1)).topic);
        } else {
            add(s_recircReturnCfg.topic);
        }
    }

    // MQTT teploměry z konfigurace (záložka "Teploměry")
    {
        // pokud outTopics==nullptr, funkce stále správně vrací počet
        String tmp[2];
        uint8_t cnt = thermometersGetMqttSubscribeTopics(outTopics ? tmp : nullptr, 2);
        if (!outTopics) {
            n += cnt;
        } else {
            // add() řeší dedup
            for (uint8_t i = 0; i < cnt && i < 2; i++) add(tmp[i]);
        }
    }

    // Pokud outTopics==nullptr, počítáme jen počet
    if (!outTopics) return n;
    return (n > maxTopics) ? maxTopics : n;
}

// --- Diagnostics / telemetry (config apply) ---
uint32_t logicGetConfigApplyOkCount() {
    return g_logicApplyOk;
}

uint32_t logicGetConfigApplyFailCount() {
    return g_logicApplyFail;
}

uint32_t logicGetConfigApplyNoMemoryCount() {
    return g_logicApplyNoMem;
}

uint32_t logicGetConfigApplyFilterOverflowCount() {
    return g_logicApplyFilterOverflow;
}

uint32_t logicGetConfigApplyOversizeCount() {
    return g_logicApplyOversize;
}

uint32_t logicGetConfigApplyLastFailMs() {
    return g_logicApplyLastFailMs;
}

uint32_t logicGetConfigApplyLastInputLen() {
    return g_logicApplyLastInputLen;
}

const char* logicGetConfigApplyLastError() {
    return g_logicApplyLastErr;
}
