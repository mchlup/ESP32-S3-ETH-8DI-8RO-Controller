#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Central temperature registry.
// Goal: every subsystem (console, web UI, logic) reads temperatures consistently
// via roles, regardless of source (OpenTherm / DS18B20 / BLE).

enum class TempRole : uint8_t {
  // Boiler / system (preferred: OpenTherm)
  Flow = 0,      // flow/boiler temperature
  DhwTank,       // DHW tank temperature (from boiler)
  Outside,       // outside temperature (OT preferred, BLE fallback)
  Return,        // return temperature (OT preferred, DS fallback on GPIO2)

  // Storage tank (DS18B20 on GPIO3)
  TankTop,
  TankMid,
  TankBottom,

  // DHW circulation return (DS18B20 on GPIO1)
  DhwReturn,

  COUNT
};

enum class TempSource : uint8_t {
  None = 0,
  OpenTherm,
  Dallas,
  Ble
};

struct TempValue {
  float c = NAN;
  bool valid = false;
  TempSource src = TempSource::None;
  uint32_t ageMs = 0;

  // Dallas diagnostics (if src == Dallas)
  uint8_t gpio = 255;
  uint64_t rom = 0;
};

namespace TemperatureManager {
  void begin();
  void loop();

  // Get the best available value for a role.
  TempValue get(TempRole role, uint32_t maxAgeMs = 600000);

  // Dedicated Dallas-only return temperature on GPIO2. This value is used for
  // hydraulic port B and is never overridden by the OpenTherm return value.
  TempValue getDallasReturn(uint32_t maxAgeMs = 600000);

  // Backward-compatible alias retained for existing API fields/older UI code.
  TempValue getMixFeedback(uint32_t maxAgeMs = 600000);

  struct SelectableSourceInfo {
    const char* key;
    const char* label;
  };

  // Resolve a configured source key to a live temperature:
  // tank_mid, return_dallas, opentherm_ch, or none.
  // return_dallas always reads the dedicated DS18B20 Return role and is never
  // overridden by the OpenTherm return value.
  TempValue getBySourceKey(const String& key, uint32_t maxAgeMs = 600000);
  String normalizeSourceKey(const String& key, const char* fallback = "none");
  const SelectableSourceInfo* getSelectableSourcesForPort(const char* port, size_t& count);

  // Helpers for UI/API.
  const char* roleName(TempRole role);
  const char* roleLabel(TempRole role);
  bool parseRole(const String& s, TempRole& out);

  struct DallasRoleBindingInfo {
    TempRole role;
    const char* key;
    const char* label;
    uint8_t gpio;
    bool assignable;
  };

  const DallasRoleBindingInfo* getDallasRoleBindings(size_t& count);

  // Dallas role mapping (ROM=0 => AUTO).
  uint64_t getRoleRom(TempRole role);
  void setRoleRom(TempRole role, uint64_t rom);

  // Cache invalidation helpers. Useful after config/source changes.
  void invalidateRole(TempRole role);
  void invalidateAll();
  void invalidateDallasBackedRoles();

  String romToHex(uint64_t rom);
  bool parseRomHex(const String& s, uint64_t& out);

  // JSON helpers
  void fillTempsJson(JsonObject out);
  void fillDallasJson(JsonObject out);
}
