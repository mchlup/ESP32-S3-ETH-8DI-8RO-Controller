// IMPORTANT: include Features.h first so FEATURE_BLE is visible before
// including the header (which provides stubs when the feature is disabled).
#include "Features.h"
#include "BleController.h"

#if defined(FEATURE_BLE)

#include "Log.h"

#include <ArduinoJson.h>

// Passive BLE advertisement listener (no pairing, no connections).
//
// Why this design:
//  - outdoor sensor broadcasts periodically -> we only need advertisements
//  - scanning is lower latency than GATT polling and usually more robust
//  - no bonding/pairing state to maintain

namespace {
  bool g_inited = false;
  bool g_enabled = true;
  String g_mac;          // optional filter
  // Supported:
  //  - "esp_meteo_mfg" : our ESP32-C3 outdoor sensor broadcasting manufacturer data
  //  - "atc_mitherm"   : common ATC MiThermometer custom firmware
  //  - "auto"          : try known decoders
  String g_type = "auto";
  uint32_t g_maxAgeMs = 10 * 60 * 1000UL;

  bool g_haveMeteo = false;
  float g_meteoC = NAN;
  int g_meteoHum = -1;
  int g_meteoPress = -1;
  int g_meteoTrend = 0;
  uint32_t g_lastUpdateMs = 0;
  int g_lastRssi = 0;

  // --- BLE Mesh (lightweight relay over advertisements) ---
  // NOTE: This is NOT Bluetooth SIG Mesh. It is a tiny relay protocol carried
  // in Manufacturer Data to extend outdoor sensor range.
  bool g_meshEnabled = false;
  bool g_meshRelay = false;
  uint8_t g_meshTtl = 3;
  uint32_t g_meshAdvIntervalMs = 1200;
  int g_meshMinRelayRssi = -95;
  bool g_meshPreferDirect = true;

  // Last packet meta
  bool g_lastFromMesh = false;
  String g_lastOriginMac = "";
  uint8_t g_lastHops = 0;
  uint8_t g_lastTtl = 0;
  uint16_t g_lastSeq = 0;

  struct SeenEntry {
    uint8_t mac[6];
    uint16_t seq;
    uint32_t ts;
  };
  SeenEntry g_seen[16];
  uint8_t g_seenWr = 0;

  static uint8_t crc8_xor(const uint8_t* p, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; ++i) c ^= p[i];
    return c;
  }

  static String mac6ToString(const uint8_t mac[6]) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
  }

  static bool seenBefore(const uint8_t mac[6], uint16_t seq, uint32_t nowMs) {
    // Expire entries after 60s to keep cache small and stable.
    const uint32_t kExp = 60 * 1000UL;
    for (auto& e : g_seen) {
      if (e.ts && (uint32_t)(nowMs - e.ts) > kExp) e.ts = 0;
      if (!e.ts) continue;
      if (e.seq != seq) continue;
      if (memcmp(e.mac, mac, 6) != 0) continue;
      return true;
    }
    return false;
  }

  static void rememberSeen(const uint8_t mac[6], uint16_t seq, uint32_t nowMs) {
    SeenEntry& e = g_seen[g_seenWr++ & 0x0F];
    memcpy(e.mac, mac, 6);
    e.seq = seq;
    e.ts = nowMs;
  }

  static String normalizeMac(const String& mac) {
    String m = mac;
    m.trim();
    m.toUpperCase();
    m.replace("-", ":");
    return m;
  }

  static bool isStale(uint32_t nowMs) {
    if (!g_haveMeteo) return true;
    if (!g_maxAgeMs) return false;
    return (uint32_t)(nowMs - g_lastUpdateMs) > g_maxAgeMs;
  }

  // Decode manufacturer data broadcasted by ESP32C3_BLE_MeteoSensor.ino:
  //   uint16 companyId (0xFFFF) LE
  //   uint8  version (1)
  //   int16  temp_x10 LE
  //   uint8  hum_pct
  //   uint16 press_hPa LE
  // Total 8 bytes.
  static bool decodeEspMeteoMfg(const uint8_t* data, size_t len,
                               float& outTempC, int& outHum, int& outPress, int& outTrend) {
    if (len < 8) return false;
    const uint16_t cid = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    if (cid != 0xFFFF) return false;
    const uint8_t ver = data[2];
    if (ver != 1) return false;

    const int16_t t10 = (int16_t)((uint16_t)data[3] | ((uint16_t)data[4] << 8));
    const uint8_t hum = data[5];
    const uint16_t press = (uint16_t)data[6] | ((uint16_t)data[7] << 8);

    outTempC = ((float)t10) / 10.0f;
    outHum = (int)hum;
    outPress = (int)press;
    outTrend = 0; // trend is not in adv, only in 6B notify frame

    if (!isfinite(outTempC) || outTempC < -60.0f || outTempC > 80.0f) return false;
    if (outHum < 0 || outHum > 100) return false;
    if (outPress < 300 || outPress > 1100) return false;
    return true;
  }

  // Decode lightweight BLE Mesh meteo frame (Manufacturer Data):
  //   uint16 companyId = 0xFFFE (LE)
  //   uint8  ver = 1
  //   uint8  ttl
  //   uint8  hops
  //   uint16 seq (LE)
  //   uint8  originMac[6]
  //   int16  temp_x10 (LE)
  //   uint8  hum_pct
  //   uint16 press_hPa (LE)
  //   [optional] uint8 crc8 (xor of all previous bytes)
  static bool decodeMeshMfg(const uint8_t* data, size_t len,
                            float& outTempC, int& outHum, int& outPress, int& outTrend,
                            uint8_t& outTtl, uint8_t& outHops, uint16_t& outSeq,
                            uint8_t outOriginMac[6]) {
    if (len < 18) return false;
    const uint16_t cid = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    if (cid != 0xFFFE) return false;
    const uint8_t ver = data[2];
    if (ver != 1) return false;

    outTtl = data[3];
    outHops = data[4];
    outSeq = (uint16_t)data[5] | ((uint16_t)data[6] << 8);
    memcpy(outOriginMac, &data[7], 6);
    const int16_t t10 = (int16_t)((uint16_t)data[13] | ((uint16_t)data[14] << 8));
    const uint8_t hum = data[15];
    const uint16_t press = (uint16_t)data[16] | ((uint16_t)data[17] << 8);

    // Optional CRC
    if (len >= 19) {
      const uint8_t want = data[18];
      const uint8_t got = crc8_xor(data, 18);
      if (want != got) return false;
    }

    outTempC = ((float)t10) / 10.0f;
    outHum = (int)hum;
    outPress = (int)press;
    outTrend = 0;

    if (!isfinite(outTempC) || outTempC < -60.0f || outTempC > 80.0f) return false;
    if (outHum < 0 || outHum > 100) return false;
    if (outPress < 300 || outPress > 1100) return false;
    return true;
  }

  // Decode ATC_MiThermometer custom firmware (very common):
  // Service Data UUID 0x181A:  [0..1]=?; [2..3]=temp*10 (int16 BE) ... varies by version
  // We'll support the common 13-byte format:
  //  [0]=0x16 [1]=0x1A [2]=0x18 then 10 bytes payload:
  //    mac(6) + temp(2, BE, *10) + hum(2, BE, *10) + batt(1)
  // But many stacks already provide pure service data; we handle both.
  static bool decodeATC(const uint8_t* data, size_t len, float& outTempC) {
    // Accept either "raw service data" (>= 10) or full AD structure.
    // Heuristic: if starts with 0x16 0x1A 0x18 then skip 3 bytes.
    const uint8_t* p = data;
    size_t n = len;
    if (n >= 3 && p[0] == 0x16 && p[1] == 0x1A && p[2] == 0x18) {
      p += 3;
      n -= 3;
    }
    if (n < 8) return false;

    // Locate temp field in common ATC payload: after 6 bytes MAC.
    const size_t off = 6;
    if (n < off + 2) return false;
    const int16_t t10 = (int16_t)((p[off] << 8) | p[off + 1]);
    outTempC = ((float)t10) / 10.0f;
    return isfinite(outTempC) && outTempC > -60.0f && outTempC < 80.0f;
  }
}

// Use NimBLE-Arduino (required for BLE scanning on ESP32).
// NOTE: Avoid __has_include(...) here; Arduino's library resolver may not add include paths
//       unless it sees a direct #include, which can incorrectly force "stub" mode.
#include <NimBLEDevice.h>

namespace {
  NimBLEScan* g_scan = nullptr;
  NimBLEAdvertising* g_adv = nullptr;
  bool g_advRunning = false;
  uint32_t g_lastAdvMs = 0;

  static void meshRelayIfEnabled(const uint8_t originMac[6], uint16_t seq,
                                uint8_t ttl, uint8_t hops,
                                float tC, int hum, int press) {
    if (!g_meshEnabled || !g_meshRelay) return;
    if (g_meshTtl == 0) return;
    if (ttl == 0) return;
    const uint32_t now = millis();
    if (g_meshAdvIntervalMs && (uint32_t)(now - g_lastAdvMs) < g_meshAdvIntervalMs) return;
    if (g_lastRssi < g_meshMinRelayRssi) return;
    if (seenBefore(originMac, seq, now)) return;
    rememberSeen(originMac, seq, now);

    // Build relayed manufacturer data (companyId 0xFFFE, ver=1)
    uint8_t payload[19];
    payload[0] = 0xFE; payload[1] = 0xFF;
    payload[2] = 1; // ver
    const uint8_t nextTtl = (uint8_t)(ttl - 1);
    payload[3] = nextTtl;
    payload[4] = (uint8_t)(hops + 1);
    payload[5] = (uint8_t)(seq & 0xFF);
    payload[6] = (uint8_t)((seq >> 8) & 0xFF);
    memcpy(&payload[7], originMac, 6);
    const int16_t t10 = (int16_t)lrintf(tC * 10.0f);
    payload[13] = (uint8_t)(t10 & 0xFF);
    payload[14] = (uint8_t)((t10 >> 8) & 0xFF);
    payload[15] = (uint8_t)constrain(hum, 0, 100);
    const uint16_t p = (uint16_t)constrain(press, 300, 1100);
    payload[16] = (uint8_t)(p & 0xFF);
    payload[17] = (uint8_t)((p >> 8) & 0xFF);
    payload[18] = crc8_xor(payload, 18);

    if (!g_adv) g_adv = NimBLEDevice::getAdvertising();

    NimBLEAdvertisementData ad;
    ad.setManufacturerData(std::string((const char*)payload, sizeof(payload)));
    g_adv->setAdvertisementData(ad);

    // Keep it simple: start advertising once and then just update payload.
    if (!g_advRunning) {
      g_adv->start();
      g_advRunning = true;
    }
    g_lastAdvMs = now;
  }

  // NimBLE-Arduino API: use NimBLEScanCallbacks (not NimBLEAdvertisedDeviceCallbacks)
  class ScanCallbacks : public NimBLEScanCallbacks {
   public:
    // NimBLE-Arduino versions differ in the exact callback signature.
    // Provide both overloads and avoid 'override' to stay compatible.
    void onResult(NimBLEAdvertisedDevice* dev) { handle((const NimBLEAdvertisedDevice*)dev); }
    void onResult(const NimBLEAdvertisedDevice* dev) { handle(dev); }

   private:
    void handle(const NimBLEAdvertisedDevice* dev) {
      if (!g_enabled) return;

      const int rssi = dev->getRSSI();

      float tC = NAN;
      int hum = -1;
      int press = -1;
      int trend = 0;
      bool ok = false;
      bool isMesh = false;
      uint8_t ttl = 0;
      uint8_t hops = 0;
      uint16_t seq = 0;
      uint8_t originMac[6] = {0};

      // Manufacturer data first (ESP meteo sensor broadcast lives here)
      if (dev->haveManufacturerData()) {
        std::string md = dev->getManufacturerData();
        // Mesh frames are carried in manufacturer data (0xFFFE)
        if (!ok && g_meshEnabled && (g_type == "esp_meteo_mfg" || g_type == "auto")) {
          ok = decodeMeshMfg((const uint8_t*)md.data(), md.size(), tC, hum, press, trend,
                             ttl, hops, seq, originMac);
          if (ok) isMesh = true;
        }
        if (!ok && (g_type == "esp_meteo_mfg" || g_type == "auto")) {
          ok = decodeEspMeteoMfg((const uint8_t*)md.data(), md.size(), tC, hum, press, trend);
        }
        if (!ok && (g_type == "atc_mitherm" || g_type == "auto")) {
          ok = decodeATC((const uint8_t*)md.data(), md.size(), tC);
        }
      }

      // Service Data (ATC, or other sensors)
      if (!ok && dev->haveServiceData()) {
        std::string sd = dev->getServiceData();
        if (g_type == "atc_mitherm" || g_type == "auto") {
          ok = decodeATC((const uint8_t*)sd.data(), sd.size(), tC);
        }
      }

      if (!ok) return;

      // Allowlist filter:
      //  - for mesh frames, compare against originMac
      //  - for legacy frames, compare against advertiser MAC
      if (g_mac.length()) {
        String m;
        if (isMesh) {
          m = mac6ToString(originMac);
        } else {
          const std::string a = dev->getAddress().toString();
          m = String(a.c_str());
          m.toUpperCase();
        }
        if (m != g_mac) return;
      }

      // Prefer direct frames over relayed frames for the same origin.
      if (g_meshPreferDirect && isMesh && hops > 0 && !g_lastFromMesh && g_lastOriginMac.length()) {
        if (g_lastOriginMac == mac6ToString(originMac) && !isStale(millis())) {
          return;
        }
      }

      g_meteoC = tC;
      g_meteoHum = hum;
      g_meteoPress = press;
      g_meteoTrend = trend;
      g_haveMeteo = true;
      g_lastUpdateMs = millis();
      g_lastRssi = rssi;

      // Meta (mesh)
      if (isMesh) {
        g_lastFromMesh = (hops > 0);
        g_lastOriginMac = mac6ToString(originMac);
        g_lastHops = hops;
        g_lastTtl = ttl;
        g_lastSeq = seq;

        // Opportunistically relay further (if enabled)
        meshRelayIfEnabled(originMac, seq, ttl, hops, tC, hum, press);
      } else {
        g_lastFromMesh = false;
        g_lastOriginMac = "";
        g_lastHops = 0;
        g_lastTtl = 0;
        g_lastSeq = 0;
      }
    }
  };

  ScanCallbacks g_cb;
}

void bleInit() {
  if (g_inited) return;
  g_inited = true;

  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  g_scan = NimBLEDevice::getScan();
  // (wantDuplicates=true) = deliver also duplicates; we filter ourselves.
  // This improves stability for sensors that advertise identical payloads for a while.
  g_scan->setScanCallbacks(&g_cb, true);

  // Passive scan: no scan requests, just listen to advertisements.
  g_scan->setActiveScan(false);
  // Use wider window/interval to reduce chance of missing slow advertisements.
  // NimBLE values are in 0.625ms units.
  g_scan->setInterval(160);
  g_scan->setWindow(80);
  // Do not rely on NimBLE duplicate filter (can drop valid updates). We filter in code.
  g_scan->setDuplicateFilter(false);
  g_scan->setMaxResults(0);

  // Start continuous scan (0 = forever)
  // NimBLE-Arduino signature: start(duration, isContinue=false, restart=true)
  g_scan->start(0, false, true);
  LOGI("BLE scan started (passive adv listener)");
}

void bleLoop() {
  // NimBLE scan is expected to run continuously in background.
  // On some builds it may stop unexpectedly (memory pressure / BT stack hiccup).
  // Keep it alive with a lightweight watchdog.
  static uint32_t lastKickMs = 0;
  const uint32_t now = millis();
  if ((uint32_t)(now - lastKickMs) < 2000UL) return;
  lastKickMs = now;

  if (!g_inited || !g_scan) return;
  if (!g_enabled) return;

  // Restart scan if it stopped.
  if (!g_scan->isScanning()) {
    g_scan->start(0, false, true);
    LOGW("BLE scan restarted by watchdog");
  }
}

bool bleIsScanning() {
  if (!g_inited || !g_scan) return false;
  return g_scan->isScanning();
}

static bool isStaleWithOverride(uint32_t nowMs, uint32_t maxAgeOverrideMs) {
  if (!g_haveMeteo) return true;
  const uint32_t maxAge = (maxAgeOverrideMs > 0) ? maxAgeOverrideMs : g_maxAgeMs;
  if (!maxAge) return false;
  return (uint32_t)(nowMs - g_lastUpdateMs) > maxAge;
}

bool bleGetMeteoTempCEx(float& outC, uint32_t maxAgeOverrideMs, uint32_t* ageMs) {
  if (!g_enabled) return false;
  if (!g_haveMeteo) return false;
  const uint32_t now = millis();
  const uint32_t age = (uint32_t)(now - g_lastUpdateMs);
  if (ageMs) *ageMs = age;
  if (isStaleWithOverride(now, maxAgeOverrideMs)) return false;
  outC = g_meteoC;
  return isfinite(outC);
}

bool bleGetTempCByIdEx(const String& id, float& outC, uint32_t maxAgeOverrideMs, uint32_t* ageMs) {
  // Supported IDs: "" (default), "meteo", "meteo.tempC"
  // Backward/robustness: treat unknown IDs as "meteo.tempC" (many UIs send "outdoor" etc.)
  if (id.length() == 0 || id == "meteo" || id == "meteo.tempC") {
    return bleGetMeteoTempCEx(outC, maxAgeOverrideMs, ageMs);
  }
  // Accept common aliases
  if (id == "outdoor" || id == "outdoor.tempC" || id == "tempC") {
    return bleGetMeteoTempCEx(outC, maxAgeOverrideMs, ageMs);
  }
  return false;
}


void bleApplyConfig(const String& json) {
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return;

  JsonObject ble = doc["ble"].as<JsonObject>();
  if (ble.isNull()) return;

  g_enabled = (bool)(ble["enabled"] | g_enabled);
  g_type = String((const char*)(ble["type"] | g_type.c_str()));
  g_mac = normalizeMac(String((const char*)(ble["mac"] | g_mac.c_str())));
  g_maxAgeMs = (uint32_t)(ble["maxAgeMs"] | g_maxAgeMs);

  // Mesh options: {"ble": {"mesh": {"enabled":true, "relay":true, "ttl":3,
  //                              "advIntervalMs":1200, "minRelayRssi":-95,
  //                              "preferDirect":true }}}
  JsonObject mesh = ble["mesh"].as<JsonObject>();
  if (!mesh.isNull()) {
    g_meshEnabled = (bool)(mesh["enabled"] | g_meshEnabled);
    g_meshRelay = (bool)(mesh["relay"] | g_meshRelay);
    g_meshTtl = (uint8_t)(mesh["ttl"] | g_meshTtl);
    g_meshAdvIntervalMs = (uint32_t)(mesh["advIntervalMs"] | g_meshAdvIntervalMs);
    g_meshMinRelayRssi = (int)(mesh["minRelayRssi"] | g_meshMinRelayRssi);
    g_meshPreferDirect = (bool)(mesh["preferDirect"] | g_meshPreferDirect);
  }

  // If user disabled relay, stop advertising to save IRAM/CPU.
  if ((!g_meshEnabled || !g_meshRelay) && g_advRunning && g_adv) {
    g_adv->stop();
    g_advRunning = false;
  }

  // If BLE already running and user disabled it, keep scan running but ignore results
  // (stopping/starting scan is possible but tends to be glitchy on some builds).
  if (g_enabled && g_inited == false) bleInit();

  LOGI("BLE cfg: enabled=%d mac=%s type=%s mesh(en=%d rel=%d ttl=%u)",
       (int)g_enabled, g_mac.c_str(), g_type.c_str(),
       (int)g_meshEnabled, (int)g_meshRelay, (unsigned)g_meshTtl);
}

bool bleGetTempCById(const String& id, float& outC) {
  // Keep compatibility with older callers.
  return bleGetTempCByIdEx(id, outC, 0, nullptr);
}


bool bleGetMeteoTempC(float& outC) {
  return bleGetMeteoTempCEx(outC, 0, nullptr);
}


bool bleGetMeteoReading(float& outTempC, int& outHumPct, int& outPressHpa, int& outTrend) {
  if (!g_enabled) return false;
  if (!g_haveMeteo) return false;
  const uint32_t now = millis();
  if (isStale(now)) return false;
  outTempC = g_meteoC;
  outHumPct = g_meteoHum;
  outPressHpa = g_meteoPress;
  outTrend = g_meteoTrend;
  return isfinite(outTempC);
}

String bleGetStatusJson() {
  StaticJsonDocument<1024> doc;
  doc["ok"] = true;
  JsonObject data = doc.createNestedObject("data");
  data["enabled"] = g_enabled;
  data["mode"] = "passive_adv";
  data["scanning"] = bleIsScanning();
  data["broadcastPassive"] = true;
  data["allowlistEnforced"] = g_mac.length() > 0;
  data["allowMac"] = g_mac;
  data["type"] = g_type;
  JsonObject mesh = data.createNestedObject("mesh");
  mesh["enabled"] = g_meshEnabled;
  mesh["relay"] = g_meshRelay;
  mesh["ttl"] = g_meshTtl;
  mesh["advIntervalMs"] = g_meshAdvIntervalMs;
  mesh["minRelayRssi"] = g_meshMinRelayRssi;
  mesh["preferDirect"] = g_meshPreferDirect;
  data["lastRssi"] = g_haveMeteo ? g_lastRssi : 0;
  data["ageMs"] = g_haveMeteo ? (uint32_t)(millis() - g_lastUpdateMs) : 0;

  JsonObject r = data.createNestedObject("reading");
  const bool stale = g_haveMeteo ? isStale(millis()) : true;
  data["stale"] = stale;
  data["maxAgeMs"] = g_maxAgeMs;
  if (g_haveMeteo) {
    r["src"] = g_lastFromMesh ? "mesh" : "direct";
    if (g_lastOriginMac.length()) r["originMac"] = g_lastOriginMac;
    if (g_lastFromMesh) {
      r["hops"] = g_lastHops;
      r["ttl"] = g_lastTtl;
      r["seq"] = g_lastSeq;
    }
    r["tempC"] = g_meteoC;
    if (g_meteoHum >= 0) r["hum"] = g_meteoHum;
    if (g_meteoPress >= 0) r["press"] = g_meteoPress;
    r["trend"] = g_meteoTrend;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void bleDebugSetMeteoTempC(float c) {
  g_meteoC = c;
  g_haveMeteo = isfinite(c);
  g_lastUpdateMs = millis();
}

void bleFillFastJson(ArduinoJson::JsonObject b) {
  // Always provide basic meta so UI can show why data is missing.
  b["en"] = g_enabled;
  b["typ"] = g_type;
  if (g_mac.length()) b["mac"] = g_mac;

  // Default state
  b["ok"] = false;

  if (!g_enabled) return;
  if (!g_haveMeteo) return;

  const uint32_t now = millis();
  const uint32_t age = (uint32_t)(now - g_lastUpdateMs);
  b["a"] = age;
  b["r"] = g_lastRssi;

  const bool fresh = !isStale(now);
  b["fr"] = fresh;
  if (!fresh) return;

  // Fresh reading
  b["ok"] = true;
  b["src"] = g_lastFromMesh ? "mesh" : "direct";
  if (g_lastOriginMac.length()) b["om"] = g_lastOriginMac; // originMac
  if (g_lastFromMesh) {
    b["hp"] = g_lastHops;
    b["tl"] = g_lastTtl;
    b["sq"] = g_lastSeq;
  }
  if (isfinite(g_meteoC)) b["t"] = g_meteoC;
  if (g_meteoHum >= 0) b["h"] = g_meteoHum;
  if (g_meteoPress >= 0) b["p"] = g_meteoPress;
}

// (duplicate definition removed)

#endif // FEATURE_BLE