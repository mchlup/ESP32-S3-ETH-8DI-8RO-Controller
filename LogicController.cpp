#include "LogicController.h"
#include "RelayController.h"
#include "InputController.h"
#include "ConfigStore.h"
#include "RuleEngine.h"
#include "BuzzerController.h"
#include "NtcController.h"
#include "DallasController.h"
#include "MqttController.h"

#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

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
    uint8_t relayA = 0;  // 0..7 (směr A)
    uint8_t relayB = 0;  // 0..7 (směr B)
    uint32_t travelMs = 6000;   // doba přeběhu ventilu (ms)
    uint32_t pulseMs  = 800;    // krátký puls pro test/kalibraci (ms)
    uint32_t guardMs  = 300;    // pauza mezi směry (ms)
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
    if (!v.moving) return v.currentB ? 100 : 0;
    if ((int32_t)(nowMs - v.guardEndMs) < 0) return v.startPct;
    if ((int32_t)(nowMs - v.moveEndMs) >= 0) return v.targetPct;
    const float travel = (float)v.travelMs;
    if (travel <= 0.0f) return v.targetPct;
    float p = (float)(nowMs - v.guardEndMs) / travel;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    const float val = (float)v.startPct + ((float)v.targetPct - (float)v.startPct) * p;
    int iv = (int)lroundf(val);
    if (iv < 0) iv = 0;
    if (iv > 100) iv = 100;
    return (uint8_t)iv;
}

static void valveCommand(uint8_t masterA0, bool wantB){
    if (!isValveMaster(masterA0)) return;
    Valve3WayState &v = s_valves[masterA0];

    if (v.invertDir) wantB = !wantB;

    const uint32_t nowMs = millis();
    const uint8_t curPct = valveComputePosPct(v, nowMs);

    // Pokud už je v cílové poloze a nic neběží, jen pro jistotu vypni obě cívky.
    if (!v.moving && v.currentB == wantB){
        v.posPct = wantB ? 100 : 0;
        relaySet(static_cast<RelayId>(v.relayA), false);
        relaySet(static_cast<RelayId>(v.relayB), false);
        return;
    }

    // Bezpečně: vždy nejdřív vypnout obě (i při přepínání směru během pohybu)
    relaySet(static_cast<RelayId>(v.relayA), false);
    relaySet(static_cast<RelayId>(v.relayB), false);

    v.currentB = wantB;
    v.moving = true;

    v.startPct = curPct;
    v.targetPct = wantB ? 100 : 0;
    v.posPct = curPct;

    v.guardEndMs = nowMs + v.guardMs;       // krátká pauza mezi směry
    v.moveStartMs = v.guardEndMs;
    v.moveEndMs  = v.guardEndMs + v.travelMs;
}

static void valveTick(uint32_t nowMs){
    for (uint8_t i=0;i<RELAY_COUNT;i++){
        if (!s_valves[i].configured) continue;
        Valve3WayState &v = s_valves[i];
        if (!v.moving) continue;

        // průběžná pozice pro UI
        v.posPct = valveComputePosPct(v, nowMs);

        // guard: drž obě vypnuté
        if ((int32_t)(nowMs - v.guardEndMs) < 0){
            relaySet(static_cast<RelayId>(v.relayA), false);
            relaySet(static_cast<RelayId>(v.relayB), false);
            continue;
        }

        // během pohybu je sepnutá jen správná cívka
        const bool wantB = v.currentB;
        const uint8_t dir = wantB ? v.relayB : v.relayA;
        const uint8_t other = wantB ? v.relayA : v.relayB;
        relaySet(static_cast<RelayId>(other), false);
        relaySet(static_cast<RelayId>(dir), true);

        // konec pohybu
        if ((int32_t)(nowMs - v.moveEndMs) >= 0){
            relaySet(static_cast<RelayId>(v.relayA), false);
            relaySet(static_cast<RelayId>(v.relayB), false);
            v.moving = false;
            v.posPct = v.targetPct;
        }
    }
}

// Nastavení relé s respektem k šablonám (např. 3c ventil)
static void relayApplyLogical(uint8_t r0, bool on){
    if (r0 >= RELAY_COUNT) return;
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
enum class ScheduleKind : uint8_t { SET_MODE=0, SET_CONTROL_MODE=1, TUV_ENABLE=2, NIGHT_MODE=3 };

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

// TUV demand + request relay
static int8_t s_tuvDemandInput = -1;  // 0..7 or -1
static int8_t s_tuvRequestRelay = -1; // 0..7 or -1
static bool s_tuvScheduleEnabled = false;
static bool s_tuvDemandActive = false;
static bool s_nightMode = false;

// Equitherm konfigurace + stav
static bool   s_eqEnabled = false;
static String s_eqOutdoorSource = "none"; // "none" | "temp1..temp8" | "mqtt"
static String s_eqOutdoorTopic  = "";
static float  s_eqHeatTout1 = -10, s_eqHeatTflow1 = 55, s_eqHeatTout2 = 15, s_eqHeatTflow2 = 30;
static float  s_eqNightTout1 = -10, s_eqNightTflow1 = 50, s_eqNightTout2 = 15, s_eqNightTflow2 = 25;

static float  s_eqMinFlow = 22, s_eqMaxFlow = 50;
static String s_eqReason = "";

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

static bool tryGetOutdoorTempC(float &outC){
    outC = NAN;
    if (!s_eqEnabled) return false;

    if (s_eqOutdoorSource.startsWith("temp")){
        int idx = s_eqOutdoorSource.substring(4).toInt(); // temp1..8
        if (idx < 1 || idx > (int)INPUT_COUNT) return false;
        uint8_t i0 = (uint8_t)(idx - 1);
        if (!s_tempValid[i0]) return false;
        outC = s_tempC[i0];
        return isfinite(outC);
    }

    if (s_eqOutdoorSource == "mqtt"){
        if (!s_eqOutdoorTopic.length()) return false;
        String v;
        if (!mqttGetLastValue(s_eqOutdoorTopic, &v)) return false;
        v.trim();
        outC = v.toFloat();
        return isfinite(outC);
    }

    return false;
}

static void equithermRecompute(){
    s_eqStatus.enabled = s_eqEnabled;
    s_eqStatus.night = s_nightMode;
    s_eqStatus.active = false;
    s_eqStatus.outdoorC = NAN;
    s_eqStatus.targetFlowC = NAN;
    s_eqReason = "";

    if (!s_eqEnabled) { s_eqReason = "disabled"; return; }

    float tout;
    if (!tryGetOutdoorTempC(tout)) { s_eqReason = "no outdoor temp"; return; }

    const bool night = s_nightMode;
    const float x1 = night ? s_eqNightTout1 : s_eqHeatTout1;
    const float y1 = night ? s_eqNightTflow1 : s_eqHeatTflow1;
    const float x2 = night ? s_eqNightTout2 : s_eqHeatTout2;
    const float y2 = night ? s_eqNightTflow2 : s_eqHeatTflow2;

    float tflow = lerpClamped(tout, x1, y1, x2, y2);
    if (!isfinite(tflow)) { s_eqReason = "invalid curve"; return; }

    // limity (min/max)
    if (isfinite(s_eqMinFlow) && isfinite(s_eqMaxFlow)) {
        if (s_eqMinFlow > s_eqMaxFlow) { float tmp=s_eqMinFlow; s_eqMinFlow=s_eqMaxFlow; s_eqMaxFlow=tmp; }
        if (tflow < s_eqMinFlow) tflow = s_eqMinFlow;
        if (tflow > s_eqMaxFlow) tflow = s_eqMaxFlow;
    }

    s_eqStatus.active = true;
    s_eqStatus.outdoorC = tout;
    s_eqStatus.targetFlowC = tflow;
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

static void applyTuvRequest() {
    if (s_tuvRequestRelay < 0 || s_tuvRequestRelay >= (int8_t)RELAY_COUNT) return;
    bool want = (s_tuvScheduleEnabled || s_tuvDemandActive);
    relaySet(static_cast<RelayId>(s_tuvRequestRelay), want);
}


static ControlMode currentControlMode = ControlMode::MANUAL;
static SystemMode  manualMode         = SystemMode::MODE1;
static SystemMode  currentMode        = SystemMode::MODE1;

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
    return (s_tuvScheduleEnabled || s_tuvDemandActive);
}

bool logicGetNightMode() {
    return s_nightMode;
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
    if (s == "MODE1" || s == "MODE1") {
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
    // Výchozí stav po startu: MANUAL, režim neřídí vstupy
    currentControlMode = ControlMode::MANUAL;
    manualMode         = SystemMode::MODE1;
    currentMode        = manualMode;

    s_autoStatus = { false, 0, currentMode, false, false };

    updateRelaysForMode(currentMode);

    Serial.print(F("[LOGIC] Init, control=MANUAL, mode="));
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
    equithermRecompute();
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

        // recompute demand input
        if (s_tuvDemandInput >= 0 && s_tuvDemandInput < (int8_t)INPUT_COUNT) {
            s_tuvDemandActive = inputGetState(static_cast<InputId>(s_tuvDemandInput));
        } else {
            s_tuvDemandActive = false;
        }

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

                case ScheduleKind::TUV_ENABLE:
                    s_tuvScheduleEnabled = s.enableValue;
                    Serial.printf("[SCHED] TUV -> %s\n", s_tuvScheduleEnabled ? "ON" : "OFF");
                    break;

                case ScheduleKind::NIGHT_MODE:
                    s_nightMode = s.enableValue;
                    Serial.printf("[SCHED] night_mode -> %s\n", s_nightMode ? "ON" : "OFF");
                    break;
            }
        }
    } else {
        // No valid time -> still update TUV from input
        if (s_tuvDemandInput >= 0 && s_tuvDemandInput < (int8_t)INPUT_COUNT) {
            s_tuvDemandActive = inputGetState(static_cast<InputId>(s_tuvDemandInput));
        } else {
            s_tuvDemandActive = false;
        }
    }

    // Always enforce TUV request relay after mode/rules have run
    applyTuvRequest();
}

void logicOnInputChanged(InputId id, bool newState) {
    (void)newState;

    // TUV demand input (works in MANUAL/AUTO)
    if (s_tuvDemandInput >= 0 && id == static_cast<InputId>(s_tuvDemandInput)) {
        s_tuvDemandActive = inputGetState(id);
        applyTuvRequest();
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
    // config.json může být větší (popisy režimů, jména, MQTT, mapování...)
    DynamicJsonDocument doc(12288);
    DeserializationError err = deserializeJson(doc, json);
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
                JsonObject params = io["params"].as<JsonObject>();

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

                if (role == "valve_3way_2rel") {
                    if (i >= RELAY_COUNT-1) continue; // potřebuje peer (i+1)

                    Valve3WayState &v = s_valves[i];
                    v.configured = true;
                    v.relayA = i;
                    v.relayB = (uint8_t)(i+1);
                    v.travelMs = (uint32_t)((float)(params["travelTime"] | 6.0f) * 1000.0f);
                    v.pulseMs  = (uint32_t)((float)(params["pulseTime"]  | 0.8f) * 1000.0f);
                    v.guardMs  = (uint32_t)((float)(params["guardTime"]  | 0.3f) * 1000.0f);
                    v.invertDir = (bool)(params["invertDir"] | false);
                    const String defPos = String((const char*)(params["defaultPos"] | "A"));
                    v.defaultB = (defPos.equalsIgnoreCase("B"));
                    v.currentB = v.defaultB;
                    v.moving = false;
                    v.moveEndMs = 0;
                    v.guardEndMs = 0;

                    s_valvePeerOf[i+1] = (int8_t)i;

                    // jistota: peer relé vypnout
                    relaySet(static_cast<RelayId>(v.relayA), false);
                    relaySet(static_cast<RelayId>(v.relayB), false);
                }
            }
        }
    }

    // ---------------- Equitherm konfigurace ----------------
    JsonObject eq = doc["equitherm"].as<JsonObject>();
    if (!eq.isNull()) {
        s_eqEnabled = (bool)(eq["enabled"] | false);

        JsonObject outdoor = eq["outdoor"].as<JsonObject>();
        if (!outdoor.isNull()) {
            s_eqOutdoorSource = String((const char*)(outdoor["source"] | "none"));
            s_eqOutdoorTopic  = String((const char*)(outdoor["topic"] | ""));
        }

        s_eqMinFlow = (float)(eq["minFlow"] | s_eqMinFlow);
        s_eqMaxFlow = (float)(eq["maxFlow"] | s_eqMaxFlow);

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

        equithermRecompute();
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

            if (sid == "MODE1" || sid == "MODE1") {
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

    
    // --- TUV config (Nest plan -> DI demand -> DO request) ---
    s_tuvDemandInput = -1;
    s_tuvRequestRelay = -1;

    JsonObject tuv = doc["tuv"].as<JsonObject>();
    if (!tuv.isNull()) {
        int din = tuv["demandInput"] | tuv["demand_input"] | 0;
        int rel = tuv["relay"] | tuv["requestRelay"] | tuv["request_relay"] | 0;
        if (din >= 1 && din <= INPUT_COUNT) s_tuvDemandInput = (int8_t)(din - 1);
        if (rel >= 1 && rel <= RELAY_COUNT) s_tuvRequestRelay = (int8_t)(rel - 1);
        if (tuv.containsKey("enabled")) s_tuvScheduleEnabled = (bool)tuv["enabled"];
    } else {
        // legacy keys
        int din = doc["tuvDemandInput"] | doc["tuv_demand_input"] | 0;
        int rel = doc["tuvRelay"] | doc["tuv_relay"] | 0;
        if (din >= 1 && din <= INPUT_COUNT) s_tuvDemandInput = (int8_t)(din - 1);
        if (rel >= 1 && rel <= RELAY_COUNT) s_tuvRequestRelay = (int8_t)(rel - 1);
        if (doc.containsKey("tuvEnabled")) s_tuvScheduleEnabled = (bool)doc["tuvEnabled"];
    }

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
            } else if (kind == "tuv_enable") {
                it.kind = ScheduleKind::TUV_ENABLE;
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
    const int8_t peer0 = s_valvePeerOf[r0];
    out.peer = (peer0 >= 0) ? (uint8_t)(peer0 + 1) : 0;
    out.posPct = v.posPct;
    out.moving = v.moving;
    out.targetB = v.currentB;
    return true;
}
