#include "LogicController.h"
#include "RelayController.h"
#include "InputController.h"
#include "ConfigStore.h"
#include "RuleEngine.h"
#include "BuzzerController.h"
#include "NtcController.h"
#include "DallasController.h"
#include "MqttController.h"
#include "OpenThermController.h"

#include "ThermometerController.h"
#include "TempParse.h"

#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include "BleController.h"

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
static AutoStatus s_autoStatus = { false, 0, SystemMode::MODE1, false, false };

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
    uint32_t pulseMs  = 800;    // krátký puls pro test/kalibraci (ms)
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

    // Pozn.: historicky invertDir obrací smysl A/B (nejen cívky).
    // Zachováváme kvůli kompatibilitě stávajících konfigurací.
    if (v.invertDir) wantB = !wantB;

    const uint32_t nowMs = millis();
    const uint8_t targetPct = wantB ? 100 : 0;

    // Pokud už je v cílové poloze a nic neběží, nedrž periodu.
    if (!v.moving){
        const uint8_t curPct = valveComputePosPct(v, nowMs);
        if (curPct == targetPct) {
            v.posPct = targetPct;
            v.startPct = targetPct;
            v.targetPct = targetPct;
            v.hasPending = false;
            relaySet(static_cast<RelayId>(v.relayA), false);
            relaySet(static_cast<RelayId>(v.relayB), false);
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

    // kompatibilita s invertDir: obrátíme škálu 0..100
    if (v.invertDir) targetPct = (uint8_t)(100 - targetPct);

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
        v.hasPending = false;
        relaySet(static_cast<RelayId>(v.relayA), false);
        relaySet(static_cast<RelayId>(v.relayB), false);
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
                    relaySet(static_cast<RelayId>(v.relayA), v.currentB);
                    relaySet(static_cast<RelayId>(v.relayB), false);
                } else {
                    // během pohybu je sepnutá jen správná cívka
                    const bool wantB = v.currentB;
                    const uint8_t dir = wantB ? v.relayB : v.relayA;
                    const uint8_t other = wantB ? v.relayA : v.relayB;
                    relaySet(static_cast<RelayId>(other), false);
                    relaySet(static_cast<RelayId>(dir), true);
                }
            }

            // konec pohybu
            if ((int32_t)(nowMs - v.moveEndMs) >= 0){
                relaySet(static_cast<RelayId>(v.relayA), false);
                relaySet(static_cast<RelayId>(v.relayB), false);
                v.moving = false;
                v.posPct = v.targetPct;
                v.startPct = v.targetPct;
            }
        } else {
            if (v.singleRelay) {
                const bool holdOn = (v.targetPct >= 50);
                relaySet(static_cast<RelayId>(v.relayA), holdOn);
                relaySet(static_cast<RelayId>(v.relayB), false);
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

// Smart cirkulace TUV
static bool     s_recircEnabled = false;
static String   s_recircMode = "on_demand"; // on_demand | time_windows | hybrid
static int8_t   s_recircDemandInput = -1;
static int8_t   s_recircPumpRelay = -1;
static uint32_t s_recircOnDemandRunMs = 120000;
static uint32_t s_recircMinOffMs = 300000;
static uint32_t s_recircMinOnMs = 30000;
static EqSourceCfg s_recircReturnCfg;
static float    s_recircStopTempC = 42.0f;
static bool     s_recircActive = false;
static bool     s_recircStopReached = false;
static uint32_t s_recircUntilMs = 0;
static uint32_t s_recircLastOnMs = 0;
static uint32_t s_recircLastOffMs = 0;
static bool     s_recircPrevDemand = false;
static float    s_recircReturnC = NAN;
static bool     s_recircReturnValid = false;

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
static bool   s_heatCallPrevActive = true;

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

// Equitherm konfigurace + stav
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
static uint8_t  s_eqStepPct   = 4;         // krok změny pozice (%)
static uint32_t s_eqPeriodMs  = 30000;     // minimální perioda korekcí (ms)
static uint8_t  s_eqMinPct    = 0;         // clamp pozice
static uint8_t  s_eqMaxPct    = 100;
static float    s_eqAkuMinTopC = 40.0f;
static float    s_eqAkuMinDeltaC = 3.0f;
static float    s_eqAkuMinDeltaToTargetC = 2.0f;
static float    s_eqAkuMinDeltaToBoilerInC = 3.0f;
static bool     s_eqAkuSupportEnabled = true;
static String   s_eqAkuNoSupportBehavior = "close"; // close | hold
static bool     s_eqRequireHeatCall = true;
static String   s_eqNoHeatCallBehavior = "hold"; // hold | close
static float    s_eqCurveOffsetC = 0.0f;
static float    s_eqMaxBoilerInC = 55.0f;
static bool     s_eqNoFlowDetectEnabled = true;
static uint32_t s_eqNoFlowTimeoutMs = 180000;
static float    s_eqLastFlowC = NAN;
static uint32_t s_eqLastFlowChangeMs = 0;
static bool     s_eqNoFlowActive = false;

static float    s_eqFallbackOutdoorC = 0.0f;
static uint32_t s_eqOutdoorMaxAgeMs = 900000;
static float    s_eqLastOutdoorC = NAN;
static uint32_t s_eqLastOutdoorMs = 0;

static String   s_systemProfile = "standard";
static String   s_nightModeSource = "input"; // input | schedule | manual
static bool     s_nightModeManual = false;

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
        // BLE: aktuálně je k dispozici minimálně "meteo.tempC".
        // bleId může být prázdné (default) nebo např. "meteo", "meteo.tempC".
        const String id = src.bleId;
        const bool ok = bleGetTempCById(id, outC);
        if (diag) {
            diag->valid = ok && isfinite(outC);
            if (!diag->valid) diag->reason = "ble invalid";
        }
        return ok;
    }

    if (s.startsWith("opentherm")){
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

static bool isAkuMixingAllowed(float targetFlowC, float boilerInC, String* outReason = nullptr){
    if (!s_eqAkuSupportEnabled) return true;
    if (!s_eqAkuTopCfg.source.length() || s_eqAkuTopCfg.source == "none") return true;
    float top = NAN;
    EqSourceDiag diag;
    if (!tryGetTempFromSource(s_eqAkuTopCfg, top, &diag) || !isfinite(top)) {
        if (outReason) *outReason = diag.reason.length() ? diag.reason : "aku top invalid";
        return false;
    }
    if (s_eqAkuMinTopC > 0.0f && top < s_eqAkuMinTopC) {
        if (outReason) *outReason = "aku top low";
        return false;
    }
    if (s_eqAkuMinDeltaToTargetC > 0.0f && isfinite(targetFlowC) && top < (targetFlowC + s_eqAkuMinDeltaToTargetC)) {
        if (outReason) *outReason = "aku delta target low";
        return false;
    }
    if (s_eqAkuMinDeltaToBoilerInC > 0.0f && isfinite(boilerInC) && top < (boilerInC + s_eqAkuMinDeltaToBoilerInC)) {
        if (outReason) *outReason = "aku delta boiler low";
        return false;
    }
    if (s_eqAkuMinDeltaC > 0.0f && isfinite(targetFlowC) && top < (targetFlowC + s_eqAkuMinDeltaC)) {
        if (outReason) *outReason = "aku delta low";
        return false;
    }
    return true;
}

static void equithermRecompute(){
    s_eqStatus = EquithermStatus{};
    s_eqStatus.enabled = s_eqEnabled;
    s_eqStatus.night   = s_nightMode;
    s_eqStatus.valveMaster = (s_eqValveMaster0 >= 0) ? (uint8_t)(s_eqValveMaster0 + 1) : 0;
    s_eqReason = "";
    s_eqStatus.reason = "";
    s_eqStatus.akuSupportActive = false;

    if (!s_eqEnabled) { s_eqReason = "disabled"; s_eqStatus.reason = s_eqReason; return; }

    // Outdoor temperature (venek)
    float tout = NAN;
    EqSourceDiag outDiag;
    const bool outdoorOk = tryGetTempFromSource(s_eqOutdoorCfg, tout, &outDiag);
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

    // Flow temperature (feedback)
    float flow = NAN;
    (void)tryGetTempFromSource(s_eqBoilerInCfg, flow); // ok když není – jen diagnostika
    const bool hasFlow = isfinite(flow);

    s_eqStatus.active      = true;
    s_eqStatus.outdoorC    = tout;
    s_eqStatus.flowC       = flow;
    s_eqStatus.targetFlowC = target;
    s_eqStatus.actualC = flow;
    s_eqStatus.targetC = target;
    s_eqStatus.lastAdjustMs = s_eqLastAdjustMs;

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

    if (s_eqRequireHeatCall && s_heatCallInput >= 0 && !s_heatCallActive) {
        s_eqReason = "no heat call";
        s_eqStatus.reason = s_eqReason;
        return;
    }

    if (s_eqNoFlowActive && s_eqNoFlowDetectEnabled) {
        s_eqReason = "no flow";
        s_eqStatus.reason = s_eqReason;
        return;
    }

    if (isfinite(s_eqMaxBoilerInC) && s_eqMaxBoilerInC > 0.0f && flow > s_eqMaxBoilerInC) {
        s_eqReason = "boiler in high";
        s_eqStatus.reason = s_eqReason;
        return;
    }

    String akuReason;
    if (!isAkuMixingAllowed(target, flow, &akuReason)) {
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

static void equithermControlTick(uint32_t nowMs){
    if (!s_eqEnabled) return;
    if (currentControlMode != ControlMode::AUTO) return;
    if (s_tuvModeActive) return;
    if (s_eqRequireHeatCall && s_heatCallInput >= 0 && !s_heatCallActive) {
        if (s_eqNoHeatCallBehavior == "close" && s_eqValveMaster0 >= 0) {
            valveMoveToPct((uint8_t)s_eqValveMaster0, 0);
        }
        return;
    }

    // musí být spočtený target
    if (!s_eqStatus.active || !isfinite(s_eqStatus.targetFlowC)) return;

    // nutný feedback senzor
    float flow = NAN;
    if (!tryGetTempFromSource(s_eqBoilerInCfg, flow)) return;
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
    if (s_eqNoFlowActive && s_eqNoFlowDetectEnabled) {
        return;
    }

    // nutný ventil
    if (s_eqValveMaster0 < 0 || s_eqValveMaster0 >= (int8_t)RELAY_COUNT) return;
    const uint8_t master0 = (uint8_t)s_eqValveMaster0;
    if (!isValveMaster(master0)) return;

    // perioda korekcí
    if (s_eqLastAdjustMs != 0 && (uint32_t)(nowMs - s_eqLastAdjustMs) < s_eqPeriodMs) return;

    Valve3WayState &v = s_valves[master0];
    if (v.moving) return;

    const float target = s_eqStatus.targetFlowC;
    String akuReason;
    if (!isAkuMixingAllowed(target, flow, &akuReason)) {
        if (s_eqAkuNoSupportBehavior == "close") {
            valveMoveToPct(master0, 0);
            s_eqLastAdjustMs = nowMs;
        }
        return;
    }
    if (isfinite(s_eqMaxBoilerInC) && s_eqMaxBoilerInC > 0.0f && flow > s_eqMaxBoilerInC) {
        valveMoveToPct(master0, 0);
        s_eqLastAdjustMs = nowMs;
        return;
    }
    const float err = target - flow;
    if (!isfinite(err)) return;

    if (fabsf(err) <= s_eqDeadbandC) return;

    const uint8_t curPct = valveComputePosPct(v, nowMs);
    int nextPct = (int)curPct + ((err > 0) ? (int)s_eqStepPct : -(int)s_eqStepPct);
    if (nextPct < (int)s_eqMinPct) nextPct = (int)s_eqMinPct;
    if (nextPct > (int)s_eqMaxPct) nextPct = (int)s_eqMaxPct;

    if (nextPct == (int)curPct) return;

    valveMoveToPct(master0, (uint8_t)nextPct);
    s_eqLastAdjustMs = nowMs;

    // aktualizuj status pro UI
    s_eqStatus.flowC = flow;
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
static bool isTuvDemandConfigured() {
    return false;
}

static void updateInputBasedModes() {
    if (s_tuvEnableInput >= 0 && s_tuvEnableInput < (int8_t)INPUT_COUNT) {
        s_tuvScheduleEnabled = inputGetState(static_cast<InputId>(s_tuvEnableInput));
    }
    if (s_nightModeSource == "input" && s_nightModeInput >= 0 && s_nightModeInput < (int8_t)INPUT_COUNT) {
        s_nightMode = inputGetState(static_cast<InputId>(s_nightModeInput));
        equithermRecompute();
    }
    if (s_nightModeSource == "manual") {
        s_nightMode = s_nightModeManual;
    }
    if (s_heatCallInput >= 0 && s_heatCallInput < (int8_t)INPUT_COUNT) {
        s_heatCallActive = inputGetState(static_cast<InputId>(s_heatCallInput));
    } else {
        s_heatCallActive = true;
    }
}

static bool isTuvEnabledEffective() {
    return s_tuvScheduleEnabled;
}

static void applyTuvRequest() {
    if (s_tuvRequestRelay < 0 || s_tuvRequestRelay >= (int8_t)RELAY_COUNT) return;
    relaySet(static_cast<RelayId>(s_tuvRequestRelay), isTuvEnabledEffective());
}

static uint8_t clampPctInt(int v) {
    if (v < 0) return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}

static void applyTuvModeValves(uint32_t nowMs) {
    if (!s_tuvModeActive) return;
    if (s_tuvLastValveCmdMs != 0 && (uint32_t)(nowMs - s_tuvLastValveCmdMs) < 500) return;

    bool issued = false;
    if (s_eqValveMaster0 >= 0 && s_eqValveMaster0 < (int8_t)RELAY_COUNT && isValveMaster((uint8_t)s_eqValveMaster0)) {
        valveMoveToPct((uint8_t)s_eqValveMaster0, s_tuvEqValveTargetPct);
        issued = true;
    }
    if (s_tuvValveMaster0 >= 0 && s_tuvValveMaster0 < (int8_t)RELAY_COUNT && isValveMaster((uint8_t)s_tuvValveMaster0)) {
        valveMoveToPct((uint8_t)s_tuvValveMaster0, s_tuvValveTargetPct);
        issued = true;
    }
    if (issued) s_tuvLastValveCmdMs = nowMs;
}

static void updateTuvModeState(uint32_t nowMs) {
    const bool newActive = isTuvEnabledEffective();

    // hrany: uložit/vrátit polohu směšovacího ventilu (ekviterm)
    if (newActive && !s_tuvPrevModeActive) {
        // právě začal ohřev TUV
        if (s_eqValveMaster0 >= 0 && isValveMaster((uint8_t)s_eqValveMaster0)) {
            s_tuvEqValveSavedPct = valveComputePosPct(s_valves[(uint8_t)s_eqValveMaster0], nowMs);
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
        if (s_tuvValveMaster0 >= 0 && s_tuvValveMaster0 < (int8_t)RELAY_COUNT && isValveMaster((uint8_t)s_tuvValveMaster0)) {
            valveMoveToPct((uint8_t)s_tuvValveMaster0, 0);
        }
    }

    s_tuvModeActive = newActive;
    s_tuvPrevModeActive = newActive;
    applyTuvModeValves(nowMs);
}

static void applyHeatCallGating(uint32_t nowMs) {
    (void)nowMs;
    if (s_tuvModeActive) return;
    if (s_heatCallInput < 0) return;
    if (s_tuvValveMaster0 < 0 || s_tuvValveMaster0 >= (int8_t)RELAY_COUNT) return;
    if (!isValveMaster((uint8_t)s_tuvValveMaster0)) return;

    if (!s_heatCallActive && s_heatCallPrevActive) {
        valveMoveToPct((uint8_t)s_tuvValveMaster0, 100);
        s_heatCallPrevActive = false;
    } else if (s_heatCallActive && !s_heatCallPrevActive) {
        valveMoveToPct((uint8_t)s_tuvValveMaster0, 0);
        s_heatCallPrevActive = true;
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

    bool demandEdge = false;
    if (s_recircDemandInput >= 0 && s_recircDemandInput < (int8_t)INPUT_COUNT) {
        const bool cur = inputGetState(static_cast<InputId>(s_recircDemandInput));
        demandEdge = cur && !s_recircPrevDemand;
        s_recircPrevDemand = cur;
    }

    if ((s_recircMode == "on_demand" || s_recircMode == "hybrid") && demandEdge) {
        if (s_recircLastOffMs == 0 || (uint32_t)(nowMs - s_recircLastOffMs) >= s_recircMinOffMs) {
            recircStart(nowMs, s_recircOnDemandRunMs);
        }
    }

    if (!s_recircActive && windowActive && windowRemainingMs > 0) {
        if (s_recircLastOffMs == 0 || (uint32_t)(nowMs - s_recircLastOffMs) >= s_recircMinOffMs) {
            recircStart(nowMs, windowRemainingMs);
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

        if (s_recircUntilMs != 0 && (int32_t)(nowMs - s_recircUntilMs) >= 0 && minOnOk && !windowActive) {
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
    return isTuvEnabledEffective();
}

bool logicGetNightMode() {
    return s_nightMode;
}

TuvStatus logicGetTuvStatus() {
    TuvStatus st;
    st.enabled = isTuvEnabledEffective();
    st.scheduleEnabled = s_tuvScheduleEnabled;
    st.demandActive = s_tuvDemandActive;
    st.modeActive = s_tuvModeActive;
    st.eqValveMaster = (s_eqValveMaster0 >= 0) ? (uint8_t)(s_eqValveMaster0 + 1) : 0;
    st.eqValveTargetPct = s_tuvEqValveTargetPct;
    st.eqValveSavedPct = s_tuvEqValveSavedPct;
    st.eqValveSavedValid = s_tuvEqValveSavedValid;
    st.valveMaster = (s_tuvValveMaster0 >= 0) ? (uint8_t)(s_tuvValveMaster0 + 1) : 0;
    st.valveTargetPct = s_tuvValveTargetPct;
    st.valvePosPct = 0;
    if (s_tuvValveMaster0 >= 0 && s_tuvValveMaster0 < (int8_t)RELAY_COUNT && isValveMaster((uint8_t)s_tuvValveMaster0)) {
        const uint32_t nowMs = millis();
        st.valvePosPct = valveComputePosPct(s_valves[(uint8_t)s_tuvValveMaster0], nowMs);
    }
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


bool logicGetAutoDefaultOffUnmapped() {
    return s_autoDefaultOffUnmapped;
}


// Aplikace relayMap na základě aktuálních logických stavů vstupů
static void applyRelayMapFromInputs(bool defaultOffUnmapped) {
    for (uint8_t r = 0; r < RELAY_COUNT; r++) {
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

    // Pokud jsou povolená pravidla, legacy AUTO logika se nesmí „prát“ s Rule Engine
    if (ruleEngineIsEnabled()) {
        s_autoStatus.triggered = false;
        s_autoStatus.triggerInput = 0;
        s_autoStatus.triggerMode = currentMode;
        s_autoStatus.usingRelayMap = false;
        s_autoStatus.blockedByRules = true;
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
    s_autoStatus.blockedByRules = false;

    applyRelayMapFromInputs(s_autoDefaultOffUnmapped);
    Serial.println(F("[LOGIC] AUTO: relayMap applied from inputs (no mode trigger)"));
    relayPrintStates(Serial);
}

void logicSetControlMode(ControlMode mode) {
    if (mode == currentControlMode) return;

    currentControlMode = mode;

    if (currentControlMode == ControlMode::AUTO) {
        Serial.println(F("[LOGIC] Control mode -> AUTO"));
        buzzerOnControlModeChanged(true);
        // V AUTO: buď Rule Engine, nebo legacy mapování vstupů
        if (!ruleEngineIsEnabled()) {
            logicRecomputeFromInputs();
        }
    } else {
        Serial.println(F("[LOGIC] Control mode -> MANUAL"));
        buzzerOnControlModeChanged(false);
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
    buzzerOnManualModeChanged(logicModeToString(mode));
    return true;
}

bool logicSetManualModeByName(const String& name) {
    String s = name;
    s.toUpperCase();

    // Alias: MODE1 -> MODE1 (pro kompatibilitu UI/configu)
    if (s == "MODE1" || s == "MODE2") {
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

    s_autoStatus = AutoStatus{ false, 0, currentMode, false, false };

    // V AUTO rovnou dopočítej výstupy podle vstupů (ať po bootu odpovídá skutečnému stavu).
    // Rule Engine se inicializuje až po logicInit(), takže zde poběží legacy AUTO.
    logicRecomputeFromInputs();

    Serial.print(F("[LOGIC] Init, control=AUTO, mode="));
    Serial.println(logicModeToString(currentMode));
}

void logicUpdate() {
    // časové funkce (scheduler + TUV)
    static uint32_t lastTickMs = 0;
    const uint32_t nowMs = millis();
    valveTick(nowMs);
    // Aktualizace teplot (TEMP1..TEMP8): preferuj Dallas, fallback NTC
    for (uint8_t i=0;i<INPUT_COUNT;i++){
        if (dallasIsValid(i)) { s_tempValid[i]=true; s_tempC[i]=dallasGetTempC(i); }
        else if (ntcIsValid(i)) { s_tempValid[i]=true; s_tempC[i]=ntcGetTempC(i); }
        else { s_tempValid[i]=false; /* keep last value to show on dashboard */ }
    }
    updateInputBasedModes();
    equithermRecompute();
    equithermControlTick(nowMs);
    updateTuvModeState(nowMs);
    applyHeatCallGating(nowMs);
    recircUpdate(nowMs);
    if (nowMs - lastTickMs < 250) {
        // still enforce TUV output frequently (no flicker)
        applyTuvRequest();
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

        s_tuvDemandActive = false;
        updateTuvModeState(nowMs);

        for (uint8_t i=0;i<s_scheduleCount;i++) {
            ScheduleItem &s = s_schedules[i];
            if (!s.enabled) continue;
            if ((s.daysMask & dowMask) == 0) continue;
            if (t.tm_hour != s.hour || t.tm_min != s.minute) continue;
            if (s.lastFiredMinuteKey == key) continue; // already fired this minute

            s.lastFiredMinuteKey = key;

            switch (s.kind) {
                case ScheduleKind::SET_MODE:
                    manualMode = s.modeValue;
                    if (currentControlMode == ControlMode::MANUAL) {
                        currentMode = manualMode;
                        updateRelaysForMode(currentMode);
                    }
                    Serial.printf("[SCHED] set_mode -> %s\n", logicModeToString(manualMode));
                    break;

                case ScheduleKind::SET_CONTROL_MODE:
                    currentControlMode = s.controlValue;
                    if (currentControlMode == ControlMode::AUTO) {
                        logicRecomputeFromInputs();
                    } else {
                        currentMode = manualMode;
                        updateRelaysForMode(currentMode);
                    }
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
        s_tuvDemandActive = false;
        updateTuvModeState(nowMs);
    }

    // Always enforce TUV request relay after mode/rules have run
    applyTuvRequest();
}

void logicOnInputChanged(InputId id, bool newState) {
    (void)newState;

    if (s_tuvEnableInput >= 0 && id == static_cast<InputId>(s_tuvEnableInput)) {
        s_tuvScheduleEnabled = inputGetState(id);
        updateTuvModeState(millis());
        applyTuvRequest();
    }
    if (s_nightModeSource == "input" && s_nightModeInput >= 0 && id == static_cast<InputId>(s_nightModeInput)) {
        s_nightMode = inputGetState(id);
        equithermRecompute();
    }
    if (s_heatCallInput >= 0 && id == static_cast<InputId>(s_heatCallInput)) {
        s_heatCallActive = inputGetState(id);
        applyHeatCallGating(millis());
    }

    if (currentControlMode != ControlMode::AUTO) {
        return;
    }

    // Pokud je aktivní Rule Engine, vstupy si čte sám v ruleEngineUpdate()
    if (ruleEngineIsEnabled()) {
        s_autoStatus.triggered = false;
        s_autoStatus.triggerInput = 0;
        s_autoStatus.triggerMode = currentMode;
        s_autoStatus.usingRelayMap = false;
        s_autoStatus.blockedByRules = true;
        return;
    }

    logicRecomputeFromInputs();
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
    StaticJsonDocument<4096> filter;
    filter["inputActiveLevels"] = true;
    filter["inputs"][0]["activeLevel"] = true;
    filter["inputs"][0]["active_level"] = true;
    filter["relayMap"][0]["input"] = true;
    filter["relayMap"][0]["polarity"] = true;
    filter["autoDefaultOffUnmapped"] = true;
    filter["auto_default_off_unmapped"] = true;
    filter["modes"][0]["id"] = true;
    filter["modes"][0]["relayStates"][0] = true;
    filter["modes"][0]["triggerInput"] = true;
    filter["iofunc"]["inputs"][0]["role"] = true;
    filter["iofunc"]["outputs"][0]["role"] = true;
    filter["iofunc"]["outputs"][0]["params"]["peerRel"] = true;
    filter["iofunc"]["outputs"][0]["params"]["partnerRelay"] = true;
    filter["iofunc"]["outputs"][0]["params"]["travelTime"] = true;
    filter["iofunc"]["outputs"][0]["params"]["pulseTime"] = true;
    filter["iofunc"]["outputs"][0]["params"]["guardTime"] = true;
    filter["iofunc"]["outputs"][0]["params"]["minSwitchS"] = true;
    filter["iofunc"]["outputs"][0]["params"]["invertDir"] = true;
    filter["iofunc"]["outputs"][0]["params"]["defaultPos"] = true;
    filter["equitherm"]["enabled"] = true;
    filter["equitherm"]["outdoor"]["source"] = true;
    filter["equitherm"]["outdoor"]["gpio"] = true;
    filter["equitherm"]["outdoor"]["rom"] = true;
    filter["equitherm"]["outdoor"]["addr"] = true;
    filter["equitherm"]["outdoor"]["topic"] = true;
    filter["equitherm"]["outdoor"]["jsonKey"] = true;
    filter["equitherm"]["outdoor"]["key"] = true;
    filter["equitherm"]["outdoor"]["field"] = true;
    filter["equitherm"]["outdoor"]["mqttIdx"] = true;
    filter["equitherm"]["outdoor"]["preset"] = true;
    filter["equitherm"]["outdoor"]["maxAgeMs"] = true;
    filter["equitherm"]["outdoor"]["max_age_ms"] = true;
    filter["equitherm"]["outdoor"]["bleId"] = true;
    filter["equitherm"]["outdoor"]["id"] = true;
    filter["equitherm"]["flow"]["source"] = true;
    filter["equitherm"]["flow"]["gpio"] = true;
    filter["equitherm"]["flow"]["rom"] = true;
    filter["equitherm"]["flow"]["addr"] = true;
    filter["equitherm"]["flow"]["topic"] = true;
    filter["equitherm"]["flow"]["jsonKey"] = true;
    filter["equitherm"]["flow"]["key"] = true;
    filter["equitherm"]["flow"]["field"] = true;
    filter["equitherm"]["flow"]["mqttIdx"] = true;
    filter["equitherm"]["flow"]["preset"] = true;
    filter["equitherm"]["flow"]["maxAgeMs"] = true;
    filter["equitherm"]["flow"]["max_age_ms"] = true;
    filter["equitherm"]["flow"]["bleId"] = true;
    filter["equitherm"]["flow"]["id"] = true;
    filter["equitherm"]["akuTop"]["source"] = true;
    filter["equitherm"]["akuTop"]["gpio"] = true;
    filter["equitherm"]["akuTop"]["rom"] = true;
    filter["equitherm"]["akuTop"]["addr"] = true;
    filter["equitherm"]["akuTop"]["topic"] = true;
    filter["equitherm"]["akuTop"]["jsonKey"] = true;
    filter["equitherm"]["akuTop"]["key"] = true;
    filter["equitherm"]["akuTop"]["field"] = true;
    filter["equitherm"]["akuTop"]["mqttIdx"] = true;
    filter["equitherm"]["akuTop"]["preset"] = true;
    filter["equitherm"]["akuTop"]["maxAgeMs"] = true;
    filter["equitherm"]["akuTop"]["max_age_ms"] = true;
    filter["equitherm"]["akuTop"]["bleId"] = true;
    filter["equitherm"]["akuTop"]["id"] = true;
    filter["equitherm"]["akuMid"]["source"] = true;
    filter["equitherm"]["akuMid"]["gpio"] = true;
    filter["equitherm"]["akuMid"]["rom"] = true;
    filter["equitherm"]["akuMid"]["addr"] = true;
    filter["equitherm"]["akuMid"]["topic"] = true;
    filter["equitherm"]["akuMid"]["jsonKey"] = true;
    filter["equitherm"]["akuMid"]["key"] = true;
    filter["equitherm"]["akuMid"]["field"] = true;
    filter["equitherm"]["akuMid"]["mqttIdx"] = true;
    filter["equitherm"]["akuMid"]["preset"] = true;
    filter["equitherm"]["akuMid"]["maxAgeMs"] = true;
    filter["equitherm"]["akuMid"]["max_age_ms"] = true;
    filter["equitherm"]["akuMid"]["bleId"] = true;
    filter["equitherm"]["akuMid"]["id"] = true;
    filter["equitherm"]["akuBottom"]["source"] = true;
    filter["equitherm"]["akuBottom"]["gpio"] = true;
    filter["equitherm"]["akuBottom"]["rom"] = true;
    filter["equitherm"]["akuBottom"]["addr"] = true;
    filter["equitherm"]["akuBottom"]["topic"] = true;
    filter["equitherm"]["akuBottom"]["jsonKey"] = true;
    filter["equitherm"]["akuBottom"]["key"] = true;
    filter["equitherm"]["akuBottom"]["field"] = true;
    filter["equitherm"]["akuBottom"]["mqttIdx"] = true;
    filter["equitherm"]["akuBottom"]["preset"] = true;
    filter["equitherm"]["akuBottom"]["maxAgeMs"] = true;
    filter["equitherm"]["akuBottom"]["max_age_ms"] = true;
    filter["equitherm"]["akuBottom"]["bleId"] = true;
    filter["equitherm"]["akuBottom"]["id"] = true;
    filter["equitherm"]["boilerIn"]["source"] = true;
    filter["equitherm"]["boilerIn"]["gpio"] = true;
    filter["equitherm"]["boilerIn"]["rom"] = true;
    filter["equitherm"]["boilerIn"]["addr"] = true;
    filter["equitherm"]["boilerIn"]["topic"] = true;
    filter["equitherm"]["boilerIn"]["jsonKey"] = true;
    filter["equitherm"]["boilerIn"]["key"] = true;
    filter["equitherm"]["boilerIn"]["field"] = true;
    filter["equitherm"]["boilerIn"]["mqttIdx"] = true;
    filter["equitherm"]["boilerIn"]["preset"] = true;
    filter["equitherm"]["boilerIn"]["maxAgeMs"] = true;
    filter["equitherm"]["boilerIn"]["max_age_ms"] = true;
    filter["equitherm"]["boilerIn"]["bleId"] = true;
    filter["equitherm"]["boilerIn"]["id"] = true;
    filter["equitherm"]["minFlow"] = true;
    filter["equitherm"]["maxFlow"] = true;
    filter["equitherm"]["akuMinTopC"] = true;
    filter["equitherm"]["akuMinDeltaC"] = true;
    filter["equitherm"]["akuMinDeltaToTargetC"] = true;
    filter["equitherm"]["akuMinDeltaToBoilerInC"] = true;
    filter["equitherm"]["akuSupportEnabled"] = true;
    filter["equitherm"]["akuNoSupportBehavior"] = true;
    filter["equitherm"]["requireHeatCall"] = true;
    filter["equitherm"]["noHeatCallBehavior"] = true;
    filter["equitherm"]["curveOffsetC"] = true;
    filter["equitherm"]["deadbandC"] = true;
    filter["equitherm"]["stepPct"] = true;
    filter["equitherm"]["controlPeriodMs"] = true;
    filter["equitherm"]["maxBoilerInC"] = true;
    filter["equitherm"]["noFlowDetectEnabled"] = true;
    filter["equitherm"]["noFlowTimeoutMs"] = true;
    filter["equitherm"]["slopeDay"] = true;
    filter["equitherm"]["shiftDay"] = true;
    filter["equitherm"]["slopeNight"] = true;
    filter["equitherm"]["shiftNight"] = true;
    filter["equitherm"]["refs"]["day"]["tout1"] = true;
    filter["equitherm"]["refs"]["day"]["tflow1"] = true;
    filter["equitherm"]["refs"]["day"]["tout2"] = true;
    filter["equitherm"]["refs"]["day"]["tflow2"] = true;
    filter["equitherm"]["refs"]["night"]["tout1"] = true;
    filter["equitherm"]["refs"]["night"]["tflow1"] = true;
    filter["equitherm"]["refs"]["night"]["tout2"] = true;
    filter["equitherm"]["refs"]["night"]["tflow2"] = true;
    filter["equitherm"]["valve"]["master"] = true;
    filter["equitherm"]["valveMaster"] = true;
    filter["equitherm"]["control"]["deadbandC"] = true;
    filter["equitherm"]["control"]["deadband"] = true;
    filter["equitherm"]["control"]["stepPct"] = true;
    filter["equitherm"]["control"]["step"] = true;
    filter["equitherm"]["control"]["periodMs"] = true;
    filter["equitherm"]["control"]["period"] = true;
    filter["equitherm"]["control"]["minPct"] = true;
    filter["equitherm"]["control"]["maxPct"] = true;
    filter["sensors"]["outdoor"]["maxAgeMs"] = true;
    filter["equitherm"]["fallbackOutdoorC"] = true;
    filter["system"]["profile"] = true;
    filter["system"]["nightModeSource"] = true;
    filter["system"]["nightModeManual"] = true;
    filter["dhwRecirc"]["enabled"] = true;
    filter["dhwRecirc"]["mode"] = true;
    filter["dhwRecirc"]["demandInput"] = true;
    filter["dhwRecirc"]["pumpRelay"] = true;
    filter["dhwRecirc"]["onDemandRunMs"] = true;
    filter["dhwRecirc"]["minOffMs"] = true;
    filter["dhwRecirc"]["minOnMs"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["source"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["gpio"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["rom"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["addr"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["topic"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["jsonKey"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["key"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["field"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["mqttIdx"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["preset"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["maxAgeMs"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["max_age_ms"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["bleId"] = true;
    filter["dhwRecirc"]["tempReturnSource"]["id"] = true;
    filter["dhwRecirc"]["stopTempC"] = true;
    filter["dhwRecirc"]["windows"][0]["start"] = true;
    filter["dhwRecirc"]["windows"][0]["end"] = true;
    filter["dhwRecirc"]["windows"][0]["days"][0] = true;
    filter["tuv"]["demandInput"] = true;
    filter["tuv"]["demand_input"] = true;
    filter["tuv"]["relay"] = true;
    filter["tuv"]["requestRelay"] = true;
    filter["tuv"]["request_relay"] = true;
    filter["tuv"]["enabled"] = true;
    filter["tuv"]["valveMaster"] = true;
    filter["tuv"]["shortValveMaster"] = true;
    filter["tuv"]["valveTargetPct"] = true;
    filter["tuv"]["targetPct"] = true;
    filter["tuv"]["eqValveTargetPct"] = true;
    filter["tuv"]["mixValveTargetPct"] = true;
    filter["tuvDemandInput"] = true;
    filter["tuv_demand_input"] = true;
    filter["tuvRelay"] = true;
    filter["tuv_relay"] = true;
    filter["tuvEnabled"] = true;
    filter["tuvValveMaster"] = true;
    filter["tuv_valve_master"] = true;
    filter["tuvValveTargetPct"] = true;
    filter["tuv_valve_target_pct"] = true;
    filter["tuvEqValveTargetPct"] = true;
    filter["tuv_eq_valve_target_pct"] = true;
    filter["schedules"][0]["enabled"] = true;
    filter["schedules"][0]["days"][0] = true;
    filter["schedules"][0]["at"] = true;
    filter["schedules"][0]["time"] = true;
    filter["schedules"][0]["kind"] = true;
    filter["schedules"][0]["type"] = true;
    filter["schedules"][0]["value"] = true;
    filter["schedules"][0]["params"] = true;

    DynamicJsonDocument doc(16384);
    DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
    if (err) {
        Serial.print(F("[LOGIC] Config JSON parse error: "));
        Serial.println(err.c_str());
        return;
    }

    // NTC konfiguraci řeší samostatný modul (NtcController)
    ntcApplyConfig(json);

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
        bool invertDir = false;
        uint8_t posPct = 0;
    } snap[RELAY_COUNT];

    const uint32_t nowMsSnap = millis();
    for (uint8_t i=0; i<RELAY_COUNT; i++) {
        if (s_valves[i].configured) {
            snap[i].valid = true;
            snap[i].invertDir = s_valves[i].invertDir;
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

    
    s_tuvEnableInput = -1;
    s_nightModeInput = -1;
    s_heatCallInput = -1;
    s_recircDemandInput = -1;

    // ---------------- I/O funkce (roles/templates) ----------------
    JsonObject iof = doc["iofunc"].as<JsonObject>();
    if (!iof.isNull()) {

        // Inputs: např. NTC teploměr
        JsonArray in = iof["inputs"].as<JsonArray>();
        if (!in.isNull()) {
            const uint8_t cnt = (in.size() > INPUT_COUNT) ? INPUT_COUNT : (uint8_t)in.size();
            for (uint8_t i=0;i<cnt;i++){
                JsonObject io = in[i].as<JsonObject>();
                const String role = String((const char*)(io["role"] | "none"));

                if (role == "dhw_enable") {
                    s_tuvEnableInput = (int8_t)i;
                } else if (role == "night_mode") {
                    s_nightModeInput = (int8_t)i;
                } else if (role == "thermostat" || role == "heat_call") {
                    s_heatCallInput = (int8_t)i;
                } else if (role == "recirc_demand") {
                    s_recircDemandInput = (int8_t)i;
                }
            }
        }

        // Outputs: trojcestné ventily (2 relé)
        JsonArray out = iof["outputs"].as<JsonArray>();
        if (!out.isNull()) {
            const uint8_t cnt = (out.size() > RELAY_COUNT) ? RELAY_COUNT : (uint8_t)out.size();
            for (uint8_t i=0;i<cnt;i++){
                JsonObject oo = out[i].as<JsonObject>();
                const String role = String((const char*)(oo["role"] | "none"));
                JsonObject params = oo["params"].as<JsonObject>();

                // 3c ventil master – podporujeme nové rozdělení i legacy
                if (role == "valve_3way_mix" || role == "valve_3way_2rel" || role == "valve_3way_spring") {
                    // peer relé (1-based v UI), default je i+2 = "další relé"
                    uint8_t peerRel = (uint8_t)(params["peerRel"] | 0);
                    if (peerRel == 0) {
                        // legacy config key
                        peerRel = (uint8_t)(params["partnerRelay"] | (int)(i+2));
                    }
                    const int peer0 = (int)peerRel - 1;
                    if (peer0 < 0 || peer0 >= RELAY_COUNT || peer0 == (int)i) continue;

                    // Pokud už je relé použité jako peer jiné instance, vynecháme.
                    if (s_valvePeerOf[peer0] >= 0) continue;
                    // A nechceme, aby peer byl zároveň master jiného ventilu.
                    if (s_valves[peer0].configured) continue;

                    Valve3WayState &v = s_valves[i];
                    v.configured = true;
                    v.relayA = i;
                    v.relayB = (uint8_t)peer0;
                    v.travelMs = (uint32_t)((float)(params["travelTime"] | 6.0f) * 1000.0f);
                    v.pulseMs  = (uint32_t)((float)(params["pulseTime"]  | 0.8f) * 1000.0f);
                    v.guardMs  = (uint32_t)((float)(params["guardTime"]  | 0.3f) * 1000.0f);
                    // Bezpečnostní / ochranná perioda mezi přestaveními (default 30s)
                    v.minSwitchMs = (uint32_t)((float)(params["minSwitchS"] | 30.0f) * 1000.0f);
                    if (v.minSwitchMs > 3600000UL) v.minSwitchMs = 3600000UL;
                    v.lastCmdMs = 0;
                    v.hasPending = false;
                    v.invertDir = (bool)(params["invertDir"] | false);
                    const String defPos = String((const char*)(params["defaultPos"] | "A"));
                    v.defaultB = (defPos.equalsIgnoreCase("B"));

                    // Historická kompatibilita: invertDir obrací smysl A/B
                    bool initB = v.invertDir ? !v.defaultB : v.defaultB;
                    uint8_t initPct = initB ? 100 : 0;

                    // Obnova odhadované polohy po změně konfigurace
                    if (snap[i].valid) {
                        uint8_t savedPct = snap[i].posPct;
                        if (snap[i].invertDir != v.invertDir) savedPct = (uint8_t)(100 - savedPct);
                        initPct = savedPct;
                    }

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

                    s_valvePeerOf[peer0] = (int8_t)i;

                    // jistota: peer relé vypnout
                    relaySet(static_cast<RelayId>(v.relayA), false);
                    relaySet(static_cast<RelayId>(v.relayB), false);
                } else if (role == "valve_3way_tuv" || role == "valve_3way_dhw") {
                    Valve3WayState &v = s_valves[i];
                    v.configured = true;
                    v.singleRelay = true;
                    v.relayA = i;
                    v.relayB = i;
                    v.travelMs = (uint32_t)((float)(params["travelTime"] | 6.0f) * 1000.0f);
                    v.pulseMs  = (uint32_t)((float)(params["pulseTime"]  | 0.8f) * 1000.0f);
                    v.guardMs  = (uint32_t)((float)(params["guardTime"]  | 0.3f) * 1000.0f);
                    v.minSwitchMs = (uint32_t)((float)(params["minSwitchS"] | 30.0f) * 1000.0f);
                    if (v.minSwitchMs > 3600000UL) v.minSwitchMs = 3600000UL;
                    v.lastCmdMs = 0;
                    v.hasPending = false;
                    v.invertDir = (bool)(params["invertDir"] | false);
                    const String defPos = String((const char*)(params["defaultPos"] | "A"));
                    v.defaultB = (defPos.equalsIgnoreCase("B"));

                    bool initB = v.invertDir ? !v.defaultB : v.defaultB;
                    uint8_t initPct = initB ? 100 : 0;
                    if (snap[i].valid) {
                        uint8_t savedPct = snap[i].posPct;
                        if (snap[i].invertDir != v.invertDir) savedPct = (uint8_t)(100 - savedPct);
                        initPct = savedPct;
                    }

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
                }
            }
        }
    }


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
        s_eqAkuMinTopC = (float)(eq["akuMinTopC"] | s_eqAkuMinTopC);
        s_eqAkuMinDeltaC = (float)(eq["akuMinDeltaC"] | s_eqAkuMinDeltaC);
        s_eqAkuMinDeltaToTargetC = (float)(eq["akuMinDeltaToTargetC"] | s_eqAkuMinDeltaToTargetC);
        s_eqAkuMinDeltaToBoilerInC = (float)(eq["akuMinDeltaToBoilerInC"] | s_eqAkuMinDeltaToBoilerInC);
        s_eqAkuSupportEnabled = (bool)(eq["akuSupportEnabled"] | s_eqAkuSupportEnabled);
        s_eqAkuNoSupportBehavior = String((const char*)(eq["akuNoSupportBehavior"] | s_eqAkuNoSupportBehavior.c_str()));
        s_eqRequireHeatCall = (bool)(eq["requireHeatCall"] | s_eqRequireHeatCall);
        s_eqNoHeatCallBehavior = String((const char*)(eq["noHeatCallBehavior"] | s_eqNoHeatCallBehavior.c_str()));
        s_eqCurveOffsetC = (float)(eq["curveOffsetC"] | s_eqCurveOffsetC);
        s_eqMaxBoilerInC = (float)(eq["maxBoilerInC"] | s_eqMaxBoilerInC);
        s_eqNoFlowDetectEnabled = (bool)(eq["noFlowDetectEnabled"] | s_eqNoFlowDetectEnabled);
        s_eqNoFlowTimeoutMs = (uint32_t)(eq["noFlowTimeoutMs"] | s_eqNoFlowTimeoutMs);
        s_eqFallbackOutdoorC = (float)(eq["fallbackOutdoorC"] | s_eqFallbackOutdoorC);

        s_eqAkuNoSupportBehavior.toLowerCase();
        s_eqNoHeatCallBehavior.toLowerCase();

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

        // 3c ventil (master relé 1..8)
        int master1 = 0;
        JsonObject valve = eq["valve"].as<JsonObject>();
        if (!valve.isNull()) master1 = (int)(valve["master"] | 0);
        master1 = (int)(eq["valveMaster"] | master1); // kompatibilita
        if (master1 >= 1 && master1 <= (int)RELAY_COUNT) s_eqValveMaster0 = (int8_t)(master1 - 1);
        else s_eqValveMaster0 = -1;

        // control params
        JsonObject ctrl = eq["control"].as<JsonObject>();
        if (!ctrl.isNull()) {
            s_eqDeadbandC = (float)(ctrl["deadbandC"] | ctrl["deadband"] | s_eqDeadbandC);
            s_eqStepPct   = (uint8_t)(ctrl["stepPct"] | ctrl["step"] | s_eqStepPct);
            s_eqPeriodMs  = (uint32_t)(ctrl["periodMs"] | ctrl["period"] | s_eqPeriodMs);
            s_eqMinPct    = (uint8_t)(ctrl["minPct"] | s_eqMinPct);
            s_eqMaxPct    = (uint8_t)(ctrl["maxPct"] | s_eqMaxPct);
        }
        s_eqDeadbandC = (float)(eq["deadbandC"] | s_eqDeadbandC);
        s_eqStepPct = (uint8_t)(eq["stepPct"] | s_eqStepPct);
        s_eqPeriodMs = (uint32_t)(eq["controlPeriodMs"] | s_eqPeriodMs);

        // clamp
        if (s_eqStepPct < 1) s_eqStepPct = 1;
        if (s_eqStepPct > 25) s_eqStepPct = 25;
        if (!isfinite(s_eqDeadbandC) || s_eqDeadbandC < 0.0f) s_eqDeadbandC = 0.0f;
        if (s_eqDeadbandC > 5.0f) s_eqDeadbandC = 5.0f;
        if (s_eqPeriodMs < 500) s_eqPeriodMs = 500;
        if (s_eqPeriodMs > 600000) s_eqPeriodMs = 600000;
        if (s_eqMinPct > 100) s_eqMinPct = 100;
        if (s_eqMaxPct > 100) s_eqMaxPct = 100;
        if (s_eqMinPct > s_eqMaxPct) { uint8_t t=s_eqMinPct; s_eqMinPct=s_eqMaxPct; s_eqMaxPct=t; }
        if (!isfinite(s_eqAkuMinTopC) || s_eqAkuMinTopC < 0.0f) s_eqAkuMinTopC = 0.0f;
        if (!isfinite(s_eqAkuMinDeltaC) || s_eqAkuMinDeltaC < 0.0f) s_eqAkuMinDeltaC = 0.0f;
        if (!isfinite(s_eqAkuMinDeltaToTargetC) || s_eqAkuMinDeltaToTargetC < 0.0f) s_eqAkuMinDeltaToTargetC = 0.0f;
        if (!isfinite(s_eqAkuMinDeltaToBoilerInC) || s_eqAkuMinDeltaToBoilerInC < 0.0f) s_eqAkuMinDeltaToBoilerInC = 0.0f;
        if (!isfinite(s_eqCurveOffsetC)) s_eqCurveOffsetC = 0.0f;
        if (s_eqNoFlowTimeoutMs < 10000) s_eqNoFlowTimeoutMs = 10000;
        if (s_eqNoFlowTimeoutMs > 3600000UL) s_eqNoFlowTimeoutMs = 3600000UL;
        if (s_eqOutdoorMaxAgeMs < 1000) s_eqOutdoorMaxAgeMs = 1000;
        if (s_eqOutdoorMaxAgeMs > 3600000UL) s_eqOutdoorMaxAgeMs = 3600000UL;
        s_eqNoFlowActive = false;
        s_eqLastFlowChangeMs = 0;
        s_eqLastFlowC = NAN;

        // pokud vybraný ventil není nakonfigurovaný jako 3c master, ignoruj
        if (s_eqValveMaster0 >= 0 && !isValveMaster((uint8_t)s_eqValveMaster0)) s_eqValveMaster0 = -1;

        equithermRecompute();
    } else {
        s_eqEnabled = false;
        s_eqValveMaster0 = -1;
    }
    JsonObject sys = doc["system"].as<JsonObject>();
    if (!sys.isNull()) {
        s_systemProfile = String((const char*)(sys["profile"] | s_systemProfile.c_str()));
        s_nightModeSource = String((const char*)(sys["nightModeSource"] | s_nightModeSource.c_str()));
        s_nightModeManual = (bool)(sys["nightModeManual"] | s_nightModeManual);
    }
    s_systemProfile.toLowerCase();
    s_nightModeSource.toLowerCase();

    if (s_nightModeInput >= 0 && s_nightModeInput < (int8_t)INPUT_COUNT && s_nightModeSource == "input") {
        s_nightMode = inputGetState(static_cast<InputId>(s_nightModeInput));
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

            if (sid == "MODE1" || sid == "MODE2") {
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
    s_tuvDemandInput = -1;
    s_tuvRequestRelay = -1;
    s_tuvValveMaster0 = -1;
    s_tuvValveTargetPct = 0;
    s_tuvEqValveTargetPct = 0;
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

        int valve1 = tuv["valveMaster"] | tuv["shortValveMaster"] | 0;
        if (valve1 >= 1 && valve1 <= RELAY_COUNT) s_tuvValveMaster0 = (int8_t)(valve1 - 1);
        s_tuvValveTargetPct = clampPctInt((int)(tuv["valveTargetPct"] | tuv["targetPct"] | s_tuvValveTargetPct));
        s_tuvEqValveTargetPct = clampPctInt((int)(tuv["eqValveTargetPct"] | tuv["mixValveTargetPct"] | s_tuvEqValveTargetPct));
        if (tuv.containsKey("restoreEqValveAfter")) s_tuvRestoreEqValveAfter = (bool)tuv["restoreEqValveAfter"];
    } else {
        // legacy keys
        int rel = doc["tuvRelay"] | doc["tuv_relay"] | 0;
        if (rel >= 1 && rel <= RELAY_COUNT) s_tuvRequestRelay = (int8_t)(rel - 1);
        if (doc.containsKey("tuvEnabled")) s_tuvScheduleEnabled = (bool)doc["tuvEnabled"];

        int valve1 = doc["tuvValveMaster"] | doc["tuv_valve_master"] | 0;
        if (valve1 >= 1 && valve1 <= RELAY_COUNT) s_tuvValveMaster0 = (int8_t)(valve1 - 1);
        s_tuvValveTargetPct = clampPctInt((int)(doc["tuvValveTargetPct"] | doc["tuv_valve_target_pct"] | s_tuvValveTargetPct));
        s_tuvEqValveTargetPct = clampPctInt((int)(doc["tuvEqValveTargetPct"] | doc["tuv_eq_valve_target_pct"] | s_tuvEqValveTargetPct));
    }
    if (s_tuvValveMaster0 >= 0 && !isValveMaster((uint8_t)s_tuvValveMaster0)) s_tuvValveMaster0 = -1;
    s_tuvDemandActive = false;
    updateInputBasedModes();
    updateTuvModeState(millis());
    applyTuvRequest();
    applyHeatCallGating(millis());

    // --- Smart recirculation ---
    s_recircEnabled = false;
    s_recircMode = "on_demand";
    s_recircPumpRelay = -1;
    s_recircOnDemandRunMs = 120000;
    s_recircMinOffMs = 300000;
    s_recircMinOnMs = 30000;
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
        int demandInput = rec["demandInput"] | 0;
        if (demandInput >= 1 && demandInput <= INPUT_COUNT) s_recircDemandInput = (int8_t)(demandInput - 1);
        int pumpRelay = rec["pumpRelay"] | 0;
        if (pumpRelay >= 1 && pumpRelay <= RELAY_COUNT) s_recircPumpRelay = (int8_t)(pumpRelay - 1);
        s_recircOnDemandRunMs = (uint32_t)(rec["onDemandRunMs"] | s_recircOnDemandRunMs);
        s_recircMinOffMs = (uint32_t)(rec["minOffMs"] | s_recircMinOffMs);
        s_recircMinOnMs = (uint32_t)(rec["minOnMs"] | s_recircMinOnMs);
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
