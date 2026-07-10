#include <Arduino.h>

#include "config_pins.h"
#include "Log.h"

#include "RelayController.h"
#include "InputController.h"
#include "DallasController.h"
#include "ConfigStore.h"
#include "TemperatureManager.h"

#include "Features.h"          // Network + OpenTherm + BLE
#include "NetworkController.h"
#include "OtaController.h"
#include "OpenThermController.h"
#include "BleController.h"
#include "WebPortalController.h"
#include "EquithermController.h"
#include "DhwController.h"
#include "MqttController.h"
#include "BuzzerController.h"
#include "PressureAlarmController.h"
#include "EventLog.h"
#include "HistoryBuffer.h"

// -------------------- Console --------------------
static String s_cmd;

static void printHelp() {
  Serial.println(F("------------------------------------------"));
  Serial.println(F("Minimal firmware (WiFiManager + OpenTherm + Relays/Inputs + BLE + DS18B20)"));
  Serial.println(F("Relays mapping:"));
  Serial.println(F("  R1+R2  = směšovací ventil (motor OPEN/CLOSE)"));
  Serial.println(F("  R3     = přepínací 3cestný ventil TUV/CH"));
  Serial.println(F("  R4     = cirkulační čerpadlo TUV"));
  Serial.println(F("  R5     = požadavek kotli pro TUV"));
  Serial.println(F("  R6     = den/noc ekvitermní křivka na kotli"));
  Serial.println(F("  R7     = omezovaci rele vykonu kotle"));
  Serial.println(F("  R8     = stykač topné tyče (akumulační nádrž)"));
  Serial.println();
  Serial.println(F("Inputs mapping:"));
  Serial.println(F("  IN1 = denní/noční křivka (ACTIVE = noční)"));
  Serial.println(F("  IN2 = požadavek TUV (ACTIVE)"));
  Serial.println(F("  IN3 = požadavek cirkulace (ACTIVE)"));
  Serial.println(F("  IN8 = servis: při bootu vynutí WiFiManager portal"));
  Serial.println();
  Serial.println(F("Console commands:"));
  Serial.println(F("  HELP"));
  Serial.println(F("  STATE          - relé stavy"));
  Serial.println(F("  INPUTS         - vstupy (raw+logical)"));
  Serial.println(F("  TEMP           - teploty (OpenTherm + DS18B20 role mapping)"));
  Serial.println(F("  OT             - OpenTherm status JSON"));
  Serial.println(F("  OTSCAN START   - scan Data-IDs 0..127 (supported only)"));
  Serial.println(F("  OTSCAN ALL     - scan Data-IDs 0..127 (includeAll=true)"));
  Serial.println(F("  OTSCAN STATUS  - print scan status JSON"));
  Serial.println(F("  OTSCAN STOP    - stop scan"));
  Serial.println(F("  BLE            - BLE status JSON"));
  Serial.println(F("  OTA            - OTA status JSON (Arduino IDE upload)"));
  Serial.println(F("  EQ             - Ekviterm status JSON"));
  Serial.println(F("  EQ MODE DAY/NIGHT/AUTO"));
  Serial.println(F("  R1 ON/OFF/TOGGLE ... R8 ON/OFF/TOGGLE"));
  Serial.println(F("  WIFI PORTAL    - WiFiManager portal na příští boot"));
  Serial.println(F("------------------------------------------"));
}

static void printInputs() {
  for (uint8_t i = 0; i < INPUT_COUNT; i++) {
    const InputId id = (InputId)i;
    const bool raw = inputGetRaw(id);
    const bool st = inputGetState(id);
    Serial.printf("IN%u raw=%s state=%s\n", (unsigned)(i + 1), raw ? "HIGH" : "LOW", st ? "ACTIVE" : "INACTIVE");
  }
}

static void printTankTemps() {
  const auto showRole = [](TempRole role, const char* label) {
    TempValue v = TemperatureManager::get(role, 600000);
    if (!v.valid) {
      Serial.printf("  %-12s: n/a\n", label);
      return;
    }
    const char* src = (v.src == TempSource::Dallas) ? "DS" : (v.src == TempSource::OpenTherm ? "OT" : (v.src == TempSource::Ble ? "BLE" : "?"));
    if (v.src == TempSource::Dallas && v.rom) {
      Serial.printf("  %-12s: %.2f C (%s GPIO%u ROM=%s age %lums)\n",
                    label, v.c, src, (unsigned)v.gpio, TemperatureManager::romToHex(v.rom).c_str(), (unsigned long)v.ageMs);
    } else {
      Serial.printf("  %-12s: %.2f C (%s age %lums)\n", label, v.c, src, (unsigned long)v.ageMs);
    }
  };

  Serial.println(F("[DS] role temperatures:"));
  showRole(TempRole::TankTop, "tank_top");
  showRole(TempRole::TankMid, "tank_mid");
  showRole(TempRole::TankBottom, "tank_bottom");
  showRole(TempRole::Return, "return");
  showRole(TempRole::DhwReturn, "dhw_return");
}

static void printTemps() {
  const auto showRole = [](TempRole role, const char* label) {
    TempValue v = TemperatureManager::get(role, 600000);
    if (!v.valid) {
      Serial.printf("  %-12s: n/a\n", label);
      return;
    }
    const char* src = (v.src == TempSource::Dallas) ? "DS" : (v.src == TempSource::OpenTherm ? "OT" : (v.src == TempSource::Ble ? "BLE" : "?"));
    if (v.src == TempSource::Dallas && v.rom) {
      Serial.printf("  %-12s: %.2f C (%s GPIO%u ROM=%s age %lums)\n",
                    label, v.c, src, (unsigned)v.gpio, TemperatureManager::romToHex(v.rom).c_str(), (unsigned long)v.ageMs);
    } else {
      Serial.printf("  %-12s: %.2f C (%s age %lums)\n", label, v.c, src, (unsigned long)v.ageMs);
    }
  };

  Serial.println(F("[TEMP] role temperatures:"));
  showRole(TempRole::Flow, "flow/boiler");
  showRole(TempRole::DhwTank, "dhw_tank");
  showRole(TempRole::Outside, "outside");
  showRole(TempRole::Return, "return");
  printTankTemps();
}

static void setRelayWithMixingInterlock(RelayId id, bool on) {
  // RelayController already applies the configured mixing-valve interlock.
  relaySet(id, on);
}

static void processCommand(String cmd) {
  cmd.trim();
  String up = cmd;
  up.toUpperCase();

  if (!up.length()) return;

  if (up == "HELP") { printHelp(); return; }
  if (up == "STATE") { relayPrintStates(Serial); return; }
  if (up == "INPUTS") { printInputs(); return; }
  if (up == "TEMP") { printTemps(); return; }

  if (up.startsWith("OTSCAN")) {
    if (up == "OTSCAN START") {
      const bool ok = openthermScanStart(0, 127, 60, false);
      Serial.println(ok ? F("[OT] scan started") : F("[OT] scan start failed"));
      return;
    }
    if (up == "OTSCAN ALL") {
      const bool ok = openthermScanStart(0, 127, 60, true);
      Serial.println(ok ? F("[OT] scan started (ALL)") : F("[OT] scan start failed"));
      return;
    }
    if (up == "OTSCAN STOP") {
      openthermScanStop();
      Serial.println(F("[OT] scan stopped"));
      return;
    }
    // OTSCAN STATUS (default)
    Serial.println(openthermScanGetStatusJson(true));
    return;
  }

  if (up == "OT") { Serial.println(openthermGetStatusJson()); return; }
  if (up == "BLE") { Serial.println(bleGetStatusJson()); return; }
  if (up == "OTA") { Serial.println(otaGetStatusJson()); return; }
  if (up == "EQ") { Serial.println(equithermGetStatusJson()); return; }

  if (up.startsWith("EQ MODE")) {
    String m = up.substring(String("EQ MODE").length());
    m.trim();
    m.toLowerCase();
    if (m != "day" && m != "night" && m != "auto") {
      Serial.println(F("Use: EQ MODE DAY|NIGHT|AUTO"));
      return;
    }
    String err;
    StaticJsonDocument<128> doc;
    doc["mode"] = m;
    String js; serializeJson(doc, js);
    bool ok = equithermHandleCmdJson(js, err);
    if (ok) {
      Serial.println(F("[EQ] OK"));
    } else {
      Serial.print(F("[EQ] ERR: "));
      Serial.println(err);
    }
    return;
  }

  if (up == "WIFI PORTAL") {
    networkRequestConfigPortal();
    return;
  }

  // Relay commands: R1 ON/OFF/TOGGLE
  if (up.startsWith("R")) {
    int sp = up.indexOf(' ');
    if (sp < 0) {
      Serial.println(F("Bad format. Use: R1 ON"));
      return;
    }

    int num = up.substring(1, sp).toInt();
    if (num < 1 || num > 8) {
      Serial.println(F("Relay must be 1..8"));
      return;
    }

    RelayId id = (RelayId)(num - 1);

    if (up.endsWith("ON")) {
      setRelayWithMixingInterlock(id, true);
      Serial.printf("Relay %d -> ON\n", num);
      return;
    }
    if (up.endsWith("OFF")) {
      setRelayWithMixingInterlock(id, false);
      Serial.printf("Relay %d -> OFF\n", num);
      return;
    }
    if (up.endsWith("TOGGLE")) {
      // interlock: if toggling ON, turn the other OFF first
      bool newState = !relayGetState(id);
      setRelayWithMixingInterlock(id, newState);
      Serial.printf("Relay %d -> %s\n", num, newState ? "ON" : "OFF");
      return;
    }

    Serial.println(F("Unknown relay action. Use ON/OFF/TOGGLE"));
    return;
  }

  Serial.println(F("Unknown command. Use HELP."));
}

// -------------------- Setup/Loop --------------------

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("=== ESP Heat & Domestic Controller (MINIMAL) ==="));

  relayInit();
  inputInit();

  inputSetCallback([](InputId id, bool state) {
    Serial.printf("[IN] IN%u -> %s\n", (unsigned)((uint8_t)id + 1), state ? "ACTIVE" : "INACTIVE");

    if (id == InputId::IN1) {
      // Day/night override should take effect immediately in equitherm auto mode.
      equithermRequestRecompute();
    }
  });

  // DS18B20 runtime konfiguraci hydratuje centrálně ConfigRuntime přes webPortalInit().
  // Tady pouze připravíme TemperatureManager; DallasController se nakonfiguruje až po načtení NVS.
  TemperatureManager::begin();

  // Network (WiFiManager). If IN8 is ACTIVE at boot -> portal.
  networkInit();

  // Arduino IDE OTA (network upload)
  otaInit();

  // OpenTherm
  openthermInit();

  // BLE driver
  bleInit();

  // Web portal (UI + API)
  webPortalInit();

  // Buzzer + pressure alarm + diagnostics
  buzzerInit();
  pressureAlarmInit();
  EventLog::begin();
  HistoryBuffer::begin();
  EventLog::record("system", "boot", "startup");
  buzzerPlayStartup();

  // MQTT / Home Assistant
  mqttInit();

  // Ekviterm
  equithermInit();

  // DHW / circulation
  dhwInit();

  printHelp();
}

void loop() {
  // Console
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (s_cmd.length()) {
        processCommand(s_cmd);
        s_cmd = "";
      }
    } else {
      s_cmd += c;
      if (s_cmd.length() > 160) s_cmd = "";
    }
  }

  inputUpdate();
  relayUpdate();

  DallasController::loop();

  // OpenTherm polling
  openthermLoop();

  // BLE client
  bleLoop();

  HistoryBuffer::loop();

  // Central temperature registry (keeps roles consistent across program)
  TemperatureManager::loop();

  // WiFi portal/process
  networkLoop();

  // OTA handler must run often (active when any IP interface is connected)
  otaLoop();

  // Web portal
  webPortalLoop();

  buzzerLoop();
  pressureAlarmLoop();

  // DHW / circulation priority control
  dhwLoop();

  // Ekviterm (uses temps + OT)
  equithermLoop();
}
