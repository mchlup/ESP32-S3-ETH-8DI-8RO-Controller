#include "NetworkController.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>

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

void networkInit() {
    WiFi.mode(WIFI_STA);

    WiFiManager wm;

    // SSID konfigurační AP, pokud není uložená WiFi:
    // např. ESP-HeatCtrl-ABCDEF
    String apName = makeApName();

    if (wm.autoConnect(apName.c_str())) {
        wifiConnected = true;
        Serial.print(F("[WiFi] Connected, IP: "));
        Serial.println(WiFi.localIP());
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
