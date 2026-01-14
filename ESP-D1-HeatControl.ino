#include <Arduino.h>

#include "RelayController.h"
#include "config_pins.h"
#include "InputController.h"
#include "LogicController.h"
#include "OtaController.h"
#include "NetworkController.h"
#include "WebServerController.h"
#include "MqttController.h"
#include "BleController.h"
#include "RgbLedController.h"
#include "BuzzerController.h"
#include "DallasController.h"
#include "FsController.h"
#include "ThermometerController.h"
#include "OpenThermController.h"

String inputBuffer;

// Jednoduchá nápověda pro konzoli
void printHelp() {
    Serial.println(F("------------------------------------------"));
    Serial.println(F("Konzole prikazy:"));
    Serial.println(F("  R1 ON / R1 OFF / R1 TOGGLE   - manualni ovladani rele"));
    Serial.println(F("  R2 ON / R2 OFF / R2 TOGGLE"));
    Serial.println(F("  R3 ON / R3 OFF / R3 TOGGLE"));
    Serial.println(F("  R4 ON / R4 OFF / R4 TOGGLE"));
    Serial.println(F("  R5 ON / R5 OFF / R5 TOGGLE"));
    Serial.println(F("  R6 ON / R6 OFF / R6 TOGGLE"));
    Serial.println(F("  R7 ON / R7 OFF / R7 TOGGLE"));
    Serial.println(F("  R8 ON / R8 OFF / R8 TOGGLE"));
    Serial.println(F("  STATE   - vypise stavy vsech rele"));
    Serial.println(F("  MODE    - vypise aktualni rezim (MODE1 / MODE2 / MODE3 / MODE4 / MODE5)"));
    Serial.println(F("  HELP    - zobrazi tuto napovedu"));
    Serial.println(F("------------------------------------------"));
    Serial.println(F("WebUI: /index.html (z LittleFS)"));
    Serial.println(F("API:   GET /api/status, GET /api/relay?id=X&cmd=on/off/toggle"));
}

void processCommand(String cmd) {
    cmd.trim();
    cmd.toUpperCase();

    if (cmd.length() == 0) return;

    if (cmd == "HELP") {
        printHelp();
        return;
    }

    if (cmd == "STATE") {
        relayPrintStates(Serial);
        return;
    }

    if (cmd == "MODE") {
        SystemMode mode = logicGetMode();
        Serial.print(F("Aktualni rezim: "));
        Serial.println(logicModeToString(mode));
        Serial.print(F("Control mode: "));
        Serial.println((logicGetControlMode() == ControlMode::AUTO) ? "AUTO" : "MANUAL");
        return;
    }

    // Rele prikazy: "R1 ON" / "R2 OFF" / "R3 TOGGLE"
    if (cmd.startsWith("R")) {
        int space = cmd.indexOf(' ');
        if (space < 0) {
            Serial.println(F("Neznamy format. Pouzij napr: R1 ON"));
            return;
        }

        String rpart = cmd.substring(1, space);
        int relayNum = rpart.toInt();
        if (relayNum < 1 || relayNum > 8) {
            Serial.println(F("Neplatne rele. Pouzij 1..8"));
            return;
        }

        RelayId id = static_cast<RelayId>(relayNum - 1);

        // Bezpečné chování: ruční zásah z konzole => MANUAL
        if (logicGetControlMode() == ControlMode::AUTO) {
            logicSetControlMode(ControlMode::MANUAL);
        }

        if (cmd.endsWith("ON")) {
            relaySet(id, true);
            Serial.printf("Relay %d -> ON\n", relayNum);
        }
        else if (cmd.endsWith("OFF")) {
            relaySet(id, false);
            Serial.printf("Relay %d -> OFF\n", relayNum);
        }
        else if (cmd.endsWith("TOGGLE")) {
            relayToggle(id);
            Serial.printf("Relay %d -> TOGGLE (now %s)\n",
                          relayNum, relayGetState(id) ? "ON" : "OFF");
        }
        else {
            Serial.println(F("Neznamy prikaz pro rele. Pouzij ON/OFF/TOGGLE."));
        }

        return;
    }

    Serial.println(F("Neznamy prikaz. Zkus HELP."));
}

void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println(F("=== ESP Heat & Domestic Controller ==="));
    #ifdef FORCE_LOW_PIN
        pinMode(FORCE_LOW_PIN, OUTPUT);
        digitalWrite(FORCE_LOW_PIN, LOW);
    #endif

    // Relé + vstupy + FS
    relayInit();
    inputInit();
    fsInit();           // LittleFS
    webserverLoadConfigFromFS();

    inputSetCallback([](InputId id, bool state){
        Serial.printf("Zmena vstupu %d -> %s\n", id + 1, state ? "ACTIVE" : "INACTIVE");
        logicOnInputChanged(id, state);
    });

    logicInit();
    rgbLedInit();       // RGB LED (pokud používáš)
    thermometersInit(); // MQTT/BLE teploměry (konfigurace)
    openthermInit();    // OpenTherm (boiler) - zatím stub/placeholder
    networkInit();
    DallasController::begin();
    DallasController::configureGpio(0, TEMP_INPUT_AUTO);
    DallasController::configureGpio(1, TEMP_INPUT_AUTO);
    DallasController::configureGpio(2, TEMP_INPUT_AUTO);
    DallasController::configureGpio(3, TEMP_INPUT_AUTO);
    //dallasInit();
    webserverInit();
    mqttInit();
    bleInit();
    buzzerInit();
    OTA::init();

    printHelp();
}

void loop() {
    // konzole
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (inputBuffer.length()) {
                processCommand(inputBuffer);
                inputBuffer = "";
            }
        } else {
            inputBuffer += c;
            if (inputBuffer.length() > 120) inputBuffer = "";
        }
    }

    // vstupy (debounce + callback)
    inputUpdate();

    // senzory (před logikou)
    DallasController::loop();
    //dallasLoop();
    networkLoop();
    mqttLoop();

    // logika (AUTO/MANUAL + ventily + equitherm)
    logicUpdate();

    // OpenTherm (polling + setpoint)
    openthermLoop();

    bleLoop();
    rgbLedLoop();
    buzzerLoop();
    // web server
    webserverLoop();
    OTA::loop();
}
