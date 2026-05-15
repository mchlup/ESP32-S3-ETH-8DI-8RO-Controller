#include "TemperatureManager.h"

#include "config_pins.h"
#include "ConfigStore.h"

#include "OpenThermController.h"
#include "BleController.h"
#include "DallasController.h"

#include <algorithm>
#include <vector>

namespace {
  struct RoleBindingDef {
    TempRole role;
    const char* key;
    const char* label;
    uint8_t gpio;
    bool assignable;
  };

  constexpr RoleBindingDef kRoleBindings[] = {
    {TempRole::Flow,       "flow",       "Výstup kotle / CH", 255, false},
    {TempRole::DhwTank,    "dhw_tank",   "TUV zásobník",      DALLAS_TANK_PIN, true},
    {TempRole::Outside,    "outside",    "Venkovní teplota",  DALLAS_IO0_PIN, true},
    {TempRole::Return,     "return",     "Zpátečka / Return.flow", DALLAS_RETURN_PIN, true},
    {TempRole::TankTop,    "tank_top",   "AKU nahoře",        DALLAS_TANK_PIN, true},
    {TempRole::TankMid,    "tank_mid",   "AKU uprostřed",     DALLAS_TANK_PIN, true},
    {TempRole::TankBottom, "tank_bottom", "AKU dole",         DALLAS_TANK_PIN, true},
    {TempRole::DhwReturn,  "dhw_return", "Návrat TUV",        DALLAS_DHW_RETURN_PIN, true},
  };

  static const RoleBindingDef* findRoleBinding(TempRole role) {
    for (const auto& it : kRoleBindings) if (it.role == role) return &it;
    return nullptr;
  }

  struct CacheItem {
    float c = NAN;
    bool valid = false;
    TempSource src = TempSource::None;
    uint32_t updatedMs = 0;
    uint8_t gpio = 255;
    uint64_t rom = 0;
  };

  CacheItem g_cache[(uint8_t)TempRole::COUNT];
  bool g_inited = false;

  static inline void setCache(TempRole role, float c, bool valid, TempSource src, uint32_t updatedMs, uint8_t gpio=255, uint64_t rom=0) {
    CacheItem &it = g_cache[(uint8_t)role];
    it.c = c;
    it.valid = valid && isfinite(c);
    it.src = it.valid ? src : TempSource::None;
    it.updatedMs = it.valid ? updatedMs : 0;
    it.gpio = it.valid ? gpio : 255;
    it.rom = it.valid ? rom : 0;
  }

  static inline bool pickDallasByRom(uint8_t gpio, uint64_t rom, float& outC, uint64_t& outRom) {
    const DallasGpioStatus* st = DallasController::getStatus(gpio);
    if (!st) return false;
    for (auto &d : st->devices) {
      if (!d.valid || !isfinite(d.temperature)) continue;
      if (d.rom == rom) {
        outC = d.temperature;
        outRom = d.rom;
        return true;
      }
    }
    return false;
  }

  static inline bool pickDallasFirstValid(uint8_t gpio, float& outC, uint64_t& outRom) {
    const DallasGpioStatus* st = DallasController::getStatus(gpio);
    if (!st) return false;
    for (auto &d : st->devices) {
      if (d.valid && isfinite(d.temperature)) {
        outC = d.temperature;
        outRom = d.rom;
        return true;
      }
    }
    return false;
  }

  static inline bool pickDallasSortedIndex(uint8_t gpio, uint8_t idx, float& outC, uint64_t& outRom) {
    const DallasGpioStatus* st = DallasController::getStatus(gpio);
    if (!st) return false;
    struct Item { uint64_t rom; float t; bool ok; };
    std::vector<Item> items;
    items.reserve(st->devices.size());
    for (auto &d : st->devices) {
      Item it{d.rom, d.temperature, d.valid && isfinite(d.temperature)};
      items.push_back(it);
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b){ return a.rom < b.rom; });
    uint8_t seen = 0;
    for (auto &it : items) {
      if (!it.ok) continue;
      if (seen == idx) {
        outC = it.t;
        outRom = it.rom;
        return true;
      }
      seen++;
    }
    return false;
  }

  static inline uint64_t cfgRomForRole(TempRole role) {
    switch (role) {
      case TempRole::TankTop: return ConfigStore::getDallasTankTopRom();
      case TempRole::TankMid: return ConfigStore::getDallasTankMidRom();
      case TempRole::TankBottom: return ConfigStore::getDallasTankBottomRom();
      case TempRole::Return: return ConfigStore::getDallasReturnRom();
      case TempRole::DhwReturn: return ConfigStore::getDallasDhwReturnRom();
      case TempRole::DhwTank: return ConfigStore::getDallasDhwTankRom();
      case TempRole::Outside: return ConfigStore::getDallasOutsideRom();
      default: return 0;
    }
  }

  static inline void setCfgRomForRole(TempRole role, uint64_t rom) {
    switch (role) {
      case TempRole::TankTop: ConfigStore::setDallasTankTopRom(rom); break;
      case TempRole::TankMid: ConfigStore::setDallasTankMidRom(rom); break;
      case TempRole::TankBottom: ConfigStore::setDallasTankBottomRom(rom); break;
      case TempRole::Return: ConfigStore::setDallasReturnRom(rom); break;
      case TempRole::DhwReturn: ConfigStore::setDallasDhwReturnRom(rom); break;
      case TempRole::DhwTank: ConfigStore::setDallasDhwTankRom(rom); break;
      case TempRole::Outside: ConfigStore::setDallasOutsideRom(rom); break;
      default: break;
    }
  }

  static inline uint8_t roleDefaultGpio(TempRole role) {
    switch (role) {
      case TempRole::Outside:
        return DALLAS_IO0_PIN;
      case TempRole::DhwTank:
        return DALLAS_TANK_PIN;
      case TempRole::TankTop:
      case TempRole::TankMid:
      case TempRole::TankBottom:
        return DALLAS_TANK_PIN;
      case TempRole::Return:
        return DALLAS_RETURN_PIN;
      case TempRole::DhwReturn:
        return DALLAS_DHW_RETURN_PIN;
      default:
        return 255;
    }
  }

  static inline bool roleCanUseDallas(TempRole role) {
    return roleDefaultGpio(role) != 255;
  }

  static inline void clearRoleCache(TempRole role) {
    setCache(role, NAN, false, TempSource::None, 0);
  }

  static inline void clearDallasBackedRoleCaches() {
    for (uint8_t i = 0; i < (uint8_t)TempRole::COUNT; i++) {
      const TempRole role = (TempRole)i;
      if (roleCanUseDallas(role)) clearRoleCache(role);
    }
  }

  static inline bool containsRom(const std::vector<uint64_t>& roms, uint64_t rom) {
    if (!rom) return false;
    return std::find(roms.begin(), roms.end(), rom) != roms.end();
  }

  static inline std::vector<uint64_t> collectValidDallasRomsSorted(uint8_t gpio) {
    std::vector<uint64_t> out;
    const DallasGpioStatus* st = DallasController::getStatus(gpio);
    if (!st) return out;
    out.reserve(st->devices.size());
    for (const auto &d : st->devices) {
      if (!d.valid || !isfinite(d.temperature)) continue;
      out.push_back(d.rom);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  static inline bool resolveDallasRoleValue(TempRole role, float& outC, uint64_t& outRom) {
    const uint8_t gpio = roleDefaultGpio(role);
    if (gpio == 255 || !ConfigStore::getDallasEnabled()) return false;
    const uint64_t cfg = cfgRomForRole(role);
    if (cfg) return pickDallasByRom(gpio, cfg, outC, outRom);
    return pickDallasFirstValid(gpio, outC, outRom);
  }

  static inline bool pickDallasAutoFromRemaining(uint8_t gpio, const std::vector<uint64_t>& reservedRoms, uint8_t idx, float& outC, uint64_t& outRom) {
    const DallasGpioStatus* st = DallasController::getStatus(gpio);
    if (!st) return false;
    struct Item { uint64_t rom; float t; bool ok; };
    std::vector<Item> items;
    items.reserve(st->devices.size());
    for (const auto &d : st->devices) {
      const bool ok = d.valid && isfinite(d.temperature) && !containsRom(reservedRoms, d.rom);
      items.push_back(Item{d.rom, d.temperature, ok});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b){ return a.rom < b.rom; });
    uint8_t seen = 0;
    uint64_t lastRom = 0;
    for (const auto &it : items) {
      if (!it.ok) continue;
      if (it.rom == lastRom) continue;
      lastRom = it.rom;
      if (seen == idx) {
        outC = it.t;
        outRom = it.rom;
        return true;
      }
      seen++;
    }
    return false;
  }

  static inline void updateDallasRoles(uint32_t now) {
    if (!ConfigStore::getDallasEnabled()) {
      // Clear all Dallas-backed roles. OT/BLE can repopulate them later in the same loop.
      clearDallasBackedRoleCaches();
      return;
    }

    // Outside DS (GPIO0). OT can override later.
    {
      float t; uint64_t rom;
      uint64_t cfg = cfgRomForRole(TempRole::Outside);
      bool ok = false;
      if (cfg) ok = pickDallasByRom(DALLAS_IO0_PIN, cfg, t, rom);
      if (!ok) ok = pickDallasFirstValid(DALLAS_IO0_PIN, t, rom);
      if (ok) setCache(TempRole::Outside, t, true, TempSource::Dallas, now, DALLAS_IO0_PIN, rom);
      else setCache(TempRole::Outside, NAN, false, TempSource::None, 0);
    }

    // Tank roles + optional DHW tank fallback (GPIO3)
    {
      float t; uint64_t rom;

      // DHW tank fallback: explicit ROM only, no auto-pick to avoid using a wrong sensor.
      uint64_t cfg = cfgRomForRole(TempRole::DhwTank);
      bool ok = false;
      if (cfg) ok = pickDallasByRom(DALLAS_TANK_PIN, cfg, t, rom);
      setCache(TempRole::DhwTank, ok ? t : NAN, ok, TempSource::Dallas, now, DALLAS_TANK_PIN, ok ? rom : 0);

      const uint64_t cfgTop = cfgRomForRole(TempRole::TankTop);
      const uint64_t cfgMid = cfgRomForRole(TempRole::TankMid);
      const uint64_t cfgBottom = cfgRomForRole(TempRole::TankBottom);

      const bool dupTop = false;
      const bool dupMid = cfgMid && (cfgMid == cfgTop);
      const bool dupBottom = cfgBottom && (cfgBottom == cfgTop || cfgBottom == cfgMid);

      std::vector<uint64_t> reservedRoms;
      if (cfgTop && !dupTop) reservedRoms.push_back(cfgTop);
      if (cfgMid && !dupMid && !containsRom(reservedRoms, cfgMid)) reservedRoms.push_back(cfgMid);
      if (cfgBottom && !dupBottom && !containsRom(reservedRoms, cfgBottom)) reservedRoms.push_back(cfgBottom);

      bool autoAkuAllowed = false;
      if (!cfgTop && !cfgMid && !cfgBottom) {
        const std::vector<uint64_t> validTankRoms = collectValidDallasRomsSorted(DALLAS_TANK_PIN);
        autoAkuAllowed = validTankRoms.size() >= 3;
      }

      // TankTop
      ok = false;
      if (cfgTop && !dupTop) ok = pickDallasByRom(DALLAS_TANK_PIN, cfgTop, t, rom);
      else if (autoAkuAllowed) ok = pickDallasAutoFromRemaining(DALLAS_TANK_PIN, reservedRoms, 0, t, rom);
      setCache(TempRole::TankTop, ok ? t : NAN, ok, TempSource::Dallas, now, DALLAS_TANK_PIN, ok ? rom : 0);

      // TankMid
      ok = false;
      if (cfgMid && !dupMid) ok = pickDallasByRom(DALLAS_TANK_PIN, cfgMid, t, rom);
      else if (autoAkuAllowed) ok = pickDallasAutoFromRemaining(DALLAS_TANK_PIN, reservedRoms, 1, t, rom);
      setCache(TempRole::TankMid, ok ? t : NAN, ok, TempSource::Dallas, now, DALLAS_TANK_PIN, ok ? rom : 0);

      // TankBottom
      ok = false;
      if (cfgBottom && !dupBottom) ok = pickDallasByRom(DALLAS_TANK_PIN, cfgBottom, t, rom);
      else if (autoAkuAllowed) ok = pickDallasAutoFromRemaining(DALLAS_TANK_PIN, reservedRoms, 2, t, rom);
      setCache(TempRole::TankBottom, ok ? t : NAN, ok, TempSource::Dallas, now, DALLAS_TANK_PIN, ok ? rom : 0);
    }

    // Return DS fallback (GPIO2)
    {
      float t; uint64_t rom;
      uint64_t cfg = cfgRomForRole(TempRole::Return);
      bool ok = false;
      if (cfg) ok = pickDallasByRom(DALLAS_RETURN_PIN, cfg, t, rom);
      if (!ok) ok = pickDallasFirstValid(DALLAS_RETURN_PIN, t, rom);
      // We store DS return in cache, but Return role prefers OT if available.
      if (ok) {
        // use a dedicated cache slot? we reuse Return slot only when OT not available during update.
        // handled in updateOpenTherm.
        // keep it in a temp variable by storing as Dallas but may be overwritten by OT.
        // We'll keep it as Dallas for now; OT update will override if valid.
        // ageMs is computed on get().
        //
        // Note: gpio/rom are set for diagnostics.
      }
      // Save into Return slot as Dallas for now.
      setCache(TempRole::Return, ok ? t : NAN, ok, TempSource::Dallas, now, DALLAS_RETURN_PIN, ok ? rom : 0);
    }

    // DHW return (GPIO1)
    {
      float t; uint64_t rom;
      uint64_t cfg = cfgRomForRole(TempRole::DhwReturn);
      bool ok = false;
      if (cfg) ok = pickDallasByRom(DALLAS_DHW_RETURN_PIN, cfg, t, rom);
      if (!ok) ok = pickDallasFirstValid(DALLAS_DHW_RETURN_PIN, t, rom);
      setCache(TempRole::DhwReturn, ok ? t : NAN, ok, TempSource::Dallas, now, DALLAS_DHW_RETURN_PIN, ok ? rom : 0);
    }
  }

  static inline void updateOpenTherm(uint32_t now) {
    OpenThermStatusSnapshot ot = openthermGetStatus();

    // Flow + DHW
    if (ot.present && ot.ready && isfinite(ot.boilerTempC)) {
      setCache(TempRole::Flow, ot.boilerTempC, true, TempSource::OpenTherm, ot.lastUpdateMs);
    } else {
      setCache(TempRole::Flow, NAN, false, TempSource::None, 0);
    }

    // DHW tank: OpenTherm has priority. If OT value is not valid, keep Dallas fallback from updateDallasRoles().
    if (ot.present && ot.ready && isfinite(ot.dhwTempC)) {
      setCache(TempRole::DhwTank, ot.dhwTempC, true, TempSource::OpenTherm, ot.lastUpdateMs);
    }

    // Outside (OT preferred)
    if (ot.present && ot.ready && isfinite(ot.outsideTempC)) {
      setCache(TempRole::Outside, ot.outsideTempC, true, TempSource::OpenTherm, ot.lastUpdateMs);
    } else {
      // keep for BLE update
    }

    // Return (OT preferred). If OT return unavailable, keep Dallas (already set by updateDallasRoles).
    if (ot.present && ot.ready && isfinite(ot.returnTempC)) {
      setCache(TempRole::Return, ot.returnTempC, true, TempSource::OpenTherm, ot.lastUpdateMs);
    }
  }

  static inline void updateBle(uint32_t now) {
    // Only used for Outside fallback.
    // Priority: OT > DS > BLE
    CacheItem &outside = g_cache[(uint8_t)TempRole::Outside];
    if (outside.valid && (outside.src == TempSource::OpenTherm || outside.src == TempSource::Dallas)) return;

    BleMeteoData m = bleGetMeteo();
    if (m.valid && isfinite(m.tempC) && m.lastUpdateMs > 0) {
      setCache(TempRole::Outside, m.tempC, true, TempSource::Ble, m.lastUpdateMs);
    } else {
      setCache(TempRole::Outside, NAN, false, TempSource::None, 0);
    }
  }
}

namespace TemperatureManager {

  namespace {
    bool s_lastDallasEnabled = false;
    bool s_haveDallasSnapshot = false;
    uint64_t s_lastRoleRoms[(uint8_t)TempRole::COUNT] = {};

    bool dallasRoleConfigChanged() {
      const bool enabled = ConfigStore::getDallasEnabled();
      bool changed = !s_haveDallasSnapshot || (enabled != s_lastDallasEnabled);
      for (uint8_t i = 0; i < (uint8_t)TempRole::COUNT; i++) {
        const TempRole role = (TempRole)i;
        if (!roleCanUseDallas(role)) continue;
        const uint64_t rom = cfgRomForRole(role);
        if (!s_haveDallasSnapshot || s_lastRoleRoms[i] != rom) changed = true;
        s_lastRoleRoms[i] = rom;
      }
      s_lastDallasEnabled = enabled;
      s_haveDallasSnapshot = true;
      return changed;
    }
  }

  void begin() {
    if (g_inited) return;
    g_inited = true;
    for (uint8_t i = 0; i < (uint8_t)TempRole::COUNT; i++) g_cache[i] = CacheItem{};
  }

  void loop() {
    begin();
    const uint32_t now = millis();
    if (dallasRoleConfigChanged()) invalidateDallasBackedRoles();
    // Update Dallas first (so OT can override Return if present)
    updateDallasRoles(now);
    updateOpenTherm(now);
    updateBle(now);
  }

  TempValue get(TempRole role, uint32_t maxAgeMs) {
    begin();
    const CacheItem &it = g_cache[(uint8_t)role];
    TempValue out;
    if (!it.valid) return out;
    const uint32_t now = millis();
    uint32_t age = (it.updatedMs > 0) ? (uint32_t)(now - it.updatedMs) : 0;
    if (age > maxAgeMs) return out;
    out.c = it.c;
    out.valid = true;
    out.src = it.src;
    out.ageMs = age;
    out.gpio = it.gpio;
    out.rom = it.rom;
    return out;
  }

  TempValue getMixFeedback(uint32_t maxAgeMs) {
    begin();
    TempValue out;
    float t = NAN;
    uint64_t rom = 0;
    if (!resolveDallasRoleValue(TempRole::Return, t, rom) || !isfinite(t)) return out;

    const DallasGpioStatus* st = DallasController::getStatus(DALLAS_RETURN_PIN);
    if (!st || st->lastReadMs == 0) return out;

    const uint32_t now = millis();
    const uint32_t age = (now >= st->lastReadMs) ? (uint32_t)(now - st->lastReadMs) : 0;
    if (age > maxAgeMs) return out;

    out.c = t;
    out.valid = true;
    out.src = TempSource::Dallas;
    out.ageMs = age;
    out.gpio = DALLAS_RETURN_PIN;
    out.rom = rom;
    return out;
  }

  const char* roleName(TempRole role) {
    const RoleBindingDef* def = findRoleBinding(role);
    return def ? def->key : "?";
  }

  const char* roleLabel(TempRole role) {
    const RoleBindingDef* def = findRoleBinding(role);
    return def ? def->label : "?";
  }

  bool parseRole(const String& s, TempRole& out) {
    String k = s; k.trim();
    k.toLowerCase();
    k.replace(" ", "");
    k.replace("-", "_");

    if (k == "flow") { out = TempRole::Flow; return true; }
    if (k == "dhw" || k == "dhw_tank") { out = TempRole::DhwTank; return true; }
    if (k == "outside") { out = TempRole::Outside; return true; }
    if (k == "return" || k == "returntempc" ||
        k == "return.flow" || k == "return_flow" || k == "returnflow" ||
        k == "flow.return" || k == "flow_return" || k == "flowc.return" || k == "flowc_return" ||
        k == "flowreturn" || k == "flowreturnc" ||
        k == "zpatecka" || k == "zpátečka" ||
        k == "teplotazasmesovacem" || k == "teplota_za_smesovacem") {
      out = TempRole::Return;
      return true;
    }
    if (k == "tank_top" || k == "tank1") { out = TempRole::TankTop; return true; }
    if (k == "tank_mid" || k == "tank2") { out = TempRole::TankMid; return true; }
    if (k == "tank_bottom" || k == "tank3") { out = TempRole::TankBottom; return true; }
    if (k == "dhw_return") { out = TempRole::DhwReturn; return true; }
    return false;
  }

  const DallasRoleBindingInfo* getDallasRoleBindings(size_t& count) {
    count = sizeof(kRoleBindings) / sizeof(kRoleBindings[0]);
    return reinterpret_cast<const DallasRoleBindingInfo*>(kRoleBindings);
  }

  uint64_t getRoleRom(TempRole role) {
    return cfgRomForRole(role);
  }

  void setRoleRom(TempRole role, uint64_t rom) {
    setCfgRomForRole(role, rom);
    if (roleCanUseDallas(role)) invalidateDallasBackedRoles();
    else invalidateRole(role);
  }

  void invalidateRole(TempRole role) {
    begin();
    clearRoleCache(role);
  }

  void invalidateAll() {
    begin();
    for (uint8_t i = 0; i < (uint8_t)TempRole::COUNT; i++) clearRoleCache((TempRole)i);
  }

  void invalidateDallasBackedRoles() {
    begin();
    clearDallasBackedRoleCaches();
  }

  String romToHex(uint64_t rom) {
    if (!rom) return String("");
    char buf[24];
    snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)rom);
    return String(buf);
  }

  bool parseRomHex(const String& s, uint64_t& out) {
    // Accept separators; take first 16 hex digits.
    uint64_t v = 0;
    uint8_t n = 0;
    for (size_t i = 0; i < s.length(); i++) {
      char c = s[i];
      int h = -1;
      if (c >= '0' && c <= '9') h = c - '0';
      else if (c >= 'A' && c <= 'F') h = 10 + (c - 'A');
      else if (c >= 'a' && c <= 'f') h = 10 + (c - 'a');
      if (h < 0) continue;
      v = (v << 4) | (uint64_t)h;
      n++;
      if (n == 16) break;
    }
    if (n != 16) return false;
    out = v;
    return true;
  }

  void fillTempsJson(JsonObject out) {
    for (const auto &r : kRoleBindings) {
      const char* key = (r.role == TempRole::DhwTank) ? "dhw" : r.key;
      TempValue v = get(r.role, 600000);
      if (v.valid) out[key] = v.c; else out[key] = nullptr;
      // sources
      String sk = String(key) + "Src";
      const char* src = nullptr;
      switch (v.src) {
        case TempSource::OpenTherm: src = "opentherm"; break;
        case TempSource::Dallas: src = "dallas"; break;
        case TempSource::Ble: src = "ble"; break;
        default: src = nullptr; break;
      }
      out[sk] = src;

      if (r.role == TempRole::DhwTank) {
        if (v.valid) out["dhw_tank"] = v.c; else out["dhw_tank"] = nullptr;
        out["dhw_tankSrc"] = src;
      }

      if (r.role == TempRole::Return) {
        if (v.valid) {
          out["returnTempC"] = v.c;
          out["flowReturnC"] = v.c;
          out["returnFlowC"] = v.c;
          out["return.flow"] = v.c;
        } else {
          out["returnTempC"] = nullptr;
          out["flowReturnC"] = nullptr;
          out["returnFlowC"] = nullptr;
          out["return.flow"] = nullptr;
        }
        out["returnTempSrc"] = src;
        out["flowReturnSrc"] = src;
        out["returnFlowSrc"] = src;
        out["return.flowSrc"] = src;

        const TempValue mix = getMixFeedback(600000);
        const char* mixSrc = nullptr;
        switch (mix.src) {
          case TempSource::OpenTherm: mixSrc = "opentherm"; break;
          case TempSource::Dallas: mixSrc = "dallas"; break;
          case TempSource::Ble: mixSrc = "ble"; break;
          default: mixSrc = nullptr; break;
        }
        if (mix.valid) out["afterMixC"] = mix.c; else out["afterMixC"] = nullptr;
        out["afterMixSrc"] = mixSrc;
      }
    }

    // Dallas ROM diagnostics
    JsonObject roms = out.createNestedObject("rom");
    for (const auto &r : kRoleBindings) {
      const char* key = (r.role == TempRole::DhwTank) ? "dhw" : r.key;
      TempValue v = get(r.role, 600000);
      if (v.src == TempSource::Dallas && v.rom) roms[key] = romToHex(v.rom);
      else roms[key] = nullptr;
      if (r.role == TempRole::DhwTank) {
        if (v.src == TempSource::Dallas && v.rom) roms["dhw_tank"] = romToHex(v.rom);
        else roms["dhw_tank"] = nullptr;
      }
    }
  }

  void fillDallasJson(JsonObject out) {
    out["enabled"] = ConfigStore::getDallasEnabled();

    JsonObject roles = out.createNestedObject("roles");
    JsonArray available = out.createNestedArray("availableRoles");
    for (const auto &def : kRoleBindings) {
      if (!def.assignable) continue;
      uint64_t rom = cfgRomForRole(def.role);
      TempValue v = (def.role == TempRole::Return) ? getMixFeedback(600000) : get(def.role, 600000);
      const char* src = nullptr;
      switch (v.src) {
        case TempSource::OpenTherm: src = "opentherm"; break;
        case TempSource::Dallas: src = "dallas"; break;
        case TempSource::Ble: src = "ble"; break;
        default: src = nullptr; break;
      }

      JsonObject r = roles.createNestedObject(def.key);
      r["label"] = def.label;
      r["gpio"] = (int)def.gpio;
      r["rom"] = rom ? romToHex(rom) : String("");
      if (v.valid) r["currentC"] = v.c; else r["currentC"] = nullptr;
      r["currentSrc"] = src;
      r["ageMs"] = v.valid ? (uint32_t)v.ageMs : 0;
      if (v.valid && v.gpio != 255) r["resolvedGpio"] = (int)v.gpio; else r["resolvedGpio"] = nullptr;
      if (v.src == TempSource::Dallas && v.rom) r["resolvedRom"] = romToHex(v.rom); else r["resolvedRom"] = nullptr;

      JsonObject a = available.createNestedObject();
      a["key"] = def.key;
      a["label"] = def.label;
      a["gpio"] = (int)def.gpio;
    }

    // Devices per bus
    JsonArray buses = out.createNestedArray("buses");
    uint8_t gpios[] = {DALLAS_IO0_PIN, DALLAS_DHW_RETURN_PIN, DALLAS_RETURN_PIN, DALLAS_TANK_PIN};
    for (uint8_t gpio : gpios) {
      const DallasGpioStatus* st = DallasController::getStatus(gpio);
      JsonObject b = buses.createNestedObject();
      b["gpio"] = (int)gpio;
      b["status"] = st ? (int)st->status : (int)TEMP_STATUS_DISABLED;
      b["lastReadMs"] = st ? (uint32_t)st->lastReadMs : 0;
      JsonArray devs = b.createNestedArray("devs");
      if (st) {
        // stable order
        std::vector<DallasDeviceInfo> dv = st->devices;
        std::sort(dv.begin(), dv.end(), [](const DallasDeviceInfo& a, const DallasDeviceInfo& b){ return a.rom < b.rom; });
        for (auto &d : dv) {
          JsonObject dd = devs.createNestedObject();
          dd["rom"] = romToHex(d.rom);
          if (d.valid && isfinite(d.temperature)) dd["c"] = d.temperature; else dd["c"] = nullptr;
          dd["ok"] = (bool)(d.valid && isfinite(d.temperature));
        }
      }
    }
  }
}
