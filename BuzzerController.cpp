#include "BuzzerController.h"
#include "config_pins.h"
#include <LittleFS.h>
#include "FsController.h"
#include <ArduinoJson.h>

namespace {
  struct Step { bool on; uint16_t ms; };

  // Patterny (jednoduché ON/OFF, funguje pro aktivní i pasivní buzzer)
  const Step P_OFF[]    = { {false, 0} };
  const Step P_SHORT[]  = { {true,  80}, {false, 120} };
  const Step P_LONG[]   = { {true,  350}, {false, 200} };
  const Step P_DOUBLE[] = { {true,  80}, {false, 120}, {true, 80}, {false, 200} };
  const Step P_TRIPLE[] = { {true,  80}, {false, 120}, {true, 80}, {false, 120}, {true, 80}, {false, 250} };
  const Step P_ERROR[]  = { {true,  120}, {false, 120}, {true, 120}, {false, 120}, {true, 200}, {false, 300} };

  struct Cfg {
    bool enabled = true;
    bool activeHigh = true; // true: HIGH=ON, false: LOW=ON
    // Pokud je piezo pasivní, potřebuje PWM (jinak jen "cvakne").
    bool usePwm = true;
    uint16_t pwmFreqHz = 3000;   // 2–4 kHz obvykle funguje dobře
    uint8_t pwmDutyPct = 50;     // 10–80 %, podle hlasitosti

    // mapování událostí -> pattern name
    char ev_control_auto[12]   = "short";
    char ev_control_manual[12] = "short";
    char ev_manual_mode[12]    = "short";
    char ev_relay_on[12]       = "off";
    char ev_relay_off[12]      = "off";
    char ev_error[12]          = "error";
  } g_cfg;

  const Step* g_steps = nullptr;
  uint8_t g_stepCount = 0;
  uint8_t g_stepIndex = 0;
  uint32_t g_stepUntilMs = 0;
  bool g_running = false;

  const char* CFG_PATH = "/buzzer.json";

  // --- PWM helpers (LEDC) ---
  // ESP32 Arduino core 3.x: LEDC API je pin-centric (ledcAttach/ledcWrite/ledcWriteTone/ledcDetach).
  // U některých verzí existuje i channel-based API. Použijeme pin-centric, je nejméně bolestivé.
  bool g_pwmAttached = false;

  void pwmAttachIfNeeded() {
    if (!g_cfg.usePwm) return;
    if (g_pwmAttached) return;

#if defined(ARDUINO_ARCH_ESP32)
    // Attach PWM to pin (freq/resolution)
    // resolution 8 bit (0..255) je dost
    ledcAttach(BUZZER_PIN, g_cfg.pwmFreqHz, 8);
    g_pwmAttached = true;
#endif
  }

  void pwmDetachIfNeeded() {
    if (!g_pwmAttached) return;
#if defined(ARDUINO_ARCH_ESP32)
    ledcDetach(BUZZER_PIN);
#endif
    g_pwmAttached = false;
  }

  void pwmOn() {
#if defined(ARDUINO_ARCH_ESP32)
    pwmAttachIfNeeded();
    // nastav duty (0..255)
    const uint8_t duty = (uint8_t)((uint16_t)g_cfg.pwmDutyPct * 255u / 100u);
    ledcWrite(BUZZER_PIN, duty);
    // pro jistotu nastav i frekvenci (když se změnila v configu)
    ledcWriteTone(BUZZER_PIN, g_cfg.pwmFreqHz);
#endif
  }

  void pwmOff() {
#if defined(ARDUINO_ARCH_ESP32)
    if (!g_pwmAttached) return;
    ledcWrite(BUZZER_PIN, 0);
    ledcWriteTone(BUZZER_PIN, 0);
#endif
  }

  void dcOff() {
    digitalWrite(BUZZER_PIN, g_cfg.activeHigh ? LOW : HIGH);
  }

  void dcOn() {
    // DC „zapnuto“ – funguje jen pro aktivní buzzer (pasivní jen cvakne)
    digitalWrite(BUZZER_PIN, g_cfg.activeHigh ? HIGH : LOW);
  }

  void pinWriteOn(bool on) {
    if (!g_cfg.enabled) {
      // při zakázání držet OFF
      if (g_cfg.usePwm) pwmOff();
      else dcOff();
      return;
    }
    if (g_cfg.usePwm) {
      if (on) pwmOn();
      else pwmOff();
    } else {
      // DC režim (aktivní buzzer)
      if (on) dcOn();
      else dcOff();
    }
  }

  void play(const Step* steps, uint8_t count) {
    if (!steps || !count) return;
    g_steps = steps;
    g_stepCount = count;
    g_stepIndex = 0;
    g_running = true;
    g_stepUntilMs = 0; // spustit hned v loop()
  }

  const Step* patternByName(const String& name, uint8_t& outCount) {
    String s = name; s.toLowerCase();
    if (s == "off")    { outCount = 1; return P_OFF; }
    if (s == "short")  { outCount = (uint8_t)(sizeof(P_SHORT)/sizeof(P_SHORT[0])); return P_SHORT; }
    if (s == "long")   { outCount = (uint8_t)(sizeof(P_LONG)/sizeof(P_LONG[0])); return P_LONG; }
    if (s == "double") { outCount = (uint8_t)(sizeof(P_DOUBLE)/sizeof(P_DOUBLE[0])); return P_DOUBLE; }
    if (s == "triple") { outCount = (uint8_t)(sizeof(P_TRIPLE)/sizeof(P_TRIPLE[0])); return P_TRIPLE; }
    if (s == "error")  { outCount = (uint8_t)(sizeof(P_ERROR)/sizeof(P_ERROR[0])); return P_ERROR; }
    outCount = (uint8_t)(sizeof(P_SHORT)/sizeof(P_SHORT[0])); return P_SHORT;
  }

  void playEvent(const char* patternName) {
    if (!patternName || !patternName[0]) return;
    uint8_t n = 0;
    const Step* p = patternByName(String(patternName), n);
    if (p == P_OFF) return;
    play(p, n);
  }
}

void buzzerInit() {
  // default: vypnuto
  pinMode(BUZZER_PIN, OUTPUT);
  dcOff();
  buzzerLoadFromFS();

  // po načtení configu nastav výchozí stav korektně
  if (g_cfg.usePwm) {
    pwmAttachIfNeeded();
    pwmOff();
  } else {
    pwmDetachIfNeeded();
    dcOff();
  }
}

void buzzerLoop() {
  if (!g_running) return;
  const uint32_t now = millis();

  if (g_stepUntilMs == 0 || (int32_t)(now - g_stepUntilMs) >= 0) {
    if (!g_steps || g_stepIndex >= g_stepCount) {
      g_running = false;
      pinWriteOn(false);
      return;
    }

    const Step st = g_steps[g_stepIndex++];
    pinWriteOn(st.on);
    if (st.ms == 0) {
      // "off" pattern
      g_running = false;
      pinWriteOn(false);
      return;
    }
    g_stepUntilMs = now + st.ms;
  }
}

void buzzerPlayPatternByName(const String& name) {
  if (!g_cfg.enabled) return;
  uint8_t n = 0;
  const Step* p = patternByName(name, n);
  if (p == P_OFF) { buzzerStop(); return; }
  play(p, n);
}

void buzzerStop() {
  g_running = false;
  g_steps = nullptr;
  g_stepCount = 0;
  g_stepIndex = 0;
  g_stepUntilMs = 0;
  pinWriteOn(false); // zajistí i vypnutí PWM
}

void buzzerOnControlModeChanged(bool isAuto) {
  playEvent(isAuto ? g_cfg.ev_control_auto : g_cfg.ev_control_manual);
}

void buzzerOnManualModeChanged(const String& /*modeName*/) {
  playEvent(g_cfg.ev_manual_mode);
}

void buzzerOnRelayChanged(uint8_t /*relay*/, bool on) {
  playEvent(on ? g_cfg.ev_relay_on : g_cfg.ev_relay_off);
}

void buzzerOnError(const String& /*code*/) {
  playEvent(g_cfg.ev_error);
}

void buzzerToJson(String& outJson) {
  StaticJsonDocument<512> doc;
  doc["enabled"] = g_cfg.enabled;
  doc["activeHigh"] = g_cfg.activeHigh;
  doc["usePwm"] = g_cfg.usePwm;
  doc["pwmFreqHz"] = g_cfg.pwmFreqHz;
  doc["pwmDutyPct"] = g_cfg.pwmDutyPct;
  JsonObject ev = doc.createNestedObject("events");
  ev["control_auto"] = g_cfg.ev_control_auto;
  ev["control_manual"] = g_cfg.ev_control_manual;
  ev["manual_mode"] = g_cfg.ev_manual_mode;
  ev["relay_on"] = g_cfg.ev_relay_on;
  ev["relay_off"] = g_cfg.ev_relay_off;
  ev["error"] = g_cfg.ev_error;
  serializeJson(doc, outJson);
}

void buzzerUpdateFromJson(const JsonObject& cfg) {
  if (cfg.containsKey("enabled")) g_cfg.enabled = cfg["enabled"].as<bool>();
  if (cfg.containsKey("activeHigh")) g_cfg.activeHigh = cfg["activeHigh"].as<bool>();
  if (cfg.containsKey("usePwm")) g_cfg.usePwm = cfg["usePwm"].as<bool>();
  if (cfg.containsKey("pwmFreqHz")) g_cfg.pwmFreqHz = (uint16_t)cfg["pwmFreqHz"].as<uint16_t>();
  if (cfg.containsKey("pwmDutyPct")) g_cfg.pwmDutyPct = (uint8_t)cfg["pwmDutyPct"].as<uint8_t>();
  if (g_cfg.pwmDutyPct > 100) g_cfg.pwmDutyPct = 100;
  if (g_cfg.pwmFreqHz < 100) g_cfg.pwmFreqHz = 100;

  JsonObject ev = cfg["events"].as<JsonObject>();
  if (!ev.isNull()) {
    auto cp = [&](const char* k, char* dst, size_t n) {
      if (!ev.containsKey(k)) return;
      const char* v = ev[k] | "";
      if (!v) return;
      strlcpy(dst, v, n);
    };
    cp("control_auto",   g_cfg.ev_control_auto,   sizeof(g_cfg.ev_control_auto));
    cp("control_manual", g_cfg.ev_control_manual, sizeof(g_cfg.ev_control_manual));
    cp("manual_mode",    g_cfg.ev_manual_mode,    sizeof(g_cfg.ev_manual_mode));
    cp("relay_on",       g_cfg.ev_relay_on,       sizeof(g_cfg.ev_relay_on));
    cp("relay_off",      g_cfg.ev_relay_off,      sizeof(g_cfg.ev_relay_off));
    cp("error",          g_cfg.ev_error,          sizeof(g_cfg.ev_error));
  }

  // při změně režimu/polarity ihned aplikuj OFF stav správně
  if (g_cfg.usePwm) {
    pwmAttachIfNeeded();
    pwmOff();
  } else {
    pwmDetachIfNeeded();
    dcOff();
  }
}

bool buzzerSaveToFS() {
  if (!fsIsReady()) return false;
  File f = LittleFS.open(CFG_PATH, "w");
  if (!f) return false;
  StaticJsonDocument<512> doc;
  doc["enabled"] = g_cfg.enabled;
  doc["activeHigh"] = g_cfg.activeHigh;
  doc["usePwm"] = g_cfg.usePwm;
  doc["pwmFreqHz"] = g_cfg.pwmFreqHz;
  doc["pwmDutyPct"] = g_cfg.pwmDutyPct;
  JsonObject ev = doc.createNestedObject("events");
  ev["control_auto"] = g_cfg.ev_control_auto;
  ev["control_manual"] = g_cfg.ev_control_manual;
  ev["manual_mode"] = g_cfg.ev_manual_mode;
  ev["relay_on"] = g_cfg.ev_relay_on;
  ev["relay_off"] = g_cfg.ev_relay_off;
  ev["error"] = g_cfg.ev_error;
  serializeJson(doc, f);
  f.close();
  return true;
}

bool buzzerLoadFromFS() {
  if (!fsIsReady()) return false;
  if (!LittleFS.exists(CFG_PATH)) return false;
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) return false;
  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  JsonObject cfg = doc.as<JsonObject>();
  buzzerUpdateFromJson(cfg);
  return true;
}