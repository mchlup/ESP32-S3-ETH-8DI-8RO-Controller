#include "LogicController.h"
#include "RelayController.h"
#include "InputController.h"
#include "ConfigStore.h"
#include "RuleEngine.h"
#include "BuzzerController.h"
#include "NtcController.h"
#include "DallasController.h"
#include "MqttController.h"

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
                // během pohybu je sepnutá jen správná cívka
                const bool wantB = v.currentB;
                const uint8_t dir = wantB ? v.relayB : v.relayA;
                const uint8_t other = wantB ? v.relayA : v.relayB;
                relaySet(static_cast<RelayId>(other), false);
                relaySet(static_cast<RelayId>(dir), true);
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
            // jistota: nic nedržet
            relaySet(static_cast<RelayId>(v.relayA), false);
            relaySet(static_cast<RelayId>(v.relayB), false);
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
    String bleId  = "";       // do budoucna
};

static EqSourceCfg s_eqOutdoorCfg; // venkovní
static EqSourceCfg s_eqFlowCfg;    // otopná voda (feedback)

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

static bool tryGetTempFromSource(const EqSourceCfg& src, float &outC){
    outC = NAN;

    const String s = src.source;
    if (!s.length() || s == "none") return false;

    if (s == "dallas"){
        const uint8_t gpio = (uint8_t)src.gpio;
        if (gpio > 3) return false;
        // pro jistotu průběžně udržuj Dallas loop
        DallasController::loop();
        return tryGetDallasTempC(gpio, src.romHex, outC);
    }

    if (s.startsWith("temp")){
        int idx = s.substring(4).toInt(); // temp1..8
        if (idx < 1 || idx > (int)INPUT_COUNT) return false;
        const uint8_t i0 = (uint8_t)(idx - 1);
        if (!s_tempValid[i0]) return false;
        outC = s_tempC[i0];
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

        if (!topic.length()) return false;
        String payload;
        if (!mqttGetLastValue(topic, &payload)) return false;

        float tC = NAN;
        if (!tempParseFromPayload(payload, jsonKey, tC)) return false;
        outC = tC;
        return isfinite(outC);
    }

    if (s == "ble"){
        // BLE: aktuálně je k dispozici minimálně "meteo.tempC".
        // bleId může být prázdné (default) nebo např. "meteo", "meteo.tempC".
        const String id = src.bleId;
        return bleGetTempCById(id, outC);
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

static void equithermRecompute(){
    s_eqStatus = EquithermStatus{};
    s_eqStatus.enabled = s_eqEnabled;
    s_eqStatus.night   = s_nightMode;
    s_eqStatus.valveMaster = (s_eqValveMaster0 >= 0) ? (uint8_t)(s_eqValveMaster0 + 1) : 0;
    s_eqReason = "";
    s_eqStatus.reason = "";

    if (!s_eqEnabled) { s_eqReason = "disabled"; s_eqStatus.reason = s_eqReason; return; }

    // Outdoor temperature (venek)
    float tout = NAN;
    if (!tryGetTempFromSource(s_eqOutdoorCfg, tout)) {
        s_eqReason = "no outdoor temp";
        s_eqStatus.reason = s_eqReason;
        return;
    }

    // Target flow temp from curve
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

    // Flow temperature (feedback)
    float flow = NAN;
    (void)tryGetTempFromSource(s_eqFlowCfg, flow); // ok když není – jen diagnostika
    const bool hasFlow = isfinite(flow);

    s_eqStatus.active      = true;
    s_eqStatus.outdoorC    = tout;
    s_eqStatus.flowC       = flow;
    s_eqStatus.targetFlowC = target;
    s_eqStatus.lastAdjustMs = s_eqLastAdjustMs;

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

    s_eqReason = "";
    s_eqStatus.reason = ""; // OK
}

static void equithermControlTick(uint32_t nowMs){
    if (!s_eqEnabled) return;
    if (currentControlMode != ControlMode::AUTO) return;
    if (s_tuvModeActive) return;

    // musí být spočtený target
    if (!s_eqStatus.active || !isfinite(s_eqStatus.targetFlowC)) return;

    // nutný feedback senzor
    float flow = NAN;
    if (!tryGetTempFromSource(s_eqFlowCfg, flow)) return;

    // nutný ventil
    if (s_eqValveMaster0 < 0 || s_eqValveMaster0 >= (int8_t)RELAY_COUNT) return;
    const uint8_t master0 = (uint8_t)s_eqValveMaster0;
    if (!isValveMaster(master0)) return;

    // perioda korekcí
    if (s_eqLastAdjustMs != 0 && (uint32_t)(nowMs - s_eqLastAdjustMs) < s_eqPeriodMs) return;

    Valve3WayState &v = s_valves[master0];
    if (v.moving) return;

    const float target = s_eqStatus.targetFlowC;
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

static bool isTuvDemandConfigured() {
    return (s_tuvDemandInput >= 0 && s_tuvDemandInput < (int8_t)INPUT_COUNT);
}

static void updateInputBasedModes() {
    if (s_tuvEnableInput >= 0 && s_tuvEnableInput < (int8_t)INPUT_COUNT) {
        s_tuvScheduleEnabled = inputGetState(static_cast<InputId>(s_tuvEnableInput));
    }
    if (s_nightModeInput >= 0 && s_nightModeInput < (int8_t)INPUT_COUNT) {
        s_nightMode = inputGetState(static_cast<InputId>(s_nightModeInput));
        equithermRecompute();
    }
}

static bool isTuvEnabledEffective() {
    const bool demandOk = isTuvDemandConfigured() ? s_tuvDemandActive : true;
    return (s_tuvScheduleEnabled && demandOk);
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
    s_tuvModeActive = isTuvEnabledEffective();
    applyTuvModeValves(nowMs);
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
    st.valveMaster = (s_tuvValveMaster0 >= 0) ? (uint8_t)(s_tuvValveMaster0 + 1) : 0;
    st.valveTargetPct = s_tuvValveTargetPct;
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
        const bool hasNightModeInput = (s_nightModeInput >= 0 && s_nightModeInput < (int8_t)INPUT_COUNT);

        // recompute demand input
        if (s_tuvDemandInput >= 0 && s_tuvDemandInput < (int8_t)INPUT_COUNT) {
            s_tuvDemandActive = inputGetState(static_cast<InputId>(s_tuvDemandInput));
        } else {
            s_tuvDemandActive = false;
        }
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

                case ScheduleKind::TUV_ENABLE:
                    if (hasTuvEnableInput) break;
                    s_tuvScheduleEnabled = s.enableValue;
                    updateTuvModeState(nowMs);
                    Serial.printf("[SCHED] TUV -> %s\n", s_tuvScheduleEnabled ? "ON" : "OFF");
                    break;

                case ScheduleKind::NIGHT_MODE:
                    if (hasNightModeInput) break;
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
        updateTuvModeState(nowMs);
    }

    // Always enforce TUV request relay after mode/rules have run
    applyTuvRequest();
}

void logicOnInputChanged(InputId id, bool newState) {
    (void)newState;

    // TUV demand input (works in MANUAL/AUTO)
    if (s_tuvDemandInput >= 0 && id == static_cast<InputId>(s_tuvDemandInput)) {
        s_tuvDemandActive = inputGetState(id);
        updateTuvModeState(millis());
        applyTuvRequest();
    }
    if (s_tuvEnableInput >= 0 && id == static_cast<InputId>(s_tuvEnableInput)) {
        s_tuvScheduleEnabled = inputGetState(id);
        updateTuvModeState(millis());
        applyTuvRequest();
    }
    if (s_nightModeInput >= 0 && id == static_cast<InputId>(s_nightModeInput)) {
        s_nightMode = inputGetState(id);
        equithermRecompute();
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
    filter["equitherm"]["flow"]["bleId"] = true;
    filter["equitherm"]["flow"]["id"] = true;
    filter["equitherm"]["minFlow"] = true;
    filter["equitherm"]["maxFlow"] = true;
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

    DynamicJsonDocument doc(8192);
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

                if (role == "tuv_enable") {
                    s_tuvEnableInput = (int8_t)i;
                } else if (role == "night_mode") {
                    s_nightModeInput = (int8_t)i;
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

                if (role == "valve_3way_2rel") {
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

        auto parseSrc = [](JsonObject o, EqSourceCfg& dst){
            if (o.isNull()) return;
            dst.source = String((const char*)(o["source"] | dst.source.c_str()));
            dst.gpio   = (int)(o["gpio"] | dst.gpio);
            dst.romHex = String((const char*)(o["rom"] | o["addr"] | dst.romHex.c_str()));
            dst.topic  = String((const char*)(o["topic"] | dst.topic.c_str()));
            dst.jsonKey = String((const char*)(o["jsonKey"] | o["key"] | o["field"] | dst.jsonKey.c_str()));
            dst.mqttIdx = (uint8_t)(o["mqttIdx"] | o["preset"] | dst.mqttIdx);
            dst.bleId  = String((const char*)(o["bleId"] | o["id"] | dst.bleId.c_str()));
        };

        JsonObject outdoor = eq["outdoor"].as<JsonObject>();
        parseSrc(outdoor, s_eqOutdoorCfg);

        JsonObject flow = eq["flow"].as<JsonObject>();
        parseSrc(flow, s_eqFlowCfg);

        // kompatibilita se staršími konfiguracemi (jen MQTT venek)
        if (!s_eqOutdoorCfg.source.length()) s_eqOutdoorCfg.source = "none";
        if (s_eqOutdoorCfg.source == "mqtt" && !s_eqOutdoorCfg.topic.length()) {
            s_eqOutdoorCfg.topic = String((const char*)(outdoor["topic"] | ""));
        }

        s_eqMinFlow = (float)(eq["minFlow"] | s_eqMinFlow);
        s_eqMaxFlow = (float)(eq["maxFlow"] | s_eqMaxFlow);

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

        // pokud vybraný ventil není nakonfigurovaný jako 3c master, ignoruj
        if (s_eqValveMaster0 >= 0 && !isValveMaster((uint8_t)s_eqValveMaster0)) s_eqValveMaster0 = -1;

        equithermRecompute();
    } else {
        s_eqEnabled = false;
        s_eqValveMaster0 = -1;
    }
    if (s_nightModeInput >= 0 && s_nightModeInput < (int8_t)INPUT_COUNT) {
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
    s_tuvValveMaster0 = -1;
    s_tuvValveTargetPct = 0;
    s_tuvEqValveTargetPct = 0;
    s_tuvScheduleEnabled = false;
    s_tuvDemandActive = false;
    s_tuvModeActive = false;
    s_tuvLastValveCmdMs = 0;

    JsonObject tuv = doc["tuv"].as<JsonObject>();
    if (!tuv.isNull()) {
        int din = tuv["demandInput"] | tuv["demand_input"] | 0;
        int rel = tuv["relay"] | tuv["requestRelay"] | tuv["request_relay"] | 0;
        if (din >= 1 && din <= INPUT_COUNT) s_tuvDemandInput = (int8_t)(din - 1);
        if (rel >= 1 && rel <= RELAY_COUNT) s_tuvRequestRelay = (int8_t)(rel - 1);
        if (tuv.containsKey("enabled")) s_tuvScheduleEnabled = (bool)tuv["enabled"];

        int valve1 = tuv["valveMaster"] | tuv["shortValveMaster"] | 0;
        if (valve1 >= 1 && valve1 <= RELAY_COUNT) s_tuvValveMaster0 = (int8_t)(valve1 - 1);
        s_tuvValveTargetPct = clampPctInt((int)(tuv["valveTargetPct"] | tuv["targetPct"] | s_tuvValveTargetPct));
        s_tuvEqValveTargetPct = clampPctInt((int)(tuv["eqValveTargetPct"] | tuv["mixValveTargetPct"] | s_tuvEqValveTargetPct));
    } else {
        // legacy keys
        int din = doc["tuvDemandInput"] | doc["tuv_demand_input"] | 0;
        int rel = doc["tuvRelay"] | doc["tuv_relay"] | 0;
        if (din >= 1 && din <= INPUT_COUNT) s_tuvDemandInput = (int8_t)(din - 1);
        if (rel >= 1 && rel <= RELAY_COUNT) s_tuvRequestRelay = (int8_t)(rel - 1);
        if (doc.containsKey("tuvEnabled")) s_tuvScheduleEnabled = (bool)doc["tuvEnabled"];

        int valve1 = doc["tuvValveMaster"] | doc["tuv_valve_master"] | 0;
        if (valve1 >= 1 && valve1 <= RELAY_COUNT) s_tuvValveMaster0 = (int8_t)(valve1 - 1);
        s_tuvValveTargetPct = clampPctInt((int)(doc["tuvValveTargetPct"] | doc["tuv_valve_target_pct"] | s_tuvValveTargetPct));
        s_tuvEqValveTargetPct = clampPctInt((int)(doc["tuvEqValveTargetPct"] | doc["tuv_eq_valve_target_pct"] | s_tuvEqValveTargetPct));
    }
    if (s_tuvValveMaster0 >= 0 && !isValveMaster((uint8_t)s_tuvValveMaster0)) s_tuvValveMaster0 = -1;
    if (isTuvDemandConfigured()) {
        s_tuvDemandActive = inputGetState(static_cast<InputId>(s_tuvDemandInput));
    } else {
        s_tuvDemandActive = false;
    }
    updateInputBasedModes();
    updateTuvModeState(millis());
    applyTuvRequest();

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
    out.peer   = (v.relayB < RELAY_COUNT) ? (uint8_t)(v.relayB + 1) : 0;
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
