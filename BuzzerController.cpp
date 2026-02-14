// IMPORTANT: include Features.h first so FEATURE_BUZZER is visible before
// including the header (which provides stubs when the feature is disabled).
#include "Features.h"
#include "BuzzerController.h"

#if defined(FEATURE_BUZZER)

#include <Arduino.h>
#include <esp_system.h>
#include "config_pins.h"
#include <ArduinoJson.h>

// ESP32: use LEDC for smoother audio (glissando + amplitude envelope).
// Arduino-ESP32 3.x uses a pin-based LEDC API (ledcAttach/ledcWriteTone/ledcWrite).

namespace {
  bool g_inited = false;
  bool g_playing = false;

  // Pattern player (non-blocking)
  struct Step {
    uint16_t startHz;    // 0 => silence
    uint16_t endHz;      // for glissando (0 => silence)
    uint16_t durationMs; // duration of this step
    uint16_t attackMs;   // ramp-in time (0 => instant)
    uint16_t releaseMs;  // ramp-out time (0 => instant)
  };

  const Step* g_steps = nullptr;
  uint8_t g_stepCount = 0;
  uint8_t g_stepIndex = 0;
  uint8_t g_repeatsLeft = 0;
  uint32_t g_stepStartMs = 0;
  uint32_t g_lastTickMs = 0;

  // Per-play micro-detune and vibrato phase (to avoid sterile 8-bit feel)
  float g_detuneCents = 0.f;
  float g_vibPhase = 0.f;

  // LEDC audio
  constexpr int kLedcResolutionBits = 8; // duty: 0..255
  constexpr int kLedcBaseFreq = 2000;    // placeholder (real freq is set by ledcWriteTone)

  // runtime config
  bool s_enabled = true;
  uint16_t s_defaultFreq = 2000;
  uint16_t s_defaultDur = 80;
  uint8_t s_volume = 180; // 0..255
  // sound shaping (for more "modern" chimes)
  float s_envAttackCurve = 2.2f;  // >1 = softer attack
  float s_envReleaseCurve = 2.0f; // >1 = softer release
  uint8_t s_vibratoCents = 7;     // 0 disables vibrato
  float s_vibratoHz = 5.5f;       // vibrato rate
  uint8_t s_detuneCents = 3;      // per-play random detune (+/- cents)

  bool s_notifyMode = true;
  bool s_notifyManual = true;

  // notification -> sound mapping (runtime config)
  String s_soundInfo = "chime";
  String s_soundWarning = "gong";
  String s_soundAlarm = "alert";

  void beginStep(const Step& s);

  void stopNow() {
    // Keep PWM configured, just mute output.
    ledcWrite(BUZZER_PIN, 0);
    g_playing = false;
    g_steps = nullptr;
    g_stepCount = 0;
    g_stepIndex = 0;
    g_repeatsLeft = 0;
  }

  void startPattern(const Step* steps, uint8_t stepCount, uint8_t repeats) {
    if (!g_inited) buzzerInit();
    if (!s_enabled) return;
    if (!steps || stepCount == 0) return;

        // Randomize micro-detune and vibrato phase per play so repeated
    // notifications don't sound identical.
    if (s_detuneCents > 0) {
      const int range = (int)s_detuneCents;
      const int r = (int)(esp_random() % (uint32_t)(range * 2 + 1)) - range;
      g_detuneCents = (float)r;
    } else {
      g_detuneCents = 0.f;
    }
    g_vibPhase = ((float)(esp_random() & 0xFFFF) / 65535.f) * 6.2831853f;

g_steps = steps;
    g_stepCount = stepCount;
    g_stepIndex = 0;
    g_repeatsLeft = (repeats == 0 ? 1 : repeats);
    g_playing = true;
    g_lastTickMs = 0; // force immediate tick
    beginStep(g_steps[g_stepIndex]);
  }

  static inline uint8_t clampU8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
  }

  static inline uint16_t lerpU16(uint16_t a, uint16_t b, float t) {
    if (t <= 0.f) return a;
    if (t >= 1.f) return b;
    return (uint16_t)(a + (float)(b - a) * t + 0.5f);
  }

  // Calculate envelope multiplier in range 0..1.
  // A simple attack + release envelope makes the sound much softer and "modern".
  static float envelope01(uint32_t elapsedMs, uint32_t durMs, uint16_t attackMs, uint16_t releaseMs) {
    if (durMs == 0) return 0.f;

    // Attack: ease-in (pow) so the start is soft.
    float envA = 1.f;
    if (attackMs > 0) {
      float t = (float)elapsedMs / (float)attackMs;
      if (t < 0.f) t = 0.f;
      if (t > 1.f) t = 1.f;
      const float curve = (s_envAttackCurve < 0.2f) ? 0.2f : s_envAttackCurve;
      envA = powf(t, curve);
    }

    // Release: ease-out (pow) so the end is soft.
    float envR = 1.f;
    if (releaseMs > 0) {
      uint32_t tailMs = (durMs > elapsedMs) ? (durMs - elapsedMs) : 0;
      float t = (float)tailMs / (float)releaseMs;
      if (t < 0.f) t = 0.f;
      if (t > 1.f) t = 1.f;
      const float curve = (s_envReleaseCurve < 0.2f) ? 0.2f : s_envReleaseCurve;
      envR = powf(t, curve);
    }

    float env = envA;
    if (envR < env) env = envR;
    if (env < 0.f) env = 0.f;
    if (env > 1.f) env = 1.f;
    return env;
  }

void beginStep(const Step& s) {
    g_stepStartMs = millis();
    // Set initial frequency (or keep a placeholder if silent)
    if (s.startHz == 0 || s.endHz == 0) {
      ledcWriteTone(BUZZER_PIN, (uint32_t)kLedcBaseFreq);
      ledcWrite(BUZZER_PIN, 0);
    } else {
      ledcWriteTone(BUZZER_PIN, (uint32_t)s.startHz);
      ledcWrite(BUZZER_PIN, 0); // envelope will ramp in
    }
  }

  const Step* soundSteps(const String& id, uint8_t* outCount) {
    if (outCount) *outCount = 0;

    // "chime" – short, modern-ish 3-note upward notification
    // NOTE: numbers are tuned to sound pleasant on typical piezo buzzers.
    // Steps include a short attack/release so there are no harsh clicks.

    // "chime" – short, modern-ish upward notification with small glides
    static const Step chime[] = {
      { 1120, 1200, 70, 10, 25 }, { 0, 0, 25, 0, 0 },
      { 1480, 1580, 85, 12, 30 }, { 0, 0, 25, 0, 0 },
      { 1860, 1980, 130, 14, 45 }, { 0, 0, 80, 0, 0 },
    };

    // "vag" – softer 2-tone "car-like" chime.
    // Goal: closer to the linked video style (pleasant, not harsh), with longer decay.
    // It is NOT intended to be an exact copy of any OEM sound.
    // Tuning tip: if your piezo is shrill, lower volume or reduce the top note.
    static const Step vag[] = {
      // gentle glide into note 1
      { 980, 1046, 210, 22, 140 },
      {   0,    0,  35,  0,   0 },
      // note 2 (major 3rd-ish above), with soft decay
      { 1245, 1318, 260, 24, 180 },
      {   0,    0, 140,  0,   0 },
    };

    // "gong" – already used for car-like 2-tone gong
    // "gong" – 2-tone gong with soft envelope
    static const Step gong[] = {
      { 900, 860, 180, 12, 60 },
      {   0,   0,  55,  0,  0 },
      { 680, 640, 320, 14, 120 },
      {   0,   0, 140,  0,  0 },
    };

    // "alert" – stronger descending alert (2 bursts)
    // "alert" – stronger descending alert (with glide) but still non-harsh
    static const Step alert[] = {
      { 2100, 1700, 90, 8, 20 }, { 0, 0, 30, 0, 0 },
      { 1700, 1300, 110, 10, 28 }, { 0, 0, 30, 0, 0 },
      { 1300, 980, 170, 12, 55 }, { 0, 0, 140, 0, 0 },
    };

    if (id == "chime") { if (outCount) *outCount = (uint8_t)(sizeof(chime)/sizeof(chime[0])); return chime; }
    if (id == "vag")   { if (outCount) *outCount = (uint8_t)(sizeof(vag)/sizeof(vag[0])); return vag; }
    if (id == "gong")  { if (outCount) *outCount = (uint8_t)(sizeof(gong)/sizeof(gong[0])); return gong; }
    if (id == "alert") { if (outCount) *outCount = (uint8_t)(sizeof(alert)/sizeof(alert[0])); return alert; }
    return nullptr;
  }
}

void buzzerInit() {
  if (g_inited) return;
  g_inited = true;

  // Configure LEDC PWM for audio.
  // Use 8-bit resolution so we can control duty ("volume") with 0..255.
  // Arduino-ESP32 3.x (IDF 5+) pin-based LEDC setup.
  // The channel is allocated automatically.
  ledcAttach(BUZZER_PIN, kLedcBaseFreq, kLedcResolutionBits);
  ledcWriteTone(BUZZER_PIN, kLedcBaseFreq);
  ledcWrite(BUZZER_PIN, 0);
  stopNow();
}

void buzzerBeep(uint16_t freqHz, uint16_t durationMs) {
  const uint16_t f = (freqHz == 0 ? s_defaultFreq : freqHz);
  const uint16_t d = (durationMs == 0 ? s_defaultDur : durationMs);
  static Step one[2];
  // small gap after beep to ensure a clean stop
  one[0] = { f, f, d, 8, 35 };
  one[1] = { 0, 0, 25, 0, 0 };
  startPattern(one, 2, 1);
}

void buzzerGong(uint8_t repeats) {
  uint8_t cnt = 0;
  const Step* steps = soundSteps("gong", &cnt);
  if (!steps || cnt == 0) return;
  startPattern(steps, cnt, repeats);
}

void buzzerPlaySound(const char* soundId, uint8_t repeats) {
  if (!g_inited) buzzerInit();
  if (!s_enabled) return;

  const String id = String(soundId ? soundId : "");
  if (id.length() == 0 || id == "beep") {
    buzzerBeep(0, 0);
    return;
  }
  if (id == "off") {
    stopNow();
    return;
  }

  uint8_t cnt = 0;
  const Step* steps = soundSteps(id, &cnt);
  if (!steps || cnt == 0) {
    // fallback
    buzzerBeep(0, 0);
    return;
  }
  startPattern(steps, cnt, (repeats == 0 ? 1 : repeats));
}

void buzzerNotify(const char* type) {
  const String t = String(type ? type : "");
  if (t == "warning") {
    buzzerPlaySound(s_soundWarning.c_str(), 1);
    return;
  }
  if (t == "alarm") {
    buzzerPlaySound(s_soundAlarm.c_str(), 1);
    return;
  }
  // default: info
  buzzerPlaySound(s_soundInfo.c_str(), 1);
}

void buzzerOnControlModeChanged(bool autoMode) {
  if (!s_enabled || !s_notifyMode) return;

  // "Modern" UX: use configured info sound for mode changes.
  // (autoMode is currently not distinguished; user can map sounds via config.)
  (void)autoMode;
  buzzerNotify("info");
}

void buzzerOnManualModeChanged(const char* /*modeName*/) {
  if (!s_enabled || !s_notifyManual) return;
  buzzerNotify("info");
}

void buzzerApplyConfig(const String& json) {
  StaticJsonDocument<192> filter;
  filter["buzzer"]["enabled"] = true;
  filter["buzzer"]["freqHz"] = true;
  filter["buzzer"]["durationMs"] = true;
  filter["buzzer"]["volume"] = true;
  filter["buzzer"]["envAttackCurve"] = true;
  filter["buzzer"]["envReleaseCurve"] = true;
  filter["buzzer"]["vibratoCents"] = true;
  filter["buzzer"]["vibratoHz"] = true;
  filter["buzzer"]["detuneCents"] = true;
  filter["buzzer"]["notifyMode"] = true;
  filter["buzzer"]["notifyManual"] = true;
  filter["buzzer"]["notifications"]["info"] = true;
  filter["buzzer"]["notifications"]["warning"] = true;
  filter["buzzer"]["notifications"]["alarm"] = true;

  StaticJsonDocument<320> doc;
  DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
  if (err) return;

  JsonObject b = doc["buzzer"].as<JsonObject>();
  if (b.isNull()) return;

  if (b.containsKey("enabled")) s_enabled = (bool)b["enabled"];
  if (b.containsKey("freqHz")) {
    int f = (int)(b["freqHz"] | (int)s_defaultFreq);
    if (f < 10) f = 10;
    if (f > 10000) f = 10000;
    s_defaultFreq = (uint16_t)f;
  }
  if (b.containsKey("durationMs")) {
    int d = (int)(b["durationMs"] | (int)s_defaultDur);
    if (d < 10) d = 10;
    if (d > 2000) d = 2000;
    s_defaultDur = (uint16_t)d;
  }

  if (b.containsKey("volume")) {
    int v = (int)(b["volume"] | (int)s_volume);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    s_volume = (uint8_t)v;
  }

  if (b.containsKey("envAttackCurve")) {
    float c = (float)(b["envAttackCurve"] | (double)s_envAttackCurve);
    if (c < 0.2f) c = 0.2f;
    if (c > 8.0f) c = 8.0f;
    s_envAttackCurve = c;
  }
  if (b.containsKey("envReleaseCurve")) {
    float c = (float)(b["envReleaseCurve"] | (double)s_envReleaseCurve);
    if (c < 0.2f) c = 0.2f;
    if (c > 8.0f) c = 8.0f;
    s_envReleaseCurve = c;
  }
  if (b.containsKey("vibratoCents")) {
    int vc = (int)(b["vibratoCents"] | (int)s_vibratoCents);
    if (vc < 0) vc = 0;
    if (vc > 60) vc = 60;
    s_vibratoCents = (uint8_t)vc;
  }
  if (b.containsKey("vibratoHz")) {
    float vr = (float)(b["vibratoHz"] | (double)s_vibratoHz);
    if (vr < 0.f) vr = 0.f;
    if (vr > 20.f) vr = 20.f;
    s_vibratoHz = vr;
  }
  if (b.containsKey("detuneCents")) {
    int dc = (int)(b["detuneCents"] | (int)s_detuneCents);
    if (dc < 0) dc = 0;
    if (dc > 30) dc = 30;
    s_detuneCents = (uint8_t)dc;
  }

  if (b.containsKey("notifyMode")) s_notifyMode = (bool)b["notifyMode"];
  if (b.containsKey("notifyManual")) s_notifyManual = (bool)b["notifyManual"];

  if (b.containsKey("notifications")) {
    JsonObject n = b["notifications"].as<JsonObject>();
    if (!n.isNull()) {
      if (n.containsKey("info")) s_soundInfo = String((const char*)(n["info"] | "chime"));
      if (n.containsKey("warning")) s_soundWarning = String((const char*)(n["warning"] | "gong"));
      if (n.containsKey("alarm")) s_soundAlarm = String((const char*)(n["alarm"] | "alert"));
    }
  }

  // If disabled -> stop any ongoing tone
  if (!s_enabled && g_inited) stopNow();
}

void buzzerLoop() {
  if (!g_inited) return;
  if (!g_playing) return;

  // Soft real-time tick (non-blocking). 5 ms gives smooth ramps without
  // consuming too much CPU.
  const uint32_t now = millis();
  if (g_lastTickMs != 0 && (uint32_t)(now - g_lastTickMs) < 5) return;
  g_lastTickMs = now;

  if (!g_steps || g_stepCount == 0) {
    stopNow();
    return;
  }

  if (g_stepIndex >= g_stepCount) {
    // repeat or stop
    if (g_repeatsLeft > 1) {
      g_repeatsLeft--;
      g_stepIndex = 0;
      beginStep(g_steps[g_stepIndex]);
    } else {
      stopNow();
      return;
    }
  }

  const Step& s = g_steps[g_stepIndex];
  const uint32_t elapsed = (uint32_t)(now - g_stepStartMs);

  // Step finished -> advance
  if (elapsed >= (uint32_t)s.durationMs) {
    g_stepIndex++;
    if (g_stepIndex >= g_stepCount) {
      // will repeat/stop on next tick
      return;
    }
    beginStep(g_steps[g_stepIndex]);
    return;
  }

  // Silent step
  if (s.startHz == 0 || s.endHz == 0) {
    ledcWrite(BUZZER_PIN, 0);
    return;
  }

  // Smooth frequency glide + vibrato/detune + envelope
  const float t = (s.durationMs == 0) ? 1.f : (float)elapsed / (float)s.durationMs;
  const uint16_t baseHz = lerpU16(s.startHz, s.endHz, t);

  // Apply per-play micro detune (in cents)
  double freq = (double)baseHz * pow(2.0, (double)g_detuneCents / 1200.0);

  // Apply vibrato (in cents)
  if (s_vibratoCents > 0 && s_vibratoHz > 0.05f) {
    const float tt = (float)elapsed / 1000.0f;
    const float vib = sinf((float)(2.0f * 3.1415926f) * s_vibratoHz * tt + g_vibPhase);
    const float cents = (float)s_vibratoCents * vib;
    freq *= pow(2.0, (double)cents / 1200.0);
  }

  if (freq < 10.0) freq = 10.0;
  if (freq > 12000.0) freq = 12000.0;
  ledcWriteTone(BUZZER_PIN, freq);

  const float env = envelope01(elapsed, s.durationMs, s.attackMs, s.releaseMs);
  const int duty = (int)((float)s_volume * env + 0.5f);
  ledcWrite(BUZZER_PIN, clampU8(duty));
}

#endif // FEATURE_BUZZER
