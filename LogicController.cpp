#include "LogicController.h"
#include "RelayController.h"
#include "InputController.h"
#include "ConfigStore.h"
#include "RuleEngine.h"

#include <ArduinoJson.h>

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
        relaySet(static_cast<RelayId>(r), p->states[r]);
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
                relaySet(static_cast<RelayId>(r), false);
            }
            continue;
        }

        const bool inputActive = inputGetState(static_cast<InputId>(in - 1));
        const bool wantOn = (relayMap[r].polarity ? inputActive : !inputActive);
        relaySet(static_cast<RelayId>(r), wantOn);
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
        // V AUTO: buď Rule Engine, nebo legacy mapování vstupů
        if (!ruleEngineIsEnabled()) {
            logicRecomputeFromInputs();
        }
    } else {
        Serial.println(F("[LOGIC] Control mode -> MANUAL"));
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
    // rezervováno pro časové funkce (ochrany, timeouty, hystereze...)
}

void logicOnInputChanged(InputId id, bool newState) {
    (void)id;
    (void)newState;

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

    // reset relayMap + triggers (umožní "smazat" mapování v UI)
    for (uint8_t r = 0; r < RELAY_COUNT; r++) {
        relayMap[r].input = 0;
        relayMap[r].polarity = 1;
    }
    for (uint8_t i = 0; i < 5; i++) {
        modeTriggerInput[i] = 0;
    }

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

    // Po aplikaci konfigurace srovnáme relé podle zvoleného způsobu řízení
    if (currentControlMode == ControlMode::AUTO) {
        logicRecomputeFromInputs();
    } else {
        currentMode = manualMode;
        updateRelaysForMode(currentMode);
    }
    relayPrintStates(Serial);
}
