#include "NetworkController.h"
#include "RtcController.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>

static bool wifiConnected = false;

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







void networkInit() {
    WiFi.mode(WIFI_STA);

    // RTC (optional)
    rtcInit();

    // Pokud je RTC a systémový čas není validní, zkus načíst čas z RTC
    if (rtcIsPresent() && !isTimeValid()) {
        time_t e;
        if (rtcGetEpoch(e)) {
            struct timeval tv; tv.tv_sec = e; tv.tv_usec = 0;
            settimeofday(&tv, nullptr);
            s_timeSource = "rtc";
            Serial.println(F("[Time] System time set from RTC"));
        }
    }


    WiFiManager wm;

    // stabilita: nenechávej config portal běžet donekonečna
    wm.setConfigPortalTimeout(180);      // s
    wm.setConnectTimeout(20);           // s
    wm.setConnectRetries(3);

    // SSID konfigurační AP, pokud není uložená WiFi:
    // např. ESP-HeatCtrl-ABCDEF
    String apName = makeApName();

    if (wm.autoConnect(apName.c_str())) {
        wifiConnected = true;
        Serial.print(F("[WiFi] Connected, IP: "));
        Serial.println(WiFi.localIP());
        initLocalTime();
        Serial.printf("[Time] SNTP init, valid=%s\n", isTimeValid()?"yes":"no");
    } else {
        wifiConnected = false;
        Serial.println(F("[WiFi] Failed to connect, continuing offline..."));
    }
}

bool networkIsConnected() {
    return wifiConnected && (WiFi.status() == WL_CONNECTED);
}

String networkGetIp() {
    if (!networkIsConnected()) return String("0.0.0.0");
    return WiFi.localIP().toString();
}


void networkApplyConfig(const String& json) {
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, json);
    if (err) return;

    // time section
    JsonObject t = doc["time"].as<JsonObject>();
    if (!t.isNull()) {
        if (t.containsKey("ntpEnabled")) s_ntpEnabled = (bool)t["ntpEnabled"];
        if (t.containsKey("server1")) s_ntp1 = String((const char*)t["server1"]);
        if (t.containsKey("server2")) s_ntp2 = String((const char*)t["server2"]);
        if (t.containsKey("tz")) s_tz = String((const char*)t["tz"]);
        if (t.containsKey("syncIntervalMin")) s_rtcSyncIntervalMin = (uint32_t)(t["syncIntervalMin"] | 60);
    } else {
        // backward compatible keys
        if (doc.containsKey("ntpEnabled")) s_ntpEnabled = (bool)doc["ntpEnabled"];
        if (doc.containsKey("ntpServer1")) s_ntp1 = String((const char*)doc["ntpServer1"]);
        if (doc.containsKey("ntpServer2")) s_ntp2 = String((const char*)doc["ntpServer2"]);
        if (doc.containsKey("ntpTz")) s_tz = String((const char*)doc["ntpTz"]);
        if (doc.containsKey("ntpIntervalMin")) s_rtcSyncIntervalMin = (uint32_t)(doc["ntpIntervalMin"] | 60);
    }

    initLocalTime();

    // After (re)init, if time is valid -> sync to RTC
    if (rtcIsPresent() && isTimeValid()) {
        rtcSetEpoch(time(nullptr));
        s_timeSource = "ntp";
        s_lastRtcSyncMs = millis();
        Serial.printf("[Time] Re-init after config, valid=%s\n", isTimeValid() ? "yes" : "no");
    }
}

void networkLoop() {
    // Determine time source
    if (isTimeValid()) {
        if (s_timeSource == "none") s_timeSource = s_ntpEnabled ? "ntp" : "rtc";
    }

    // RTC periodic sync
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
