#include "InputController.h"
#include "config_pins.h"
#include "ConfigStore.h"

static const uint16_t DEBOUNCE_MS = 50;

struct InputItem {
    uint8_t   pin;
    bool      stableRaw;     // odfiltrovaný (debounced) RAW stav (HIGH/LOW)
    bool      lastReadRaw;   // posledně přečtený (nedebounced) RAW stav
    uint32_t  lastChange;    // čas poslední změny lastReadRaw
};

static InputItem inputs[INPUT_COUNT] = {
    { INPUT1_PIN, true, true, 0 },
    { INPUT2_PIN, true, true, 0 },
    { INPUT3_PIN, true, true, 0 },
    { INPUT4_PIN, true, true, 0 },
    { INPUT5_PIN, true, true, 0 },
    { INPUT6_PIN, true, true, 0 },
    { INPUT7_PIN, true, true, 0 },
    { INPUT8_PIN, true, true, 0 },
};

static InputChangeCallback callback = nullptr;

void inputInit() {
    const uint32_t now = millis();

    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        uint8_t pin = inputs[i].pin;

        if (pin == 0xFF) {
            // Nepřiřazený vstup – jen inicializujeme stavy
            inputs[i].stableRaw   = false;
            inputs[i].lastReadRaw = false;
            inputs[i].lastChange  = now;
            continue;
        }

        // Podle Waveshare/ESPHome referencí jsou DI typicky "active-low".
        // Interní pull-up pomůže i při pasivním vstupu (dry contact) a eliminuje plovoucí stav.
        pinMode(pin, INPUT_PULLUP);

        bool raw = (digitalRead(pin) == HIGH);
        inputs[i].stableRaw   = raw;
        inputs[i].lastReadRaw = raw;
        inputs[i].lastChange  = now;
    }
}

void inputSetCallback(InputChangeCallback cb) {
    callback = cb;
}

static bool isIndexValid(InputId id) {
    return static_cast<uint8_t>(id) < INPUT_COUNT;
}

bool inputGetRaw(InputId id) {
    if (!isIndexValid(id)) return false;
    return inputs[static_cast<uint8_t>(id)].stableRaw;
}

bool inputGetState(InputId id) {
    if (!isIndexValid(id)) return false;

    uint8_t index = static_cast<uint8_t>(id);
    const InputItem &item = inputs[index];

    // 0 = LOW je aktivní, 1 = HIGH je aktivní
    uint8_t activeLevel = ConfigStore::getInputActiveLevel(index);
    if (activeLevel == 0) {
        // aktivní při LOW
        return (item.stableRaw == false);
    } else {
        // aktivní při HIGH
        return (item.stableRaw == true);
    }
}

void inputUpdate() {
    const uint32_t now = millis();

    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
        uint8_t pin = inputs[i].pin;
        if (pin == 0xFF) {
            // vstup není využit
            continue;
        }

        bool raw = (digitalRead(pin) == HIGH);

        // zaznamenání okamžiku změny RAW stavu
        if (raw != inputs[i].lastReadRaw) {
            inputs[i].lastReadRaw = raw;
            inputs[i].lastChange  = now;
        }

        // Debounce – čekáme, až se RAW stav ustálí
        if ((now - inputs[i].lastChange) >= DEBOUNCE_MS) {
            if (inputs[i].stableRaw != raw) {
                inputs[i].stableRaw = raw;

                // Přepočet na logický stav podle polarity
                if (callback != nullptr) {
                    InputId id = static_cast<InputId>(i);
                    bool logical = inputGetState(id);
                    callback(id, logical);
                }
            }
        }
    }
}
