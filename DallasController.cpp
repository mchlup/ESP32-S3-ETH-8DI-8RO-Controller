#include "DallasController.h"

// Local copy of https://github.com/junkfix/esp32-ds18b20 (RMT-based OneWire)
#include "OneWireESP32.h"

// Ensure 1-Wire line has a pull-up even if the external resistor is missing.
// (External ~4.7k to 3V3 is still strongly recommended for reliability.)
#include <driver/gpio.h>
#include <memory>
#include <new>

// NOTE:
// For runtime stability we keep persistent OneWire32 instances per GPIO.
// This avoids repeated new/delete and heap/RMT churn in the fast path.

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
std::unique_ptr<OneWire32> s_oneWire[GPIO_MAX + 1];

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
    if (!s_oneWire[gpio]) {
        s_oneWire[gpio].reset(new (std::nothrow) OneWire32(gpio));
    }
    return s_oneWire[gpio].get();
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
    g_bus[gpio].status = (type == TEMP_INPUT_NONE) ? TEMP_STATUS_DISABLED : TEMP_STATUS_NO_SENSOR;
    clearBus(gpio);

    if (type == TEMP_INPUT_NONE) return;

    if (type == TEMP_INPUT_AUTO) {
        autodetect(gpio);
    } else if (type == TEMP_INPUT_DALLAS) {
        if (!probeAndDiscover(gpio)) {
            g_bus[gpio].status = TEMP_STATUS_ERROR;
        }
    }
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
            if (now - g_bus[gpio].lastDiscoverMs >= 3000) {
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
  bool    enabled = false;
  uint8_t gpio    = 255;
  String  addr;   // optional (hex), if empty -> first valid device on bus
};

DallasInputCfg s_cfg[INPUT_COUNT];
bool s_inited = false;

String normHex(String s) {
  s.toUpperCase();
  String out;
  out.reserve(16);
  for (size_t i=0;i<s.length();i++){
    char c=s[i];
    bool isHex = (c>='0'&&c<='9')||(c>='A'&&c<='F');
    if (isHex) out += c;
  }
  return out;
}

String romToHex(uint64_t rom){
  char buf[17];
  for(int i=0;i<16;i++) buf[i]='0';
  buf[16]='\0';
  for(int b=0;b<8;b++){
    uint8_t v = (rom >> (56 - 8*b)) & 0xFF;
    const char* hex="0123456789ABCDEF";
    buf[b*2]   = hex[(v>>4)&0xF];
    buf[b*2+1] = hex[v&0xF];
  }
  return String(buf);
}

bool pickDeviceTemp(uint8_t gpio, const String& addrNorm, float& outTemp){
  const DallasGpioStatus* st = DallasController::getStatus(gpio);
  if (!st) return false;
  if (st->devices.empty()) return false;

  if (addrNorm.length() == 0){
    for (auto &d : st->devices){
      if (d.valid && isfinite(d.temperature)){
        outTemp = d.temperature;
        return true;
      }
    }
    return false;
  }

  for (auto &d : st->devices){
    if (!d.valid || !isfinite(d.temperature)) continue;
    if (romToHex(d.rom) == addrNorm){
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
    s_cfg[i].addr = "";
  }

  // --- Header thermometers (GPIO0..3) ---
  // Pokud UI obsahuje dallasNames/dallasAddrs, bereme DS18B20 na pin-headeru jako
  // nezávislé teploměry mapované 1:1 na TEMP1..TEMP4 (GPIO0..3).
  // DŮLEŽITÉ: tato logika nesmí zablokovat další mapování (iofunc.inputs[].role==temp_dallas).
  const bool headerMode = cfg.containsKey("dallasNames") || cfg.containsKey("dallasAddrs");
  if (headerMode) {
    JsonArrayConst addrs = (cfg.containsKey("dallasAddrs") && cfg["dallasAddrs"].is<JsonArrayConst>())
                             ? cfg["dallasAddrs"].as<JsonArrayConst>()
                             : JsonArrayConst();
    for (uint8_t i=0;i<=3;i++){
      String addr = "";
      if (!addrs.isNull() && i < addrs.size()) addr = String((const char*)(addrs[i] | ""));
      s_cfg[i].enabled = true;
      s_cfg[i].gpio = i;
      s_cfg[i].addr = normHex(addr);
    }
  }

  // --- temp_dallas mapování přes iofunc.inputs[] (legacy + doplňkové teploměry) ---

  bool usedGpio[4] = {false,false,false,false};
  if (cfg.containsKey("iofunc") && cfg["iofunc"].is<JsonObjectConst>()){
    JsonObjectConst iof = cfg["iofunc"].as<JsonObjectConst>();
    if (iof.containsKey("inputs") && iof["inputs"].is<JsonArrayConst>()){
      JsonArrayConst inputs = iof["inputs"].as<JsonArrayConst>();
      uint8_t idx=0;
      for (JsonVariantConst v : inputs){
        if (idx >= INPUT_COUNT) break;
        if (!v.is<JsonObjectConst>()) { idx++; continue; }

        // TEMP1..TEMP4 jsou v headerMode rezervované pro pevné mapování GPIO0..3.
        if (headerMode && idx <= 3) { idx++; continue; }

        JsonObjectConst o = v.as<JsonObjectConst>();
        const char* role = o["role"] | "none";

        if (strcmp(role, "temp_dallas") == 0){
          JsonObjectConst p = o["params"].is<JsonObjectConst>() ? o["params"].as<JsonObjectConst>() : JsonObjectConst();
          uint8_t gpio = (uint8_t)(p["gpio"] | 0);
          String addr = String((const char*)(p["addr"] | ""));
          addr = normHex(addr);

          if (gpio <= 3){
            s_cfg[idx].enabled = true;
            s_cfg[idx].gpio = gpio;
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
  filter["dallasNames"][0] = true;
  filter["dallasAddrs"][0] = true;
  filter["iofunc"]["inputs"][0]["role"] = true;
  filter["iofunc"]["inputs"][0]["params"]["gpio"] = true;
  filter["iofunc"]["inputs"][0]["params"]["addr"] = true;
  filter["cfg"]["dallasNames"][0] = true;
  filter["cfg"]["dallasAddrs"][0] = true;
  filter["cfg"]["iofunc"]["inputs"][0]["role"] = true;
  filter["cfg"]["iofunc"]["inputs"][0]["params"]["gpio"] = true;
  filter["cfg"]["iofunc"]["inputs"][0]["params"]["addr"] = true;

  StaticJsonDocument<1024> doc;
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
      return pickDeviceTemp(inputIndex, "", t);
    }
    return false;
  }
  float t;
  return pickDeviceTemp(s_cfg[inputIndex].gpio, s_cfg[inputIndex].addr, t);
}

float dallasGetTempC(uint8_t inputIndex){
  if (inputIndex >= INPUT_COUNT) return NAN;
  float t = NAN;
  if (!s_cfg[inputIndex].enabled){
    if (inputIndex <= 3){
      pickDeviceTemp(inputIndex, "", t);
      return t;
    }
    return NAN;
  }
  pickDeviceTemp(s_cfg[inputIndex].gpio, s_cfg[inputIndex].addr, t);
  return t;
}
