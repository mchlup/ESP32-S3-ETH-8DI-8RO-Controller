#include "ConfigStore.h"

#include <Preferences.h>

namespace {
  Preferences g_prefs;
  bool g_inited = false;
  uint8_t g_levels[8] = {0,0,0,0,0,0,0,0}; // default active-low

  void load() {
    if (!g_prefs.begin("cfg", true)) return;
    size_t len = g_prefs.getBytesLength("in_lvl");
    if (len == sizeof(g_levels)) {
      g_prefs.getBytes("in_lvl", g_levels, sizeof(g_levels));
    }
    g_prefs.end();
  }

  void save() {
    if (!g_prefs.begin("cfg", false)) return;
    g_prefs.putBytes("in_lvl", g_levels, sizeof(g_levels));
    g_prefs.end();
  }
}

namespace ConfigStore {
  void begin() {
    if (g_inited) return;
    g_inited = true;
    load();
  }

  uint8_t getInputActiveLevel(uint8_t inputIndex) {
    begin();
    if (inputIndex >= 8) return 0;
    return g_levels[inputIndex] ? 1 : 0;
  }

  void setInputActiveLevels(const uint8_t* levels, uint8_t count) {
    begin();
    const uint8_t n = (count > 8) ? 8 : count;
    for (uint8_t i = 0; i < n; i++) {
      g_levels[i] = levels[i] ? 1 : 0;
    }
    save();
  }
}
