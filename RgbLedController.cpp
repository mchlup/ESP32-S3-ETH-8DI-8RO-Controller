// IMPORTANT: include Features.h first so FEATURE_RGB_LED is visible before
// including the header (which provides stubs when the feature is disabled).
#include "Features.h"
#include "RgbLedController.h"

#if defined(FEATURE_RGB_LED)

#include <Adafruit_NeoPixel.h>
#include "config_pins.h"

// Na desce je 1 ks WS2812 RGB LED (Waveshare: GPIO38)
static Adafruit_NeoPixel g_strip(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

static bool g_inited = false;
static RgbLedMode g_mode = RgbLedMode::OFF;

// pro SOLID
static uint8_t g_r = 0, g_g = 0, g_b = 0;

// blink state (non-blocking)
static bool g_blinkOn = false;
static uint32_t g_lastBlinkMs = 0;

static void applyColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_inited) return;
    g_strip.setPixelColor(0, g_strip.Color(r, g, b));
    g_strip.show();
}

void rgbLedInit() {
    if (g_inited) return;

    g_strip.begin();
    g_strip.clear();
    g_strip.setBrightness(64); // mírné ztlumení
    g_strip.show();

    g_inited = true;
    g_lastBlinkMs = millis();
}

void rgbLedSetColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_inited) rgbLedInit();
    g_r = r; g_g = g; g_b = b;
    g_mode = RgbLedMode::SOLID;
    applyColor(g_r, g_g, g_b);
}

void rgbLedSetMode(RgbLedMode mode) {
    if (!g_inited) rgbLedInit();

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

void rgbLedLoop() {
    if (!g_inited) return;

    const uint32_t now = millis();

    // blikání – 2 Hz
    const uint32_t BLINK_MS = 250;

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

#endif // FEATURE_RGB_LED
