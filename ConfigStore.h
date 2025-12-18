#pragma once
#include <Arduino.h>

// Jednoduché centrální úložiště konfigurace, kterou potřebují různé moduly.
// Zatím řešíme jen aktivní úrovně vstupů (inputActiveLevels),
// ostatní části configu lze do tohoto modulu rozšířit později.

namespace ConfigStore {

    // Počet položek v poli aktivních úrovní vstupů.
    // Pro desku ESP32-S3-POE-ETH-8DI-8DO máme 8 vstupů.
    constexpr uint8_t INPUT_ACTIVE_LEVELS_SIZE = 8;

    // Nastaví výchozí hodnoty (vše = 1 → HIGH = aktivní)
    void initDefaults();

    // Nastavení aktivních úrovní vstupů z pole (hodnoty 0/1).
    // levels  ... ukazatel na pole hodnot (0 = LOW aktivní, 1 = HIGH aktivní)
    // count   ... počet položek v poli levels
    //
    // Reálně se uloží min(count, INPUT_ACTIVE_LEVELS_SIZE) prvků.
    // Zbytek (pokud je count menší) se doplní na bezpečný default = 1 (HIGH aktivní).
    void setInputActiveLevels(const uint8_t* levels, uint8_t count);

    // Získání aktivní úrovně pro daný vstup (0 = LOW, 1 = HIGH).
    // Pokud je index mimo rozsah, vrátí 1 (bezpečný default = HIGH aktivní).
    uint8_t getInputActiveLevel(uint8_t index);
}
