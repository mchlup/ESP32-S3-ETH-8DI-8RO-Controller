#include "DallasController.h"

// Local copy of https://github.com/junkfix/esp32-ds18b20 (RMT-based OneWire)
#include "OneWireESP32.h"

// Ensure 1-Wire line has a pull-up even if the external resistor is missing.
// (External ~4.7k to 3V3 is still strongly recommended for reliability.)
#include <driver/gpio.h>
#include <memory>
#include <new>
#include <type_traits>

// NOTE:
// We keep exactly one active OneWire32 instance at a time (RMT channels are limited and
// other peripherals (e.g. WS2812) may also use them). To reduce heap fragmentation we
// avoid allocating/deallocating the OneWire32 object itself; we reuse static storage
// and only recreate the internal RMT driver when switching GPIO.

namespace {

constexpr uint8_t  GPIO_MIN = 0;
constexpr uint8_t  GPIO_MAX = 3;
constexpr uint8_t  MAX_DEVICES_PER_GPIO = 8;

constexpr uint32_t CONVERSION_MS = 800;   // DS18B20 12-bit: 750ms typ.
constexpr uint32_t CYCLE_MS      = 1500;  // request -> read cadence

struct InternalDallas {
    uint8_t gpio = 255;
    TempInputType type = TEMP_INPUT_NONE;
    TempInputStatus status = TEMP_STATUS_DISABLED;

    std::vector<DallasDeviceInfo> devices;

    // async state (per GPIO)
    bool converting = false;
    uint32_t convertStartMs = 0;
    uint32_t lastCycleMs = 0;
    uint32_t lastDiscoverMs = 0;
    uint32_t lastReadMs = 0;
};

InternalDallas g_bus[GPIO_MAX + 1];
static std::aligned_storage_t<sizeof(OneWire32), alignof(OneWire32)> s_oneWireStorage;
static OneWire32* s_oneWire = nullptr;
static uint8_t s_oneWireGpio = 255;

static void destroyOneWire() {
    if (!s_oneWire) return;
    // OneWire32::cleanup() is private (performed by the destructor).
    // Since we construct OneWire32 with placement-new, explicitly calling
    // the destructor is the correct way to release the internal RMT driver.
    s_oneWire->~OneWire32();
    s_oneWire = nullptr;
    s_oneWireGpio = 255;
}

// Global scheduler: keep OneWire/RMT usage strictly sequential (1 GPIO at a time)
static uint8_t s_rrGpio = 0;
static bool    s_busy  = false;
static uint8_t s_activeGpio = 0;

static inline bool gpioSupportsDallas(uint8_t gpio) {
    return gpio >= GPIO_MIN && gpio <= GPIO_MAX;
}

static inline void ensureDallasPullup(uint8_t gpio) {
    // Internal pull-up helps in short runs / diagnostics, but is too weak for long cables.
    // We keep it enabled as a safety net.
    gpio_set_pull_mode((gpio_num_t)gpio, GPIO_PULLUP_ONLY);
}

static OneWire32* getOneWireBus(uint8_t gpio) {
    if (!gpioSupportsDallas(gpio)) return nullptr;

    if (s_oneWire && s_oneWireGpio == gpio) {
        return s_oneWire;
    }

    // Switch GPIO: cleanup + re-create driver in the same static storage.
    destroyOneWire();

    s_oneWire = new (&s_oneWireStorage) OneWire32(gpio);
    if (!s_oneWire->ready()) {
        destroyOneWire();
        return nullptr;
    }

    s_oneWireGpio = gpio;
    return s_oneWire;
}

static void clearBus(uint8_t gpio) {
    g_bus[gpio].devices.clear();
    g_bus[gpio].converting = false;
    g_bus[gpio].convertStartMs = 0;
    g_bus[gpio].lastCycleMs = 0;
    g_bus[gpio].lastDiscoverMs = 0;
    g_bus[gpio].lastReadMs = 0;
}

static void invalidateTemps(uint8_t gpio) {
    // Prevent "stale tempC" values from remaining in /api/dash when reads fail
    for (auto &dev : g_bus[gpio].devices) {
        dev.valid = false;
        dev.temperature = NAN;
    }
}

static bool probeAndDiscover(uint8_t gpio) {
    ensureDallasPullup(gpio);

    OneWire32* ow = getOneWireBus(gpio);
    if (!ow) return false;

    // reset() == false usually means "no presence" (no sensor), not a hard error.
    if (!ow->reset()) {
        g_bus[gpio].devices.clear();
        g_bus[gpio].status = TEMP_STATUS_NO_SENSOR;
        return true;
    }

    uint64_t addrs[MAX_DEVICES_PER_GPIO] = {0};
    uint8_t found = ow->search(addrs, MAX_DEVICES_PER_GPIO);

    g_bus[gpio].devices.clear();
    if (found == 0) {
        g_bus[gpio].status = TEMP_STATUS_NO_SENSOR;
        return true;
    }

    g_bus[gpio].devices.reserve(found);
    for (uint8_t i = 0; i < found; i++) {
        DallasDeviceInfo d{};
        d.rom = addrs[i];
        d.temperature = NAN;
        d.valid = false;
        g_bus[gpio].devices.push_back(d);
    }
    g_bus[gpio].status = TEMP_STATUS_OK;
    return true;
}

static void autodetect(uint8_t gpio) {
    if (!gpioSupportsDallas(gpio)) {
        g_bus[gpio].status = TEMP_STATUS_DISABLED;
        return;
    }

    // Try Dallas discovery
    if (!probeAndDiscover(gpio)) {
        g_bus[gpio].status = TEMP_STATUS_ERROR;
        return;
    }

    if (!g_bus[gpio].devices.empty()) {
        g_bus[gpio].type = TEMP_INPUT_DALLAS;
        g_bus[gpio].status = TEMP_STATUS_OK;
        return;
    }

    // If no Dallas sensors found, keep AUTO but report NO_SENSOR.
    g_bus[gpio].status = TEMP_STATUS_NO_SENSOR;
}

static bool startConversion(uint8_t gpio) {
    if (g_bus[gpio].devices.empty()) {
        g_bus[gpio].status = TEMP_STATUS_NO_SENSOR;
        return false;
    }

    ensureDallasPullup(gpio);
    OneWire32* ow = getOneWireBus(gpio);
    if (!ow) {
        g_bus[gpio].status = TEMP_STATUS_ERROR;
        invalidateTemps(gpio);
        return false;
    }

    if (!ow->reset()) {
        g_bus[gpio].status = TEMP_STATUS_NO_SENSOR;
        g_bus[gpio].devices.clear(); // drop stale ROMs after disconnect
        return false;
    }

    // Skip ROM + Convert T
    ow->write(0xCC);
    ow->write(0x44);

    g_bus[gpio].converting = true;
    g_bus[gpio].convertStartMs = millis();
    return true;
}

static void readTemperatures(uint8_t gpio) {
    if (g_bus[gpio].devices.empty()) {
        g_bus[gpio].status = TEMP_STATUS_NO_SENSOR;
        return;
    }

    ensureDallasPullup(gpio);
    OneWire32* ow = getOneWireBus(gpio);
    if (!ow) {
        g_bus[gpio].status = TEMP_STATUS_ERROR;
        invalidateTemps(gpio);
        return;
    }

    bool anyOk = false;
    for (auto &dev : g_bus[gpio].devices) {
        float t = NAN;
        uint64_t addr = dev.rom;
        uint8_t err = ow->getTemp(addr, t);
        if (err == 0 && isfinite(t)) {
            dev.temperature = t;
            dev.valid = true;
            anyOk = true;
        } else {
            dev.valid = false;
            dev.temperature = NAN; // IMPORTANT: prevent stale tempC in JSON
        }
    }

    g_bus[gpio].lastReadMs = millis();
    g_bus[gpio].status = anyOk ? TEMP_STATUS_OK : TEMP_STATUS_ERROR;
}

} // namespace

bool DallasController::gpioSupportsDallas(uint8_t gpio) {
    return ::gpioSupportsDallas(gpio);
}

void DallasController::begin() {
    destroyOneWire();
    for (uint8_t i = GPIO_MIN; i <= GPIO_MAX; i++) {
        g_bus[i].gpio = i;
        g_bus[i].type = TEMP_INPUT_NONE;
        g_bus[i].status = TEMP_STATUS_DISABLED;
        clearBus(i);
    }
    s_busy = false;
    s_rrGpio = 0;
    s_activeGpio = 0;
}

void DallasController::configureGpio(uint8_t gpio, TempInputType type) {
    if (!gpioSupportsDallas(gpio)) return;

    g_bus[gpio].type = type;
    clearBus(gpio);

    if (type == TEMP_INPUT_NONE) {
        g_bus[gpio].status = TEMP_STATUS_DISABLED;
        return;
    }

    // IMPORTANT:
    // Do NOT run RMT/OneWire discovery inside configure. This function is called during boot
    // (setup + config apply) and creating multiple RMT channels here can crash ESP32-S3.
    // Discovery/reading is handled non-blocking in DallasController::loop().
    g_bus[gpio].status = TEMP_STATUS_NO_SENSOR;
    g_bus[gpio].lastDiscoverMs = 0; // 0 => discover ASAP in loop
    g_bus[gpio].lastCycleMs = 0;
    g_bus[gpio].converting = false;
    g_bus[gpio].convertStartMs = 0;
}

void DallasController::loop() {
    const uint32_t now = millis();

    // If converting on an active GPIO, only check completion/read for that GPIO.
    if (s_busy) {
        const uint8_t gpio = s_activeGpio;
        if (g_bus[gpio].converting) {
            if (now - g_bus[gpio].convertStartMs >= CONVERSION_MS) {
                readTemperatures(gpio);
                g_bus[gpio].converting = false;
                s_busy = false;
                s_rrGpio = (gpio + 1) % (GPIO_MAX + 1);
            }
        } else {
            s_busy = false;
        }
        return;
    }

    // Not busy: pick next eligible GPIO (round-robin)
    for (uint8_t step = 0; step <= GPIO_MAX; step++) {
        const uint8_t gpio = (s_rrGpio + step) % (GPIO_MAX + 1);

        if (g_bus[gpio].type == TEMP_INPUT_NONE) continue;
        if (!(g_bus[gpio].type == TEMP_INPUT_DALLAS || g_bus[gpio].type == TEMP_INPUT_AUTO)) continue;

        // If no devices, occasionally probe/discover (hot-plug + AUTO)
        if (g_bus[gpio].devices.empty() || g_bus[gpio].status == TEMP_STATUS_NO_SENSOR) {
            if (g_bus[gpio].lastDiscoverMs == 0 || now - g_bus[gpio].lastDiscoverMs >= 3000) {
                g_bus[gpio].lastDiscoverMs = now;
                if (!probeAndDiscover(gpio)) {
                    g_bus[gpio].status = TEMP_STATUS_ERROR;
                }
            }
        }

        if (g_bus[gpio].devices.empty()) continue;

        // Periodic cycle guard
        if (now - g_bus[gpio].lastCycleMs < CYCLE_MS) continue;
        g_bus[gpio].lastCycleMs = now;

        // Start conversion ONLY for this GPIO (sequential)
        if (startConversion(gpio)) {
            s_busy = true;
            s_activeGpio = gpio;
            s_rrGpio = (gpio + 1) % (GPIO_MAX + 1);
        }
        return;
    }
}

const DallasGpioStatus* DallasController::getStatus(uint8_t gpio) {
    if (!gpioSupportsDallas(gpio)) return nullptr;

    static DallasGpioStatus status;
    status.gpio = gpio;
    status.status = g_bus[gpio].status;
    status.devices = g_bus[gpio].devices;
    status.lastReadMs = g_bus[gpio].lastReadMs;
    return &status;
}

// ---------------- Legacy/simple API used across the project ----------------

namespace {

constexpr uint8_t INPUT_COUNT = 8;

struct DallasInputCfg {
  bool enabled = false;
  uint8_t gpio = 255;
  bool hasAddr = false;
  uint64_t addr = 0; // optional ROM (8B), if hasAddr==false -> first valid device on bus
};

DallasInputCfg s_cfg[INPUT_COUNT];
bool s_inited = false;

static inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  return -1;
}

// Parse ROM in hex (accepts separators; uses first 16 hex digits). Returns true if 16 nibbles were parsed.
static bool parseRomHex(const char* s, uint64_t &out) {
  if (!s) return false;
  uint64_t v = 0;
  uint8_t n = 0;
  for (; *s; s++) {
    int h = hexNibble(*s);
    if (h < 0) continue;
    v = (v << 4) | (uint64_t)h;
    n++;
    if (n == 16) break;
  }
  if (n != 16) return false;
  out = v;
  return true;
}

static bool pickDeviceTemp(uint8_t gpio, bool hasAddr, uint64_t addr, float& outTemp) {
  const DallasGpioStatus* st = DallasController::getStatus(gpio);
  if (!st) return false;
  if (st->devices.empty()) return false;

  if (!hasAddr) {
    for (auto &d : st->devices) {
      if (d.valid && isfinite(d.temperature)) {
        outTemp = d.temperature;
        return true;
      }
    }
    return false;
  }

  for (auto &d : st->devices) {
    if (!d.valid || !isfinite(d.temperature)) continue;
    if (d.rom == addr) {
      outTemp = d.temperature;
      return true;
    }
  }
  return false;
}

void applyCfgObj(const JsonObjectConst& root){
  JsonObjectConst cfg = root;
  if (root.containsKey("cfg") && root["cfg"].is<JsonObjectConst>()) cfg = root["cfg"].as<JsonObjectConst>();

  // reset mapping
  for (uint8_t i=0;i<INPUT_COUNT;i++){
    s_cfg[i].enabled = false;
    s_cfg[i].gpio = 255;
    s_cfg[i].hasAddr = false;
    s_cfg[i].addr = 0;
  }

  bool usedGpio[4] = {false,false,false,false};

  // --- TEMP1..TEMP8 mapping from UI (preferred) ---
  // UI can optionally provide:
  //   dallasGpios[0..7]  (0..3)
  //   dallasAddrs[0..7]  (ROM hex, empty => auto)
  // This enables multiple sensors on one GPIO and per-channel ROM selection.
  const bool hasDallasGpios = cfg.containsKey("dallasGpios") && cfg["dallasGpios"].is<JsonArrayConst>();
  JsonArrayConst mapGpios = hasDallasGpios ? cfg["dallasGpios"].as<JsonArrayConst>() : JsonArrayConst();

  JsonArrayConst mapAddrs = (cfg.containsKey("dallasAddrs") && cfg["dallasAddrs"].is<JsonArrayConst>())
                             ? cfg["dallasAddrs"].as<JsonArrayConst>()
                             : JsonArrayConst();

  const bool channelMapMode = hasDallasGpios || (!mapAddrs.isNull() && mapAddrs.size() >= INPUT_COUNT);

  // --- Header fallback (legacy) ---
  // If UI only provides dallasNames/dallasAddrs (length 4) and no dallasGpios,
  // keep backward compatibility: TEMP1..TEMP4 map 1:1 to GPIO0..GPIO3.
  const bool headerMode = !channelMapMode && (cfg.containsKey("dallasNames") || cfg.containsKey("dallasAddrs"));

  if (channelMapMode) {
    for (uint8_t i=0;i<INPUT_COUNT;i++){
      int gpio = -1;
      if (!mapGpios.isNull() && i < mapGpios.size()) {
        gpio = (int)(mapGpios[i] | -1);
      } else {
        // sensible defaults if only addrs are given
        gpio = (i <= 3) ? (int)i : -1;
      }
      bool hasAddr = false;
      uint64_t addr = 0;
      if (!mapAddrs.isNull() && i < mapAddrs.size()) {
        const char* as = (const char*)(mapAddrs[i] | "");
        hasAddr = parseRomHex(as, addr);
      }

      if (gpio >= 0 && gpio <= 3) {
        s_cfg[i].enabled = true;
        s_cfg[i].gpio = (uint8_t)gpio;
        s_cfg[i].hasAddr = hasAddr;
        s_cfg[i].addr = addr;
        usedGpio[gpio] = true;
      }
    }
  } else if (headerMode) {
    for (uint8_t i=0;i<=3;i++){
      bool hasAddr = false;
      uint64_t addr = 0;
      if (!mapAddrs.isNull() && i < mapAddrs.size()) {
        const char* as = (const char*)(mapAddrs[i] | "");
        hasAddr = parseRomHex(as, addr);
      }
      s_cfg[i].enabled = true;
      s_cfg[i].gpio = i;
      s_cfg[i].hasAddr = hasAddr;
      s_cfg[i].addr = addr;
      usedGpio[i] = true;
    }
  }

  // --- temp_dallas mapování přes iofunc.inputs[] (legacy / power users) ---
  // If configured, it overrides the per-channel mapping.
  if (cfg.containsKey("iofunc") && cfg["iofunc"].is<JsonObjectConst>()){
    JsonObjectConst iof = cfg["iofunc"].as<JsonObjectConst>();
    if (iof.containsKey("inputs") && iof["inputs"].is<JsonArrayConst>()){
      JsonArrayConst inputs = iof["inputs"].as<JsonArrayConst>();
      uint8_t idx=0;
      for (JsonVariantConst v : inputs){
        if (idx >= INPUT_COUNT) break;
        if (!v.is<JsonObjectConst>()) { idx++; continue; }

        JsonObjectConst o = v.as<JsonObjectConst>();
        const char* role = o["role"] | "none";

        if (strcmp(role, "temp_dallas") == 0){
          JsonObjectConst p = o["params"].is<JsonObjectConst>() ? o["params"].as<JsonObjectConst>() : JsonObjectConst();
          uint8_t gpio = (uint8_t)(p["gpio"] | 0);
          bool hasAddr = false;
          uint64_t addr = 0;
          const char* as = (const char*)(p["addr"] | "");
          hasAddr = parseRomHex(as, addr);

          if (gpio <= 3){
            s_cfg[idx].enabled = true;
            s_cfg[idx].gpio = gpio;
            s_cfg[idx].hasAddr = hasAddr;
            s_cfg[idx].addr = addr;
            usedGpio[gpio] = true;
          }
        }
        idx++;
      }
    }
  }

  // --- Konfigurace sběrnic GPIO0..3 ---
  // Vždy držíme AUTO (autodetekce + diagnostika). Pokud je GPIO explicitně používán,
  // přepneme ho do DALLAS režimu (rychlejší/stabilnější čtení).
  for (uint8_t gpio=0; gpio<=3; gpio++){
    DallasController::configureGpio(gpio, TEMP_INPUT_AUTO);
  }
  for (uint8_t gpio=0; gpio<=3; gpio++){
    if (usedGpio[gpio]) DallasController::configureGpio(gpio, TEMP_INPUT_DALLAS);
  }
}

} // namespace

void dallasApplyConfig(const String& jsonStr){
  if (!s_inited){
    DallasController::begin();
    for (uint8_t gpio=0; gpio<=3; gpio++){
      DallasController::configureGpio(gpio, TEMP_INPUT_AUTO);
    }
    s_inited = true;
  }

  StaticJsonDocument<256> filter;
  // IMPORTANT: for arrays, using [0] in ArduinoJson filter means "only first element".
  // We need the whole arrays (multiple sensors per GPIO + per-input ROM selection).
  filter["dallasNames"] = true;
  filter["dallasAddrs"] = true;
  filter["dallasGpios"] = true;
  filter["iofunc"]["inputs"] = true;
  filter["cfg"]["dallasNames"] = true;
  filter["cfg"]["dallasAddrs"] = true;
  filter["cfg"]["dallasGpios"] = true;
  filter["cfg"]["iofunc"]["inputs"] = true;

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, jsonStr, DeserializationOption::Filter(filter));
  if (err) return;
  applyCfgObj(doc.as<JsonObjectConst>());
}

bool dallasIsValid(uint8_t inputIndex){
  if (!s_inited){
    DallasController::begin();
    for (uint8_t gpio=0; gpio<=3; gpio++){
      DallasController::configureGpio(gpio, TEMP_INPUT_AUTO);
    }
    s_inited = true;
  }

  // Ensure Dallas loop progresses even if higher-level loop calls only legacy funcs
  DallasController::loop();

  if (inputIndex >= INPUT_COUNT) return false;
  if (!s_cfg[inputIndex].enabled) {
    if (inputIndex <= 3){
      float t;
      return pickDeviceTemp(inputIndex, false, 0, t);
    }
    return false;
  }
  float t;
  return pickDeviceTemp(s_cfg[inputIndex].gpio, s_cfg[inputIndex].hasAddr, s_cfg[inputIndex].addr, t);
}

float dallasGetTempC(uint8_t inputIndex){
  if (inputIndex >= INPUT_COUNT) return NAN;
  float t = NAN;
  if (!s_cfg[inputIndex].enabled){
    if (inputIndex <= 3){
      pickDeviceTemp(inputIndex, false, 0, t);
      return t;
    }
    return NAN;
  }
  pickDeviceTemp(s_cfg[inputIndex].gpio, s_cfg[inputIndex].hasAddr, s_cfg[inputIndex].addr, t);
  return t;
}
