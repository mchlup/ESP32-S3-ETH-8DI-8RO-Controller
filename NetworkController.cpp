// Feature gate first (keeps WiFi/ETH libs out of the build when disabled).
#include "Features.h"

#include "NetworkController.h"

#if defined(FEATURE_NETWORK)

#include "RtcController.h"

#include <Arduino.h>
#include <ETH.h>
#include <Network.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>

// Optional: use a service input to force WiFiManager portal.
// (Inputs are initialized before networkInit() in the main sketch.)
#include "InputController.h"

static bool wifiConnected = false;

// One-shot WiFiManager portal request (settable via API when device is reachable via Ethernet).
// Stored in NVS so it survives reboot.
static constexpr const char* PREF_NS_NET = "net";
static constexpr const char* PREF_KEY_PORTAL_ONCE = "portal_once";

// Ethernet (W5500 on this board)
// Pin mapping from Waveshare Wiki (ESP32-S3-POE-ETH-8DI-8DO):
// GPIO12=ETH_INT, GPIO13=ETH_MOSI, GPIO14=ETH_MISO, GPIO15=ETH_SCLK, GPIO16=ETH_CS, GPIO39=ETH_RST
static constexpr int ETH_SPI_MOSI = 13;
static constexpr int ETH_SPI_MISO = 14;
static constexpr int ETH_SPI_SCK  = 15;
static constexpr int ETH_PHY_CS   = 16;
static constexpr int ETH_PHY_IRQ  = 12;
static constexpr int ETH_PHY_RST  = 39;
static constexpr int ETH_PHY_ADDR = 1; // ETHClass internal index for SPI PHY

static bool ethLinkUp = false;
static bool ethHasIp = false;

// DHCP grace period: if link is present but IP isn't ready yet, keep Wi-Fi disabled
// for a short time (otherwise we'd immediately fall back to Wi-Fi when Ethernet DHCP is slow).
static uint32_t s_ethLinkUpSinceMs = 0;
static constexpr uint32_t ETH_DHCP_GRACE_MS = 20000; // 20 s

// Runtime switching (Ethernet <-> Wi-Fi)
static bool s_wifiDesired = true;
static bool s_wifiEverConfigured = false; // set true once WiFiManager succeeded
static bool s_pendingSwitchToEth = false;
static bool s_pendingSwitchToWifi = false;
static uint32_t s_lastWifiBeginMs = 0;
static uint32_t s_nextWifiBeginMs = 0;
static String s_lastPrintedIp = "";
static String s_lastPrintedType = "";

static String makeApName() {
    // ESP32-S3 nemá ESP.getChipId(), použijeme spodních 24 bitů MAC adresy
    uint64_t mac = ESP.getEfuseMac();
    uint32_t low = (uint32_t)(mac & 0xFFFFFF);

    char buf[9];
    snprintf(buf, sizeof(buf), "%06X", low);

    String suffix(buf);
    suffix.toUpperCase();
    return "ESP-HeatCtrl-" + suffix;
}

static bool isTimeValid() {
    time_t now = time(nullptr);
    if (now < 1700000000) return false; // ~2023-11-14, jednoduchý sanity check
    return true;
}

// ---- time config (from config.json) ----
static bool s_ntpEnabled = true;
static String s_tz = "CET-1CEST,M3.5.0/2,M10.5.0/3";
static String s_ntp1 = "pool.ntp.org";
static String s_ntp2 = "time.nist.gov";
static uint32_t s_rtcSyncIntervalMin = 60;
static uint32_t s_lastRtcSyncMs = 0;
static String s_timeSource = "none"; // ntp|rtc|none

static bool s_rtcFallbackEnabled = true; // read RTC when system time invalid
static bool s_rtcSyncEnabled = true;     // write system time -> RTC

// ---- ethernet config (runtime preferences) ----
static bool s_ethEnabled = true;
static bool s_preferEthernet = true;
static uint32_t s_ethDhcpGraceMs = ETH_DHCP_GRACE_MS;

// ---- wifi config ----
static bool s_wifiEnabled = true;
static uint32_t s_wifiPortalTimeoutS = 180;
static uint32_t s_wifiConnectTimeoutS = 20;
static uint32_t s_wifiConnectRetries = 3;
static String s_wifiPortalSsid = "";   // empty => auto (ESP-HeatCtrl-XXXXXX)
static String s_wifiPortalPass = "";   // empty => open portal

static bool takePortalOnceFlag() {
    Preferences p;
    if (!p.begin(PREF_NS_NET, false)) return false;
    const bool v = p.getBool(PREF_KEY_PORTAL_ONCE, false);
    if (v) p.putBool(PREF_KEY_PORTAL_ONCE, false);
    p.end();
    return v;
}

static String ianaToPosixTz(const String& iana) {
    // Minimal mapping – extend later if needed
    String s = iana;
    s.trim();
    if (s.length() == 0) return "UTC0";
    String u = s; u.toUpperCase();
    if (u == "EUROPE/PRAGUE" || u == "EUROPE/BRATISLAVA") return "CET-1CEST,M3.5.0/2,M10.5.0/3";
    if (u == "UTC" || u == "ETC/UTC") return "UTC0";
    // If user already provided POSIX, keep it
    if (s.indexOf("CET") >= 0 || s.indexOf("UTC") >= 0 || s.indexOf(",M") >= 0) return s;
    return "UTC0";
}

static void initLocalTime() {
    // Nastavení časové zóny + spuštění SNTP (neblokující)
    const String tzPosix = ianaToPosixTz(s_tz);

    if (!s_ntpEnabled) {
        Serial.printf("[Time] NTP disabled, TZ=%s\n", tzPosix.c_str());
        setenv("TZ", tzPosix.c_str(), 1);
        tzset();
        return;
    }

    Serial.printf("[Time] SNTP init TZ=%s, s1=%s, s2=%s\n", tzPosix.c_str(), s_ntp1.c_str(), s_ntp2.c_str());
    configTzTime(tzPosix.c_str(), s_ntp1.c_str(), s_ntp2.c_str());
}

static void printDebugLinks(const char* netType, const IPAddress& ip) {
    const uint32_t raw = (uint32_t)ip;
    if (raw == 0 || raw == 0xFFFFFFFFUL) return;
    String ipStr = ip.toString();

    const String t = String(netType);
    if (ipStr == s_lastPrintedIp && t == s_lastPrintedType) return;
    s_lastPrintedIp = ipStr;
    s_lastPrintedType = t;

    Serial.printf("[NET] %s IP: %s\n", netType, ipStr.c_str());
    Serial.printf("[NET] UI: http://%s/\n", ipStr.c_str());
    Serial.printf("[NET] Status: http://%s/api/status\n", ipStr.c_str());
    Serial.printf("[NET] FS: http://%s/api/fs/list\n", ipStr.c_str());
}

// --- Ethernet events (W5500) + Wi-Fi events ---
static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
    (void)info;
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            Serial.println(F("[ETH] Started"));
            ETH.setHostname(makeApName().c_str());
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println(F("[ETH] Link UP"));
            ethLinkUp = true;
            s_ethLinkUpSinceMs = millis();
            s_pendingSwitchToEth = true; // if Wi-Fi was active, disable it
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            ethHasIp = true;
            printDebugLinks("eth", ETH.localIP());
            break;
        case ARDUINO_EVENT_ETH_LOST_IP:
            Serial.println(F("[ETH] Lost IP"));
            ethHasIp = false;
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println(F("[ETH] Link DOWN"));
            ethLinkUp = false;
            ethHasIp = false;
            s_ethLinkUpSinceMs = 0;
            s_pendingSwitchToWifi = true;
            break;
        case ARDUINO_EVENT_ETH_STOP:
            Serial.println(F("[ETH] Stopped"));
            ethLinkUp = false;
            ethHasIp = false;
            s_ethLinkUpSinceMs = 0;
            s_pendingSwitchToWifi = true;
            break;

        // Wi-Fi events (mainly for debug output)
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            wifiConnected = true;
            printDebugLinks("wifi", WiFi.localIP());
            break;
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            wifiConnected = false;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            wifiConnected = false;
            break;
        default:
            break;
    }
}

static void ethBeginOnce() {
    static bool started = false;
    if (started) return;
    started = true;

    // Start SPI bus used by onboard W5500
    SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI);
    Serial.printf("[ETH] Init W5500 SPI: CS=%d IRQ=%d SCK=%d MISO=%d MOSI=%d\n",
                  ETH_PHY_CS, ETH_PHY_IRQ, ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI);

    // ETH.begin(type, addr, cs, irq, rst, spi)
    ETH.begin(ETH_PHY_W5500, ETH_PHY_ADDR, ETH_PHY_CS, ETH_PHY_IRQ, ETH_PHY_RST, SPI);
}

void networkInit() {
    // Decide if WiFiManager portal is requested for this boot.
    const bool portalOnce = takePortalOnceFlag();

    // Station mode by default (unless Wi-Fi is disabled).
    if (s_wifiEnabled) {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);          // improves stability on ESP32-S3 with busy loop/SSE
        WiFi.setAutoReconnect(true);
    } else {
        WiFi.mode(WIFI_OFF);
    }
    // Pokud už jsou v NVS uložené Wi-Fi údaje, ber to jako "konfigurováno",
    // i když tentokrát WiFiManager vůbec nespustíme (např. kvůli Ethernetu).
    if (WiFi.SSID().length() > 0) s_wifiEverConfigured = true;

    // RTC (optional)
    rtcInit();

    // Pokud je RTC a systémový čas není validní, zkus načíst čas z RTC
    if (s_rtcFallbackEnabled && rtcIsPresent() && !isTimeValid()) {
        time_t e;
        if (rtcGetEpoch(e)) {
            struct timeval tv; tv.tv_sec = e; tv.tv_usec = 0;
            settimeofday(&tv, nullptr);
            s_timeSource = "rtc";
            Serial.println(F("[Time] System time set from RTC"));
        }
    }

    // Ethernet first: if the RJ45 link is present, we skip WiFiManager AP entirely.
    Network.onEvent(onNetworkEvent);
    if (s_ethEnabled) {
        ethBeginOnce();
    } else {
        Serial.println(F("[ETH] Disabled by config"));
    }

    // Give ETH a brief moment to report link state (non-critical).
    {
        const uint32_t start = millis();
        while (!ethLinkUp && (millis() - start) < 1500) {
            // bez delay() – jen umožni FreeRTOS/driverům zpracovat události
            yield();
        }
    }

    if (s_preferEthernet && ethLinkUp) {
        // Primary Wi-Fi is skipped when LAN cable is connected.
        wifiConnected = false;
        s_wifiDesired = false;
        WiFi.mode(WIFI_OFF);
        Serial.println(F("[NET] Ethernet link detected -> skipping WiFiManager (no AP portal)."));
        initLocalTime();
        // Still allow forcing the portal even when RJ45 is present (service / remote reconfig).
        // If Wi-Fi is disabled in config, the portal can still be requested and will run in AP+STA mode.
        if (!portalOnce) {
            return;
        }
    }

    // Service override: if IN8 is active at boot, force WiFiManager config portal.
    // IN1..IN3 are used by the heating logic; IN8 is typically unused.
    const bool forcePortal = inputGetState(InputId::IN8) || portalOnce;

    // Fast path: if credentials exist and portal is not forced, try a short connect first.
    // If it fails, we MUST still offer the AP portal so the user can recover from wrong/changed creds.
    // (Otherwise the device becomes "unreachable" in Wi-Fi-only installations.)
    bool quickConnectFailed = false;
    if (s_wifiEnabled && !forcePortal && WiFi.SSID().length() > 0) {
        Serial.printf("[WiFi] Stored SSID='%s' -> quick connect...\n", WiFi.SSID().c_str());
        WiFi.begin();
        const uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < 10000UL) {
            yield();
        }
        if (WiFi.status() == WL_CONNECTED) {
            wifiConnected = true;
            s_wifiEverConfigured = true;
            printDebugLinks("wifi", WiFi.localIP());
            initLocalTime();
            Serial.printf("[Time] SNTP init, valid=%s\n", isTimeValid()?"yes":"no");
            return;
        }
        Serial.println(F("[WiFi] Quick connect failed -> starting WiFiManager AP portal for recovery."));
        wifiConnected = false;
        quickConnectFailed = true;
    }

    // If Wi-Fi is disabled and portal is not forced, stay offline.
    if (!s_wifiEnabled && !forcePortal) {
        Serial.println(F("[WiFi] Disabled by config (no portal requested)."));
        return;
    }

    WiFiManager wm;

    // stabilita: nenechávej config portal běžet donekonečna
    wm.setConfigPortalTimeout((int)s_wifiPortalTimeoutS);
    wm.setConnectTimeout((int)s_wifiConnectTimeoutS);
    wm.setConnectRetries((int)s_wifiConnectRetries);

    // SSID konfigurační AP (WiFiManager portal):
    String apName = s_wifiPortalSsid.length() ? s_wifiPortalSsid : makeApName();

    if (forcePortal) {
        Serial.println(F("[WiFi] IN8 active -> forcing WiFiManager portal"));
    }

    // When credentials are missing OR quick connect failed OR portal is forced -> start portal.
    // This makes Wi-Fi setup always recoverable.
    const bool allowPortal = forcePortal || (WiFi.SSID().length() == 0) || quickConnectFailed;
    if (!allowPortal) {
        Serial.println(F("[WiFi] Portal disabled (credentials exist and quick connect not requested)."));
        return;
    }

    // Ensure AP can come up reliably on ESP32-S3 (some stacks behave better in AP+STA mode).
    WiFi.mode(WIFI_AP_STA);

    wm.setAPCallback([](WiFiManager* wmm) {
        Serial.printf("[WiFi] Config portal started. SSID='%s' IP=%s\n",
                      wmm->getConfigPortalSSID().c_str(),
                      WiFi.softAPIP().toString().c_str());
    });

    const char* apPass = (s_wifiPortalPass.length() >= 8) ? s_wifiPortalPass.c_str() : nullptr;
    if (wm.autoConnect(apName.c_str(), apPass)) {
        wifiConnected = true;
        s_wifiEverConfigured = true;
        printDebugLinks("wifi", WiFi.localIP());
        initLocalTime();
        Serial.printf("[Time] SNTP init, valid=%s\n", isTimeValid()?"yes":"no");
    } else {
        wifiConnected = false;
        Serial.println(F("[WiFi] Failed to connect, continuing offline..."));
    }
}

bool networkIsConnected() {
    // Prefer Ethernet if it has an IP (DHCP/static).
    if (ethHasIp) return true;
    return wifiConnected && (WiFi.status() == WL_CONNECTED);
}

bool networkIsWifiConnected() {
    return wifiConnected && (WiFi.status() == WL_CONNECTED);
}

bool networkIsEthernetConnected() {
    return ethHasIp;
}

String networkGetIp() {
    if (ethHasIp) return ETH.localIP().toString();
    if (wifiConnected && (WiFi.status() == WL_CONNECTED)) return WiFi.localIP().toString();
    return String("0.0.0.0");
}

void networkApplyConfig(const String& json) {
    StaticJsonDocument<256> filter;
    filter["time"]["ntpEnabled"] = true;
    filter["time"]["server1"] = true;
    filter["time"]["server2"] = true;
    filter["time"]["tz"] = true;
    filter["time"]["syncIntervalMin"] = true;
    filter["ntpEnabled"] = true;
    filter["ntpServer1"] = true;
    filter["ntpServer2"] = true;
    filter["ntpTz"] = true;
    filter["ntpIntervalMin"] = true;

    filter["time"]["rtcFallbackEnabled"] = true;
    filter["time"]["rtcSyncEnabled"] = true;

    filter["ethernet"]["enabled"] = true;
    filter["ethernet"]["preferEthernet"] = true;
    filter["ethernet"]["dhcpGraceMs"] = true;

    filter["wifi"]["enabled"] = true;
    filter["wifi"]["portalTimeoutS"] = true;
    filter["wifi"]["connectTimeoutS"] = true;
    filter["wifi"]["connectRetries"] = true;
    filter["wifi"]["portalSsid"] = true;
    filter["wifi"]["portalPassword"] = true;

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
    if (err) return;

    // time section
    JsonObject t = doc["time"].as<JsonObject>();
    if (!t.isNull()) {
        if (t.containsKey("ntpEnabled")) s_ntpEnabled = (bool)t["ntpEnabled"];
        if (t.containsKey("server1")) s_ntp1 = String((const char*)t["server1"]);
        if (t.containsKey("server2")) s_ntp2 = String((const char*)t["server2"]);
        if (t.containsKey("tz")) s_tz = String((const char*)t["tz"]);
        if (t.containsKey("syncIntervalMin")) s_rtcSyncIntervalMin = (uint32_t)(t["syncIntervalMin"] | 60);
        if (t.containsKey("rtcFallbackEnabled")) s_rtcFallbackEnabled = (bool)t["rtcFallbackEnabled"];
        if (t.containsKey("rtcSyncEnabled")) s_rtcSyncEnabled = (bool)t["rtcSyncEnabled"];
    } else {
        // backward compatible keys
        if (doc.containsKey("ntpEnabled")) s_ntpEnabled = (bool)doc["ntpEnabled"];
        if (doc.containsKey("ntpServer1")) s_ntp1 = String((const char*)doc["ntpServer1"]);
        if (doc.containsKey("ntpServer2")) s_ntp2 = String((const char*)doc["ntpServer2"]);
        if (doc.containsKey("ntpTz")) s_tz = String((const char*)doc["ntpTz"]);
        if (doc.containsKey("ntpIntervalMin")) s_rtcSyncIntervalMin = (uint32_t)(doc["ntpIntervalMin"] | 60);
    }

    // ethernet section
    JsonObject e = doc["ethernet"].as<JsonObject>();
    if (!e.isNull()) {
        if (e.containsKey("enabled")) s_ethEnabled = (bool)e["enabled"];
        if (e.containsKey("preferEthernet")) s_preferEthernet = (bool)e["preferEthernet"];
        if (e.containsKey("dhcpGraceMs")) {
            uint32_t v = (uint32_t)(e["dhcpGraceMs"] | (uint32_t)s_ethDhcpGraceMs);
            if (v < 0) v = 0;
            if (v > 120000UL) v = 120000UL;
            s_ethDhcpGraceMs = v;
        }
    }

    // wifi section
    JsonObject w = doc["wifi"].as<JsonObject>();
    if (!w.isNull()) {
        if (w.containsKey("enabled")) s_wifiEnabled = (bool)w["enabled"];
        if (w.containsKey("portalTimeoutS")) {
            uint32_t v = (uint32_t)(w["portalTimeoutS"] | (uint32_t)s_wifiPortalTimeoutS);
            if (v < 30) v = 30;
            if (v > 3600) v = 3600;
            s_wifiPortalTimeoutS = v;
        }
        if (w.containsKey("connectTimeoutS")) {
            uint32_t v = (uint32_t)(w["connectTimeoutS"] | (uint32_t)s_wifiConnectTimeoutS);
            if (v < 5) v = 5;
            if (v > 120) v = 120;
            s_wifiConnectTimeoutS = v;
        }
        if (w.containsKey("connectRetries")) {
            uint32_t v = (uint32_t)(w["connectRetries"] | (uint32_t)s_wifiConnectRetries);
            if (v < 1) v = 1;
            if (v > 10) v = 10;
            s_wifiConnectRetries = v;
        }
        if (w.containsKey("portalSsid")) s_wifiPortalSsid = String((const char*)w["portalSsid"]);
        if (w.containsKey("portalPassword")) s_wifiPortalPass = String((const char*)w["portalPassword"]);

        // Security: empty/short password => open portal
        if (s_wifiPortalPass.length() > 0 && s_wifiPortalPass.length() < 8) {
            Serial.println(F("[WiFi] portalPassword too short (<8), ignoring -> open portal"));
            s_wifiPortalPass = "";
        }
    }

    initLocalTime();

    // After (re)init, if time is valid -> sync to RTC
    if (s_rtcSyncEnabled && rtcIsPresent() && isTimeValid()) {
        rtcSetEpoch(time(nullptr));
        s_timeSource = "ntp";
        s_lastRtcSyncMs = millis();
        Serial.printf("[Time] Re-init after config, valid=%s\n", isTimeValid() ? "yes" : "no");
    }
}

void networkLoop() {
    if (!s_wifiEnabled) {
        // If Wi-Fi is disabled, keep it off (unless Ethernet is also disabled and user
        // triggers a one-shot portal via API/IN8 on next boot).
        if (WiFi.getMode() != WIFI_OFF) {
            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_OFF);
        }
        wifiConnected = false;
    }
    // --- Runtime switching between Ethernet and Wi-Fi ---
    // Priority: if Ethernet link is up, we prefer Ethernet and turn Wi-Fi off.
    // If cable is removed, we re-enable Wi-Fi and attempt to reconnect using stored credentials.

    // Wi-Fi is primary, but when RJ45 is connected we prefer Ethernet.
    // While waiting for DHCP on Ethernet, keep Wi-Fi disabled for a short grace period;
    // if DHCP fails (no IP after grace), fall back to Wi-Fi.
    bool wantWifi = s_wifiEnabled;
    if (s_ethEnabled && s_preferEthernet && ethLinkUp) {
        if (ethHasIp) {
            wantWifi = false;
        } else if (s_ethLinkUpSinceMs != 0 && (millis() - s_ethLinkUpSinceMs) < s_ethDhcpGraceMs) {
            wantWifi = false;
        } else {
            wantWifi = true;
        }
    }
    if (wantWifi != s_wifiDesired) {
        s_wifiDesired = wantWifi;
        if (!s_wifiDesired) {
            s_pendingSwitchToEth = true;
        } else {
            s_pendingSwitchToWifi = true;
        }
    }

    if (s_pendingSwitchToEth) {
        s_pendingSwitchToEth = false;
        if (WiFi.getMode() != WIFI_OFF) {
            Serial.println(F("[NET] RJ45 connected -> switching to Ethernet (Wi-Fi OFF)"));
            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_OFF);
            wifiConnected = false;
        }
    }

    if (s_pendingSwitchToWifi && s_wifiEnabled) {
        s_pendingSwitchToWifi = false;
        if (WiFi.getMode() == WIFI_OFF) {
            Serial.println(F("[NET] RJ45 disconnected -> switching back to Wi-Fi"));
            WiFi.mode(WIFI_STA);
        }
        // kick reconnect attempt (non-blocking)
        if (!networkIsWifiConnected()) {
            // avoid spamming begin()
            const uint32_t nowMs = millis();
            if (s_wifiEverConfigured) {
                if (s_nextWifiBeginMs == 0 || nowMs >= s_nextWifiBeginMs) {
                    Serial.println(F("[WiFi] Reconnect..."));
                    WiFi.begin(); // use stored credentials
                    s_lastWifiBeginMs = nowMs;
                    s_nextWifiBeginMs = nowMs + 15000UL; // next try in 15s
                }
            } else {
                Serial.println(F("[WiFi] No stored credentials yet (WiFiManager not completed)."));
            }
        }
    }

    // If Wi-Fi is desired and we're still disconnected, try reconnect periodically.
    if (s_wifiEnabled && s_wifiDesired && !networkIsWifiConnected() && s_wifiEverConfigured && WiFi.getMode() != WIFI_OFF) {
        const uint32_t nowMs = millis();
        if (s_nextWifiBeginMs == 0 || nowMs >= s_nextWifiBeginMs) {
            Serial.println(F("[WiFi] Reconnect..."));
            WiFi.begin();
            s_lastWifiBeginMs = nowMs;
            s_nextWifiBeginMs = nowMs + 15000UL;
        }
    }

    // Determine time source
    if (isTimeValid()) {
        if (s_timeSource == "none") s_timeSource = s_ntpEnabled ? "ntp" : "rtc";
    }

    // RTC periodic sync
    if (!s_rtcSyncEnabled) return;
    if (!rtcIsPresent()) return;
    if (!isTimeValid()) return;

    const uint32_t nowMs = millis();
    const uint32_t intervalMs = (s_rtcSyncIntervalMin < 5 ? 5 : s_rtcSyncIntervalMin) * 60UL * 1000UL;
    if (nowMs - s_lastRtcSyncMs < intervalMs) return;

    rtcSetEpoch(time(nullptr));
    s_lastRtcSyncMs = nowMs;
}

bool networkIsTimeValid() {
    return isTimeValid();
}

String networkGetTimeIso() {
    time_t now = time(nullptr);
    if (!isTimeValid()) return String("");
    struct tm t;
    localtime_r(&now, &t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &t);
    return String(buf);
}

uint32_t networkGetTimeEpoch() {
    time_t now = time(nullptr);
    if (!isTimeValid()) return 0;
    return (uint32_t)now;
}

String networkGetTimeSource() {
    return s_timeSource;
}

bool networkIsRtcPresent() {
    return rtcIsPresent();
}

void networkRequestConfigPortal() {
    Preferences p;
    if (p.begin(PREF_NS_NET, false)) {
        p.putBool(PREF_KEY_PORTAL_ONCE, true);
        p.end();
    }
    Serial.println(F("[WiFi] Requesting WiFiManager portal on next boot -> restart"));
    delay(150);
    ESP.restart();
}

#endif // FEATURE_NETWORK
