#include "Features.h"
#include "OtaController.h"

#if defined(FEATURE_OTA)

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>

#include <ArduinoJson.h>

#include "ConfigStore.h"
#include "NetworkController.h"
#include "Log.h"

namespace {
  bool s_inited = false;
  bool s_started = false;

  bool s_enabled = true;
  String s_hostname;
  String s_password;
  uint16_t s_port = 3232;

  bool s_uploading = false;
  uint32_t s_progress = 0;
  uint32_t s_total = 0;
  String s_lastError;
  uint32_t s_lastEventMs = 0;

  static String makeDefaultHostname() {
    uint64_t mac = ESP.getEfuseMac();
    uint32_t low = (uint32_t)(mac & 0xFFFFFF);
    char buf[16];
    snprintf(buf, sizeof(buf), "ESP-HeatCtrl-%06X", low);
    String s(buf);
    s.toUpperCase();
    return s;
  }

  static void loadConfig() {
    // Defaults are intentionally safe.
    s_enabled = ConfigStore::getOtaEnabled();
    s_hostname = ConfigStore::getOtaHostname();
    s_password = ConfigStore::getOtaPassword();
    s_port = ConfigStore::getOtaPort();

    if (!s_hostname.length()) s_hostname = makeDefaultHostname();
    if (s_port < 1024) s_port = 3232;
  }

  static void startIfPossible() {
    if (s_started) return;
    if (!s_enabled) return;
    if (!networkIsConnected()) return;

    loadConfig();

    // Start mDNS (so Arduino IDE can discover "Network Port")
    if (!MDNS.begin(s_hostname.c_str())) {
      LOGW("[OTA] mDNS begin failed (hostname=%s)", s_hostname.c_str());
    } else {
      // Arduino IDE uses _arduino._tcp to list ports.
      MDNS.addService("arduino", "tcp", s_port);
    }

    ArduinoOTA.setHostname(s_hostname.c_str());
    ArduinoOTA.setPort((uint16_t)s_port);

    // Password is optional.
    // Important: setPassword("") can behave as "empty password required" on some cores,
    // leading to confusing auth failures. Use nullptr to disable auth when empty.
    if (s_password.length() > 0) ArduinoOTA.setPassword(s_password.c_str());
    else ArduinoOTA.setPassword(nullptr);

    ArduinoOTA.onStart([]() {
      s_uploading = true;
      s_progress = 0;
      s_total = 0;
      s_lastError = "";
      s_lastEventMs = millis();
      LOGI("[OTA] Start");
    });

    ArduinoOTA.onEnd([]() {
      s_uploading = false;
      s_lastEventMs = millis();
      LOGI("[OTA] End");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      s_uploading = true;
      s_progress = (uint32_t)progress;
      s_total = (uint32_t)total;
      s_lastEventMs = millis();
      // Keep logging sparse (every ~10%)
      if (total > 0) {
        const uint32_t pct = (uint32_t)((progress * 100UL) / total);
        static uint32_t lastPct = 0;
        if (pct >= lastPct + 10 || pct == 100) {
          lastPct = pct;
          LOGI("[OTA] Progress %lu%%", (unsigned long)pct);
        }
      }
    });

    ArduinoOTA.onError([](ota_error_t error) {
      s_uploading = false;
      s_lastEventMs = millis();
      switch (error) {
        case OTA_AUTH_ERROR:    s_lastError = "auth"; break;
        case OTA_BEGIN_ERROR:   s_lastError = "begin"; break;
        case OTA_CONNECT_ERROR: s_lastError = "connect"; break;
        case OTA_RECEIVE_ERROR: s_lastError = "receive"; break;
        case OTA_END_ERROR:     s_lastError = "end"; break;
        default:                s_lastError = "unknown"; break;
      }
      LOGE("[OTA] Error: %s (%d)", s_lastError.c_str(), (int)error);
    });

    ArduinoOTA.begin();
    s_started = true;

    LOGI("[OTA] Ready: %s:%u (password %s)",
         s_hostname.c_str(), (unsigned)s_port, s_password.length() ? "ON" : "OFF");
  }

  static void stopIfDisconnected() {
    if (!s_started) return;
    if (networkIsConnected()) return;

    // No reliable stop API across all cores; mark as stopped so it restarts on reconnect.
    s_started = false;
    s_uploading = false;
    s_progress = 0;
    s_total = 0;
    LOGW("[OTA] Network disconnected -> OTA will restart after reconnect");
  }
}

void otaInit() {
  if (s_inited) return;
  s_inited = true;
  loadConfig();
  // Start will happen lazily after WiFi connect.
}

void otaLoop() {
  stopIfDisconnected();
  startIfPossible();
  if (s_started) ArduinoOTA.handle();
}

OtaConfig otaGetConfig() {
  loadConfig();
  OtaConfig c;
  c.enabled = s_enabled;
  c.hostname = s_hostname;
  c.port = s_port;
  c.passwordSet = (s_password.length() > 0);
  return c;
}

void otaApplyConfig(const String& json) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json)) return;
  if (!doc.is<JsonObject>()) return;

  JsonObjectConst o = doc.as<JsonObjectConst>();
  if (o.containsKey("enabled")) ConfigStore::setOtaEnabled((bool)(o["enabled"] | true));
  if (o.containsKey("hostname")) {
    String h = String((const char*)(o["hostname"] | ""));
    h.trim();
    ConfigStore::setOtaHostname(h);
  }
  if (o.containsKey("port")) {
    uint32_t p = (uint32_t)(o["port"] | 3232);
    if (p < 1024) p = 3232;
    if (p > 65535) p = 65535;
    ConfigStore::setOtaPort((uint16_t)p);
  }
  if (o.containsKey("password")) {
    String pw = String((const char*)(o["password"] | ""));
    pw.trim();
    // Empty string clears password
    ConfigStore::setOtaPassword(pw);
  }

  // Apply changes at next reconnect / reboot.
  // We mark OTA as stopped so it restarts with new settings.
  s_started = false;
  loadConfig();
}

void otaFillFastJson(JsonObject out) {
  out["enabled"] = ConfigStore::getOtaEnabled();
  out["started"] = s_started;
  out["uploading"] = s_uploading;
  if (s_total > 0) out["pct"] = (uint32_t)((s_progress * 100UL) / s_total);
  else out["pct"] = 0;
}

String otaGetStatusJson() {
  DynamicJsonDocument doc(1024);
  doc["ok"] = true;
  doc["enabled"] = ConfigStore::getOtaEnabled();
  doc["started"] = s_started;
  doc["uploading"] = s_uploading;
  doc["hostname"] = otaGetConfig().hostname;
  doc["port"] = otaGetConfig().port;
  doc["passwordSet"] = otaGetConfig().passwordSet;
  doc["progress"] = s_progress;
  doc["total"] = s_total;
  // ArduinoJson + Arduino String: avoid ternary with nullptr (ambiguous conversion)
  if (s_lastError.length()) {
    doc["lastError"] = s_lastError;
  } else {
    doc["lastError"] = nullptr;
  }
  doc["lastEventMs"] = (uint32_t)s_lastEventMs;

  String out;
  serializeJson(doc, out);
  return out;
}

#endif // FEATURE_OTA
