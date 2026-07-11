#include "Features.h"
#include "NetworkController.h"

#if defined(FEATURE_NETWORK)

#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ETH.h>
#include <SPI.h>
#include <esp_arduino_version.h>

#include <time.h>
#include <stdlib.h>

#include "InputController.h"
#include "ConfigStore.h"

// ETH_PHY_W5500 is an enum value in Arduino-ESP32, not a preprocessor macro.
// Testing it with defined(ETH_PHY_W5500) therefore always evaluated to false
// and disabled Ethernet even on cores that support the SPI W5500 driver.
// The ETH.begin(..., SPI) overload used below is available in Arduino-ESP32 3.x.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
#define NETWORK_ETH_W5500_SUPPORTED 1
#else
#define NETWORK_ETH_W5500_SUPPORTED 0
#endif

namespace {
  // Preferences: one-shot portal request
  static constexpr const char* PREF_NS_NET = "net";
  static constexpr const char* PREF_KEY_PORTAL_ONCE = "portal_once";

  static WiFiManager wm;
  static bool s_inited = false;
  static bool s_portalActive = false;
  static bool s_wifiConnected = false;
  static bool s_ethStarted = false;
  static bool s_ethConnected = false;

#if NETWORK_ETH_W5500_SUPPORTED
  static constexpr int ETH_SPI_INT_PIN = 12;
  static constexpr int ETH_SPI_MOSI_PIN = 13;
  static constexpr int ETH_SPI_MISO_PIN = 14;
  static constexpr int ETH_SPI_SCK_PIN = 15;
  static constexpr int ETH_SPI_CS_PIN = 16;
  static constexpr int ETH_SPI_RST_PIN = -1;
#endif

  // Time (SNTP)
  static bool s_timeConfigured = false;
  static bool s_timeValid = false;
  static uint32_t s_lastTimeCheckMs = 0;
  static String s_timeSource = "none";

  static bool s_timeEnabled = true;
  static String s_tz = "CET-1CEST,M3.5.0,M10.5.0/3";
  static String s_ntp1 = "pool.ntp.org";
  static String s_ntp2 = "europe.pool.ntp.org";
  static String s_ntp3 = "time.nist.gov";

  static uint32_t s_portalTimeoutS = 180;
  static uint32_t s_connectTimeoutS = 20;
  static uint32_t s_connectRetries = 3;

  static String s_apSsid = "";
  static String s_apPass = "";

  static bool takePortalOnceFlag() {
    Preferences p;
    if (!p.begin(PREF_NS_NET, false)) return false;
    const bool v = p.getBool(PREF_KEY_PORTAL_ONCE, false);
    if (v) p.putBool(PREF_KEY_PORTAL_ONCE, false);
    p.end();
    return v;
  }

  static void setPortalOnceFlag() {
    Preferences p;
    if (!p.begin(PREF_NS_NET, false)) return;
    p.putBool(PREF_KEY_PORTAL_ONCE, true);
    p.end();
  }

  static String makeApName() {
    uint64_t mac = ESP.getEfuseMac();
    uint32_t low = (uint32_t)(mac & 0xFFFFFF);
    char buf[9];
    snprintf(buf, sizeof(buf), "%06X", low);
    String suffix(buf);
    suffix.toUpperCase();
    return "ESP-HeatCtrl-" + suffix;
  }

  static bool anyIpConnected() {
    return s_wifiConnected || s_ethConnected;
  }

  static void configureTimeIfPossible(bool force = false);

#if NETWORK_ETH_W5500_SUPPORTED
  static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
    (void)info;
    switch (event) {
      case ARDUINO_EVENT_ETH_START:
        s_ethStarted = true;
        ETH.setHostname(makeApName().c_str());
        Serial.println("[ETH] Started");
        break;
      case ARDUINO_EVENT_ETH_CONNECTED:
        Serial.println("[ETH] Link up");
        break;
      case ARDUINO_EVENT_ETH_GOT_IP:
        s_ethConnected = true;
        Serial.printf("[ETH] GOT IP: %s\n", ETH.localIP().toString().c_str());
        configureTimeIfPossible(true);
        break;
      case ARDUINO_EVENT_ETH_LOST_IP:
        s_ethConnected = false;
        Serial.println("[ETH] Lost IP");
        break;
      case ARDUINO_EVENT_ETH_DISCONNECTED:
        s_ethConnected = false;
        Serial.println("[ETH] Disconnected");
        break;
      case ARDUINO_EVENT_ETH_STOP:
        s_ethStarted = false;
        s_ethConnected = false;
        Serial.println("[ETH] Stopped");
        break;
      default:
        break;
    }
  }

  static void startEthernet() {
    if (s_ethStarted) return;
    Serial.printf("[ETH] Arduino-ESP32 %d.%d.%d, enabling W5500 SPI driver\n",
                  ESP_ARDUINO_VERSION_MAJOR,
                  ESP_ARDUINO_VERSION_MINOR,
                  ESP_ARDUINO_VERSION_PATCH);
    Network.onEvent(onNetworkEvent);
    SPI.begin(ETH_SPI_SCK_PIN, ETH_SPI_MISO_PIN, ETH_SPI_MOSI_PIN);
    if (!ETH.begin(ETH_PHY_W5500, 1, ETH_SPI_CS_PIN, ETH_SPI_INT_PIN, ETH_SPI_RST_PIN, SPI)) {
      Serial.println("[ETH] W5500 init failed");
      return;
    }
    s_ethStarted = true;
    Serial.printf("[ETH] W5500 init requested (INT=%d MOSI=%d MISO=%d SCK=%d CS=%d)\n",
                  ETH_SPI_INT_PIN, ETH_SPI_MOSI_PIN, ETH_SPI_MISO_PIN, ETH_SPI_SCK_PIN, ETH_SPI_CS_PIN);
  }
#else
  static void startEthernet() {
    Serial.printf("[ETH] W5500 requires Arduino-ESP32 3.x; detected %d.%d.%d\n",
                  ESP_ARDUINO_VERSION_MAJOR,
                  ESP_ARDUINO_VERSION_MINOR,
                  ESP_ARDUINO_VERSION_PATCH);
  }
#endif

  static void wifiManagerSetupCommon() {
    wm.setDebugOutput(true);
    wm.setConfigPortalBlocking(false);
    wm.setConfigPortalTimeout((int)s_portalTimeoutS);
    wm.setConnectTimeout((int)s_connectTimeoutS);
    wm.setConnectRetries((int)s_connectRetries);
    wm.setBreakAfterConfig(true);

    wm.setAPCallback([](WiFiManager* wmm) {
      s_portalActive = true;
      Serial.printf("[WiFi] Config portal started. SSID='%s' IP=%s\n",
                    wmm->getConfigPortalSSID().c_str(),
                    WiFi.softAPIP().toString().c_str());
    });
  }

  static void loadTimeConfig() {
    s_timeEnabled = ConfigStore::getTimeEnabled();
    s_tz = ConfigStore::getTimeTz();
    s_ntp1 = ConfigStore::getTimeNtp1();
    s_ntp2 = ConfigStore::getTimeNtp2();
    s_ntp3 = ConfigStore::getTimeNtp3();

    s_tz.trim();
    s_ntp1.trim(); s_ntp2.trim(); s_ntp3.trim();
    if (!s_ntp1.length()) s_ntp1 = "pool.ntp.org";
  }

  static void configureTimeIfPossible(bool force) {
    if (!s_timeEnabled) {
      s_timeConfigured = false;
      s_timeValid = false;
      s_timeSource = "disabled";
      return;
    }
    if (!anyIpConnected()) return;
    if (s_timeConfigured && !force) return;

    loadTimeConfig();
    setenv("TZ", s_tz.c_str(), 1);
    tzset();

    // configTime expects UTC offset and DST; we use TZ env instead -> pass 0,0.
    configTime(0, 0,
               s_ntp1.c_str(),
               s_ntp2.length() ? s_ntp2.c_str() : nullptr,
               s_ntp3.length() ? s_ntp3.c_str() : nullptr);

    s_timeConfigured = true;
    s_timeSource = "sntp";
    s_lastTimeCheckMs = 0;
    s_timeValid = false;
  }

  static void updateTimeValidity() {
    const uint32_t nowMs = millis();
    if (nowMs - s_lastTimeCheckMs < 2000) return;
    s_lastTimeCheckMs = nowMs;
    time_t now = time(nullptr);
    // Consider valid if after 2023-01-01
    s_timeValid = (now > (time_t)1672531200);
  }

  static void startPortalOrAutoConnect(bool forcePortal) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);

    wifiManagerSetupCommon();

    const String apName = s_apSsid.length() ? s_apSsid : makeApName();
    const char* apPass = (s_apPass.length() >= 8) ? s_apPass.c_str() : nullptr;

    bool ok = false;

    if (forcePortal) {
      Serial.printf("[WiFi] Starting portal (forced). SSID='%s' timeout=%lus\n",
                    apName.c_str(), (unsigned long)s_portalTimeoutS);
      ok = wm.startConfigPortal(apName.c_str(), apPass);
      s_portalActive = true;
    } else {
      Serial.printf("[WiFi] WiFiManager autoConnect. SSID='%s' timeout=%lus\n",
                    apName.c_str(), (unsigned long)s_portalTimeoutS);
      ok = wm.autoConnect(apName.c_str(), apPass);
      if (!ok) s_portalActive = true;
    }

    if (ok && WiFi.status() == WL_CONNECTED) {
      s_wifiConnected = true;
      s_portalActive = false;
      Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
    } else {
      s_wifiConnected = false;
      Serial.println("[WiFi] Not connected yet (portal running or trying).");
    }
  }
}

void networkInit() {
  if (s_inited) return;
  s_inited = true;

  startEthernet();

  const bool portalOnce = takePortalOnceFlag();
  const bool portalByInput = inputGetState(InputId::IN8); // IN8: service button
  const bool forcePortal = portalOnce || portalByInput;

  startPortalOrAutoConnect(forcePortal);

  // If already connected quickly, start time immediately.
  configureTimeIfPossible(true);
}

void networkLoop() {
  // WiFiManager should only own the captive portal while there is no usable IP
  // path. Once either WiFi or Ethernet is online, stop the portal so our own
  // web/API server remains authoritative on port 80.

  const bool wifiNow = (WiFi.status() == WL_CONNECTED);
  const bool haveNet = wifiNow || s_ethConnected;

  if (!haveNet) {
    wm.process();
  }

  if (wifiNow != s_wifiConnected) {
    const bool wasPortal = s_portalActive;
    s_wifiConnected = wifiNow;

    if (wifiNow) {
      if (wasPortal) {
        Serial.println("[WiFi] Stopping WiFiManager config portal (connected).");
        wm.stopConfigPortal();
      }
      s_portalActive = false;
      Serial.printf("[WiFi] GOT IP: %s\n", WiFi.localIP().toString().c_str());
      configureTimeIfPossible(true);
    }
  }

  if (haveNet && s_portalActive) {
    Serial.println("[WiFi] Stopping WiFiManager config portal (network available).");
    wm.stopConfigPortal();
    s_portalActive = false;
  }

  if (haveNet && s_timeConfigured) updateTimeValidity();
}

void networkApplyConfig(const String& json) {
  // Minimal: allow portal timeout and SSID/PASS override.
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json)) return;
  JsonObjectConst o = doc.as<JsonObjectConst>();
  if (o.isNull()) return;

  if (o.containsKey("portalTimeoutS")) s_portalTimeoutS = (uint32_t)(o["portalTimeoutS"] | s_portalTimeoutS);
  if (o.containsKey("connectTimeoutS")) s_connectTimeoutS = (uint32_t)(o["connectTimeoutS"] | s_connectTimeoutS);
  if (o.containsKey("connectRetries")) s_connectRetries = (uint32_t)(o["connectRetries"] | s_connectRetries);
  if (o.containsKey("apSsid")) s_apSsid = String((const char*)(o["apSsid"] | s_apSsid.c_str()));
  if (o.containsKey("apPass")) s_apPass = String((const char*)(o["apPass"] | s_apPass.c_str()));

  // Time config (optional). Accept either top-level or nested {time:{...}}
  JsonObjectConst t = o.containsKey("time") ? o["time"].as<JsonObjectConst>() : o;
  if (!t.isNull()) {
    bool touched = false;
    if (t.containsKey("enabled")) { ConfigStore::setTimeEnabled((bool)(t["enabled"] | true)); touched = true; }
    if (t.containsKey("tz")) { ConfigStore::setTimeTz(String((const char*)(t["tz"] | ""))); touched = true; }
    if (t.containsKey("ntp") && t["ntp"].is<JsonArrayConst>()) {
      JsonArrayConst a = t["ntp"].as<JsonArrayConst>();
      String s1 = ConfigStore::getTimeNtp1();
      String s2 = ConfigStore::getTimeNtp2();
      String s3 = ConfigStore::getTimeNtp3();
      int idx = 0;
      for (JsonVariantConst v : a) {
        if (!v.is<const char*>()) continue;
        String sv = String((const char*)v);
        sv.trim();
        if (!sv.length()) continue;
        if (idx == 0) s1 = sv;
        if (idx == 1) s2 = sv;
        if (idx == 2) s3 = sv;
        idx++;
        if (idx >= 3) break;
      }
      ConfigStore::setTimeNtp1(s1);
      ConfigStore::setTimeNtp2(s2);
      ConfigStore::setTimeNtp3(s3);
      touched = true;
    }

    if (touched) {
      // Re-configure time immediately if possible.
      s_timeConfigured = false;
      configureTimeIfPossible(true);
    }
  }
}

void networkRequestConfigPortal() {
  setPortalOnceFlag();
  Serial.println("[WiFi] Requested portal on next boot -> restarting...");
  delay(200);
  ESP.restart();
}

bool networkIsConnected() { return networkIsWifiConnected() || networkIsEthernetConnected(); }

bool networkIsWifiConnected() {
  if (WiFi.status() != WL_CONNECTED) return false;
  const IPAddress ip = WiFi.localIP();
  return ip != IPAddress(0, 0, 0, 0);
}

bool networkIsEthernetConnected() {
#if NETWORK_ETH_W5500_SUPPORTED
  // Event flags are the primary source, but also verify the live interface.
  // This makes startup robust if GOT_IP happened before another module queried
  // the network state or if an event was missed during initialization.
  const IPAddress ip = ETH.localIP();
  const bool live = ETH.linkUp() && ip != IPAddress(0, 0, 0, 0);
  if (live && !s_ethConnected) s_ethConnected = true;
  if (!live && s_ethConnected) s_ethConnected = false;
  return live;
#else
  return false;
#endif
}

String networkGetIp() {
#if NETWORK_ETH_W5500_SUPPORTED
  if (networkIsEthernetConnected()) return ETH.localIP().toString();
#endif
  if (networkIsWifiConnected()) return WiFi.localIP().toString();
  return String();
}

bool networkIsTimeValid() {
  updateTimeValidity();
  return s_timeValid;
}

uint32_t networkGetTimeEpoch() {
  time_t now = time(nullptr);
  if (now < 0) return 0;
  return (uint32_t)now;
}

String networkGetTimeIso() {
  time_t now = time(nullptr);
  if (now <= (time_t)1672531200) return String();
  struct tm tmv;
  localtime_r(&now, &tmv);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
  return String(buf);
}

String networkGetTimeSource() {
  return s_timeSource;
}
bool networkIsRtcPresent() { return false; }

#endif
