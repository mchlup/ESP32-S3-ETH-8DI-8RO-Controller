#include "OtaController.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

namespace {

String g_hostname;
String g_password;

static String makeDefaultHostname() {
    // unikátní suffix z MAC (spodních 24 bitů)
    uint64_t mac = ESP.getEfuseMac();
    uint32_t low = (uint32_t)(mac & 0xFFFFFF);

    char buf[9];
    snprintf(buf, sizeof(buf), "%06X", low);
    String suffix(buf);
    suffix.toUpperCase();
    return "ESP-HeatCtrl-" + suffix;
}

} // anonymous namespace

void OTA::begin(const String& hostname, const String& password) {
    g_hostname = hostname;
    g_password = password;

    if (!g_hostname.length()) {
        g_hostname = makeDefaultHostname();
    }

    // Nastavení hostname pro OTA
    ArduinoOTA.setHostname(g_hostname.c_str());

    // Volitelné heslo
    if (g_password.length() > 0) {
        ArduinoOTA.setPassword(g_password.c_str());
    }

    // ===== Callbacks =====

    ArduinoOTA.onStart([]() {
        String type;

        // Na ESP32 je určitě U_FLASH, někdy i U_SPIFFS
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        }
#if defined(U_SPIFFS)
        else if (ArduinoOTA.getCommand() == U_SPIFFS) {
            type = "filesystem";
        }
#endif
        else {
            type = "unknown";
        }

        Serial.println("[OTA] Start updating: " + type);
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\n[OTA] End");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        if (total == 0) return;
        uint32_t percent = (progress * 100U) / total;
        Serial.printf("[OTA] Progress: %u%%\r", percent);
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA] Error[%u]: ", error);
        switch (error) {
            case OTA_AUTH_ERROR:
                Serial.println("Auth Failed");
                break;
            case OTA_BEGIN_ERROR:
                Serial.println("Begin Failed");
                break;
            case OTA_CONNECT_ERROR:
                Serial.println("Connect Failed");
                break;
            case OTA_RECEIVE_ERROR:
                Serial.println("Receive Failed");
                break;
            case OTA_END_ERROR:
                Serial.println("End Failed");
                break;
            default:
                Serial.println("Unknown");
                break;
        }
    });

    // Start OTA služby – pro ESP32 stačí aktivní síť (WiFi / ETH)
    ArduinoOTA.begin();

    Serial.print("[OTA] Ready. Hostname: ");
    Serial.println(g_hostname);
}

void OTA::loop() {
    // pravidelně zpracovává OTA pakety
    ArduinoOTA.handle();
}
