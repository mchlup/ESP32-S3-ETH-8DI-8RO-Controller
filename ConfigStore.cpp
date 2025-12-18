#include "ConfigStore.h"

namespace {
    // 1 = HIGH je aktivní, 0 = LOW je aktivní
    // Velikost pole je dána konstantou v namespace ConfigStore.
    uint8_t g_inputActiveLevels[ConfigStore::INPUT_ACTIVE_LEVELS_SIZE] = {
        0, 0, 0, 0, 0, 0, 0, 0
    };
}

namespace ConfigStore {

void initDefaults() {
    // Všech 8 vstupů = LOW je aktivní (doporučené pro Waveshare DI s INPUT_PULLUP)
    for (uint8_t i = 0; i < INPUT_ACTIVE_LEVELS_SIZE; i++) {
        g_inputActiveLevels[i] = 0;
    }
}

void setInputActiveLevels(const uint8_t* levels, uint8_t count) {
    if (!levels) return;

    if (count > INPUT_ACTIVE_LEVELS_SIZE) {
        count = INPUT_ACTIVE_LEVELS_SIZE;
    }

    // Přepíšeme prvních "count" položek
    for (uint8_t i = 0; i < count; i++) {
        g_inputActiveLevels[i] = levels[i] ? 1 : 0;
    }
}

uint8_t getInputActiveLevel(uint8_t index) {
    if (index >= INPUT_ACTIVE_LEVELS_SIZE) {
        // mimo rozsah → bezpečný default = LOW aktivní
        return 0;
    }
    return g_inputActiveLevels[index];
}

} // namespace ConfigStore
