// IMPORTANT: include Features.h first so FEATURE_RGB_LED is visible before
// including the header (which provides stubs when the feature is disabled).
#include "Features.h"
#include "RgbLedController.h"

#if defined(FEATURE_RGB_LED)

#include <Adafruit_NeoPixel.h>
#include "config_pins.h"
#include <ArduinoJson.h>

// Na desce je 1 ks WS2812 RGB LED (Waveshare: GPIO38)
static Adafruit_NeoPixel g_strip(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

static bool g_inited = false;
static RgbLedMode g_mode = RgbLedMode::OFF;

// runtime config
static bool s_enabled = true;
static uint8_t s_brightness = 64; // default matches previous hardcoded value
static bool s_allowStatus = true;

// pro SOLID
static uint8_t g_r = 0, g_g = 0, g_b = 0;

// blink state (non-blocking)
static bool g_blinkOn = false;
static uint32_t g_lastBlinkMs = 0;
static uint16_t g_blinkPeriodMs = 500;
static uint8_t g_blinkR = 0, g_blinkG = 0, g_blinkB = 0;

static void applyColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_inited) return;
    g_strip.setPixelColor(0, g_strip.Color(r, g, b));
    g_strip.show();
}

static void applyBrightness() {
    if (!g_inited) return;
    g_strip.setBrightness(s_brightness);
    g_strip.show();
}

void rgbLedOff() {
    if (!g_inited) rgbLedInit();
    g_mode = RgbLedMode::OFF;
    applyColor(0, 0, 0);
}

void rgbLedInit() {
    if (g_inited) return;

    g_strip.begin();
    g_strip.clear();
    g_strip.setBrightness(s_brightness);
    g_strip.show();

    g_inited = true;
    g_lastBlinkMs = millis();
}

void rgbLedSetColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_inited) rgbLedInit();
    if (!s_enabled) {
        rgbLedOff();
        return;
    }
    g_r = r; g_g = g; g_b = b;
    g_mode = RgbLedMode::SOLID;
    applyColor(g_r, g_g, g_b);
}

void rgbLedSetMode(RgbLedMode mode) {
    if (!g_inited) rgbLedInit();

    if (!s_enabled || (!s_allowStatus && (mode == RgbLedMode::BLE_DISABLED || mode == RgbLedMode::BLE_IDLE ||
                                         mode == RgbLedMode::BLE_CONNECTED || mode == RgbLedMode::BLE_PAIRING ||
                                         mode == RgbLedMode::ERROR))) {
        rgbLedOff();
        return;
    }

    if (g_mode == mode) return;
    g_mode = mode;

    // reset blink
    g_blinkOn = false;
    g_lastBlinkMs = millis();

    // okamžitá aplikace (pro solid stavy)
    switch (g_mode) {
        case RgbLedMode::OFF:
        case RgbLedMode::BLE_DISABLED:
            applyColor(0, 0, 0);
            break;
        case RgbLedMode::SOLID:
            applyColor(g_r, g_g, g_b);
            break;
        case RgbLedMode::BLE_IDLE:
            // slabá modrá
            applyColor(0, 0, 24);
            break;
        case RgbLedMode::BLE_CONNECTED:
            // silnější modrá
            applyColor(0, 0, 128);
            break;
        case RgbLedMode::BLE_PAIRING:
        case RgbLedMode::ERROR:
            // nastaví v loop()
            break;
    }
}

void rgbLedBlink(uint8_t r, uint8_t g, uint8_t b, uint16_t periodMs) {
    if (!g_inited) rgbLedInit();
    if (!s_enabled) {
        rgbLedOff();
        return;
    }

    // Sanity limits
    if (periodMs < 50) periodMs = 50;
    if (periodMs > 5000) periodMs = 5000;

    g_blinkR = r;
    g_blinkG = g;
    g_blinkB = b;
    g_blinkPeriodMs = periodMs;

    g_mode = RgbLedMode::BLINK;
    g_blinkOn = false;
    g_lastBlinkMs = millis();
    applyColor(0, 0, 0);
}

void rgbLedLoop() {
    if (!g_inited) return;
    if (!s_enabled || (!s_allowStatus && (g_mode == RgbLedMode::BLE_DISABLED || g_mode == RgbLedMode::BLE_IDLE ||
                                         g_mode == RgbLedMode::BLE_CONNECTED || g_mode == RgbLedMode::BLE_PAIRING ||
                                         g_mode == RgbLedMode::ERROR))) {
        return;
    }

    const uint32_t now = millis();

    // blikání – 2 Hz
    const uint32_t BLINK_MS = 250;

    // User-requested blink mode (period configurable)
    if (g_mode == RgbLedMode::BLINK) {
        const uint32_t half = (uint32_t)g_blinkPeriodMs / 2U;
        if (half > 0 && (now - g_lastBlinkMs >= half)) {
            g_lastBlinkMs = now;
            g_blinkOn = !g_blinkOn;
            if (g_blinkOn) applyColor(g_blinkR, g_blinkG, g_blinkB);
            else applyColor(0, 0, 0);
        }
        return;
    }

    if (g_mode == RgbLedMode::BLE_PAIRING) {
        if (now - g_lastBlinkMs >= BLINK_MS) {
            g_lastBlinkMs = now;
            g_blinkOn = !g_blinkOn;
            if (g_blinkOn) applyColor(0, 0, 180);
            else applyColor(0, 0, 0);
        }
        return;
    }

    if (g_mode == RgbLedMode::ERROR) {
        if (now - g_lastBlinkMs >= BLINK_MS) {
            g_lastBlinkMs = now;
            g_blinkOn = !g_blinkOn;
            if (g_blinkOn) applyColor(180, 0, 0);
            else applyColor(0, 0, 0);
        }
        return;
    }

    // ostatní režimy jsou statické – nic nedělej
}

void rgbLedApplyConfig(const String& json) {
    StaticJsonDocument<192> filter;
    filter["rgbLed"]["enabled"] = true;
    filter["rgbLed"]["brightness"] = true;
    filter["rgbLed"]["allowStatus"] = true;

    StaticJsonDocument<384> doc;
    DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
    if (err) return;

    JsonObject o = doc["rgbLed"].as<JsonObject>();
    if (o.isNull()) return;

    if (o.containsKey("enabled")) s_enabled = (bool)o["enabled"];
    if (o.containsKey("brightness")) {
        int b = (int)(o["brightness"] | (int)s_brightness);
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        s_brightness = (uint8_t)b;
    }
    if (o.containsKey("allowStatus")) s_allowStatus = (bool)o["allowStatus"];

    if (!g_inited) rgbLedInit();
    applyBrightness();
    if (!s_enabled) {
        rgbLedOff();
    } else {
        // re-apply current mode (forces immediate color)
        const RgbLedMode m = g_mode;
        g_mode = (RgbLedMode)255;
        rgbLedSetMode(m);
    }
}

#endif // FEATURE_RGB_LED
