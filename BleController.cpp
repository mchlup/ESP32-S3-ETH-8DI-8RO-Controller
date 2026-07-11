#include "Features.h"
#include "BleController.h"

#if defined(FEATURE_BLE)

#include <NimBLEDevice.h>

namespace {
  static BleConfig g_cfg;
  static BleStatus g_st;
  static BleMeteoData g_meteo;

  static NimBLEScan* g_scan = nullptr;
  static NimBLEClient* g_client = nullptr;
  static NimBLERemoteCharacteristic* g_ch = nullptr;

  static NimBLEAddress g_targetAddr; // default (null)
  static bool g_haveTarget = false;

  static uint32_t g_nextScanMs = 0;
  static uint32_t g_nextConnectAttemptMs = 0;

  // UUIDs must match the outdoor sensor sketch
  static NimBLEUUID UUID_SVC("7b7c1001-3a2b-4f2a-8bb0-8d2c2c1a1001");
  static NimBLEUUID UUID_CH ("7b7c1002-3a2b-4f2a-8bb0-8d2c2c1a1001");

  // 3s scan burst; non-blocking; repeats via g_cfg.scanIntervalMs
  static constexpr uint32_t SCAN_DURATION_MS = 3000;

  static inline int16_t le_i16(const uint8_t* p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
  }
  static inline uint16_t le_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
  }

  static void meteoOnNotify(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    (void)chr;
    (void)isNotify;
    if (!data || len < 6) return;

    const int16_t t10 = le_i16(data);
    const uint8_t hum = data[2];
    const uint16_t pHpa = le_u16(data + 3);
    const int8_t trend = (int8_t)data[5];

    g_meteo.valid = true;
    g_meteo.tempC = ((float)t10) / 10.0f;
    g_meteo.humidityPct = (int)hum;
    g_meteo.pressureHpa = (float)pHpa;
    g_meteo.trend = (int)trend;
    g_meteo.lastUpdateMs = millis();

    g_st.lastUpdateMs = g_meteo.lastUpdateMs;
  }

  class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* c) override {
      (void)c;
      g_st.connected = true;
      g_st.lastError = "";
    }

    void onConnectFail(NimBLEClient* c, int reason) override {
      (void)c;
      g_st.connected = false;
      g_ch = nullptr;
      g_st.lastError = String("connect fail (") + String(reason) + ")";
      g_nextConnectAttemptMs = millis() + g_cfg.reconnectBackoffMs;
      g_nextScanMs = millis() + 500;
      g_haveTarget = false; // rescan, sensor may use random address
    }

    void onDisconnect(NimBLEClient* c, int reason) override {
      (void)c;
      g_st.connected = false;
      g_ch = nullptr;
      g_st.peer = "";
      g_st.lastError = String("disconnected (") + String(reason) + ")";

      // Rescan (sensor may rotate random address)
      g_haveTarget = false;
      g_nextScanMs = millis() + 1000;
      g_nextConnectAttemptMs = millis() + g_cfg.reconnectBackoffMs;
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
      (void)connInfo;
      // no security expected
    }
  };

  class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* adv) override {
      if (!adv) return;
      if (!g_cfg.enabled) return;
      if (g_haveTarget) return;

      bool accept = false;

      // name prefix
      const std::string name = adv->getName();
      if (!name.empty()) {
        String s(name.c_str());
        if (s.startsWith(g_cfg.namePrefix)) accept = true;
      }

      // service UUID fallback
      if (!accept) {
        if (adv->isAdvertisingService(UUID_SVC)) accept = true;
      }

      if (!accept) return;

      g_targetAddr = adv->getAddress();
      g_haveTarget = true;
      g_st.peer = String(g_targetAddr.toString().c_str());

      // Stop scan ASAP to reduce load.
      if (g_scan && g_scan->isScanning()) {
        g_scan->stop();
      }
      g_st.scanning = false;

      // Try connect soon.
      g_nextConnectAttemptMs = millis();
    }

    void onScanEnd(const NimBLEScanResults& scanResults, int reason) override {
      (void)scanResults;
      (void)reason;
      g_st.scanning = false;

      // if nothing found, schedule next scan
      if (!g_haveTarget && g_cfg.enabled) {
        g_nextScanMs = millis() + g_cfg.scanIntervalMs;
      }
    }
  };

  static ClientCallbacks g_clientCb;
  static ScanCallbacks g_scanCb;

  static void ensureInit() {
    static bool inited = false;
    if (inited) return;
    inited = true;

    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // No pairing / no MITM.
    NimBLEDevice::setSecurityAuth(false, false, false);

    g_scan = NimBLEDevice::getScan();
    if (g_scan) {
      g_scan->setScanCallbacks(&g_scanCb, false);
      g_scan->setMaxResults(0);        // callbacks only
      g_scan->setActiveScan(true);
      g_scan->setInterval(45);
      g_scan->setWindow(15);
    }

    g_client = NimBLEDevice::createClient();
    if (g_client) {
      g_client->setClientCallbacks(&g_clientCb, false);
    }

    g_nextScanMs = millis() + 500;
    g_nextConnectAttemptMs = millis() + 1000;
  }

  static void startScan() {
    if (!g_scan) return;

    g_haveTarget = false;
    g_st.scanning = true;
    g_st.lastError = "";

    // Non-blocking scan; callback-driven.
    // duration in ms, 0 = forever.
    const bool ok = g_scan->start(SCAN_DURATION_MS, false, true);
    if (!ok) {
      g_st.scanning = false;
      g_st.lastError = "scan start failed";
      g_nextScanMs = millis() + g_cfg.scanIntervalMs;
    }
  }

  static bool connectAndSubscribe() {
    if (!g_client) return false;
    if (!g_haveTarget) return false;

    g_st.lastConnectAttemptMs = millis();

    if (g_client->isConnected()) {
      g_st.connected = true;
      return true;
    }

    if (!g_client->connect(g_targetAddr)) {
      g_st.lastError = "connect failed";
      g_st.connected = false;
      return false;
    }

    NimBLERemoteService* svc = g_client->getService(UUID_SVC);
    if (!svc) {
      g_st.lastError = "service not found";
      g_client->disconnect();
      return false;
    }

    g_ch = svc->getCharacteristic(UUID_CH);
    if (!g_ch) {
      g_st.lastError = "characteristic not found";
      g_client->disconnect();
      return false;
    }

    if (!g_ch->canNotify()) {
      g_st.lastError = "notify not supported";
      g_client->disconnect();
      return false;
    }

    if (!g_ch->subscribe(true, meteoOnNotify, true)) {
      g_st.lastError = "subscribe failed";
      g_client->disconnect();
      return false;
    }

    g_st.connected = true;
    return true;
  }
}

void bleInit() {
  g_st.enabled = g_cfg.enabled;
  if (g_cfg.enabled) ensureInit();
}

void bleLoop() {
  g_st.enabled = g_cfg.enabled;

  if (!g_cfg.enabled) {
    if (g_scan && g_scan->isScanning()) {
      g_scan->stop();
    }
    if (g_client && g_client->isConnected()) {
      g_client->disconnect();
    }
    g_st.scanning = false;
    g_st.connected = false;
    g_haveTarget = false;
    return;
  }

  ensureInit();
  const uint32_t now = millis();

  // Connected => nothing to do here; notify callback keeps data updated.
  if (g_client && g_client->isConnected()) {
    g_st.connected = true;
    return;
  }
  g_st.connected = false;

  // If no target, scan periodically.
  if (!g_haveTarget) {
    if (!g_scan) return;

    if (!g_scan->isScanning()) {
      g_st.scanning = false;
      if ((int32_t)(now - g_nextScanMs) >= 0) {
        startScan();
        // next schedule is handled by onScanEnd, but keep a safety fallback
        g_nextScanMs = now + g_cfg.scanIntervalMs;
      }
    } else {
      g_st.scanning = true;
    }
    return;
  }

  // Have target => attempt connect with backoff.
  if ((int32_t)(now - g_nextConnectAttemptMs) >= 0) {
    const bool ok = connectAndSubscribe();
    if (!ok) {
      g_nextConnectAttemptMs = now + g_cfg.reconnectBackoffMs;
      g_nextScanMs = now + 500;
      g_haveTarget = false; // rescan (random addr)

      if (g_scan && !g_scan->isScanning()) {
        startScan();
      }
    }
  }
}

void bleApplyConfig(const String& json) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json)) return;
  JsonObjectConst o = doc.as<JsonObjectConst>();
  if (o.isNull()) return;

  if (o.containsKey("enabled")) g_cfg.enabled = (bool)o["enabled"];
  if (o.containsKey("namePrefix")) {
    g_cfg.namePrefix = String((const char*)(o["namePrefix"] | g_cfg.namePrefix.c_str()));
  }
  if (o.containsKey("scanIntervalMs")) g_cfg.scanIntervalMs = (uint32_t)(o["scanIntervalMs"] | g_cfg.scanIntervalMs);
  if (o.containsKey("reconnectBackoffMs")) g_cfg.reconnectBackoffMs = (uint32_t)(o["reconnectBackoffMs"] | g_cfg.reconnectBackoffMs);

  // Apply quickly
  g_st.enabled = g_cfg.enabled;
  if (!g_cfg.enabled) {
    g_haveTarget = false;
    if (g_scan && g_scan->isScanning()) g_scan->stop();
    if (g_client && g_client->isConnected()) g_client->disconnect();
    g_st.scanning = false;
    g_st.connected = false;
  } else {
    g_nextScanMs = millis() + 200;
  }
}

BleConfig bleGetConfig() { return g_cfg; }
BleStatus bleGetStatus() { return g_st; }
BleMeteoData bleGetMeteo() { return g_meteo; }

String bleGetStatusJson() {
  DynamicJsonDocument doc(768);
  doc["ok"] = true;
  doc["en"] = g_cfg.enabled;
  doc["sc"] = g_st.scanning;
  doc["cn"] = g_st.connected;
  doc["peer"] = g_st.peer;
  doc["err"] = g_st.lastError;
  doc["lu"] = g_st.lastUpdateMs;
  doc["namePrefix"] = g_cfg.namePrefix;
  doc["scanIntervalMs"] = g_cfg.scanIntervalMs;
  doc["reconnectBackoffMs"] = g_cfg.reconnectBackoffMs;

  JsonObject m = doc.createNestedObject("meteo");
  m["v"] = g_meteo.valid;
  if (g_meteo.valid && isfinite(g_meteo.tempC)) m["t"] = g_meteo.tempC;
  else m["t"] = nullptr;
  if (g_meteo.valid && g_meteo.humidityPct >= 0) m["h"] = g_meteo.humidityPct;
  else m["h"] = nullptr;
  if (g_meteo.valid && isfinite(g_meteo.pressureHpa)) m["p"] = g_meteo.pressureHpa;
  else m["p"] = nullptr;
  m["tr"] = g_meteo.valid ? g_meteo.trend : 0;
  m["ms"] = g_meteo.lastUpdateMs;

  String out;
  serializeJson(doc, out);
  return out;
}

void bleFillFastJson(JsonObject& out) {
  out["en"] = g_cfg.enabled;
  out["sc"] = g_st.scanning;
  out["cn"] = g_st.connected;
  if (g_meteo.valid && isfinite(g_meteo.tempC)) out["t"] = g_meteo.tempC;
  else out["t"] = nullptr;
  if (g_meteo.valid && g_meteo.humidityPct >= 0) out["h"] = g_meteo.humidityPct;
  else out["h"] = nullptr;
  if (g_meteo.valid && isfinite(g_meteo.pressureHpa)) out["p"] = g_meteo.pressureHpa;
  else out["p"] = nullptr;
  out["ms"] = g_meteo.lastUpdateMs;
}

#else

// stubs
void bleInit() {}
void bleLoop() {}
void bleApplyConfig(const String&) {}
BleConfig bleGetConfig() { return BleConfig{}; }
BleStatus bleGetStatus() { return BleStatus{}; }
BleMeteoData bleGetMeteo() { return BleMeteoData{}; }
String bleGetStatusJson() { return String("{\"ok\":true,\"en\":false}"); }
void bleFillFastJson(JsonObject& out) { out["en"] = false; }

#endif
