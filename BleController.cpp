#include "BleController.h"

#include <FS.h>
#include <LittleFS.h>
#include "FsController.h"
#include <ArduinoJson.h>

#include <string>
#include <ctype.h>

// NimBLE (Arduino-ESP32 core 3.x)
#include <NimBLEDevice.h>

// NimBLE-Arduino 2.x uses explicit address type (public/random) in NimBLEAddress.
// Keep buildable even if headers don't export these macros for some reason.
#ifndef BLE_ADDR_PUBLIC
#define BLE_ADDR_PUBLIC 0
#endif
#ifndef BLE_ADDR_RANDOM
#define BLE_ADDR_RANDOM 1
#endif

#include "RelayController.h"
#include "LogicController.h"
#include "RgbLedController.h"
#include "ThermometerController.h"

// ---------- Files ----------
static const char* BLE_CFG_PATH = "/ble.json";
static const char* BLE_CFG_BAK_PATH = "/ble.json.bak";
static const char* BLE_PAIRED_PATH = "/ble_paired.json";
static const char* BLE_PAIRED_BAK_PATH = "/ble_paired.json.bak";

// ---------- UUIDs ----------
static NimBLEUUID UUID_SVC_CTRL("7b7c0001-3a2b-4f2a-8bb0-8d2c2c1a0001");
static NimBLEUUID UUID_CH_STATUS("7b7c0002-3a2b-4f2a-8bb0-8d2c2c1a0001"); // notify+read
static NimBLEUUID UUID_CH_CMD("7b7c0003-3a2b-4f2a-8bb0-8d2c2c1a0001");    // write(+resp)
static NimBLEUUID UUID_CH_ACK("7b7c0004-3a2b-4f2a-8bb0-8d2c2c1a0001");    // notify

// Meteo service/char (příprava – můžeš sladit s vlastním senzorem)
static NimBLEUUID UUID_SVC_METEO("7b7c1001-3a2b-4f2a-8bb0-8d2c2c1a1001");
static NimBLEUUID UUID_CH_METEO("7b7c1002-3a2b-4f2a-8bb0-8d2c2c1a1001");  // notify

// ---------- Config ----------
struct BleConfig {
    bool enabled = true;

    // server (pro displej)
    String deviceName = "ESP32-S3 HeatCtrl";
    bool advertise = true;

    // security
    String securityMode = "bonding"; // "off" | "bonding" | "passkey"
    uint32_t passkey = 123456;       // pro "passkey" (MITM)
    bool allowlistEnforced = true;   // mimo pairing window povol jen zařízení v allowlistu

    // meteo client
    bool meteoEnabled = true;
    String meteoMac = "";            // "AA:BB:CC:DD:EE:FF" (pokud je prázdné a auto-discovery je ON, pokusí se zařízení najít)
    bool meteoAutoDiscover = true;   // scan a vybere nejlepší RSSI meteostanici, pokud meteoMac není nastavené
    bool meteoAutoSave = true;       // po nalezení uloží meteoMac do /ble.json a přidá do allowlistu
    bool meteoAutoPair = false;      // pokud je true, při scan nálezu automaticky uloží meteoMac + allowlist (i mimo pairing)

    // scan tuning
    bool meteoFastScan = true;       // agresivnější scan parametry pro rychlejší nalezení/refresh
    bool meteoStopScanOnMatch = true; // ukonči scan dříve při dobrém kandidátovi / přesném match
    int meteoPairMinRssi = -99;      // RSSI threshold pro early-stop při pairing (typicky -85..-70)
    uint16_t meteoScanInterval = 97; // default scan interval (units depend on NimBLE)
    uint16_t meteoScanWindow = 37;
    uint16_t meteoFastScanInterval = 60;
    uint16_t meteoFastScanWindow = 45;

    // connect tuning
    uint32_t meteoConnectTimeoutMs = 900; // kratší timeout = méně blokování v loopu
    uint32_t meteoDiscoverIntervalMs = 15000; // jak často opakovat scan, když se nedaří nic najít
    uint32_t meteoScanMs = 6000;     // délka scan okna
    uint32_t meteoReconnectMs = 2000;
    int meteoMaxConnectFails = 3;    // 0 = unlimited
    uint32_t meteoCooldownMs = 30000;
    uint32_t schemaVersion = 1;
};

static BleConfig g_cfg;

// ---------- State ----------
static NimBLEServer* g_server = nullptr;
static uint16_t g_serverConnCount = 0;
static uint32_t g_serverLastConnectMs = 0;
static uint32_t g_serverLastDisconnectMs = 0;
static NimBLEService* g_ctrlSvc = nullptr;
static NimBLECharacteristic* g_statusCh = nullptr;
static NimBLECharacteristic* g_cmdCh = nullptr;
static NimBLECharacteristic* g_ackCh = nullptr;
static bool g_bleInitialized = false;
static bool g_bleNamePendingRestart = false;

static bool g_pairingWindow = false;
static uint32_t g_pairingUntilMs = 0;
static String g_pairingRoleHint = "";

// meteo pairing: during pairing window (roleHint="meteo") we allow scan/connect even if meteoEnabled=false
static bool g_meteoPairingActive = false;
static uint32_t g_meteoPairingUntilMs = 0;

static String g_lastAck = "{}";
static uint32_t g_lastStatusNotifyMs = 0;

struct PairedDevice {
    String mac;
    String name;
    String role;     // "display" / "meteo" / "other"
    uint32_t addedAt;
};

static std::vector<PairedDevice> g_allow;

// meteo readings
static bool g_meteoFix = false;
static int16_t g_meteoTempX10 = 0;
static uint8_t g_meteoHum = 0;
static uint16_t g_meteoPress = 0;
static int8_t g_meteoTrend = 0;
static uint32_t g_meteoLastMs = 0;

// client
static NimBLEClient* g_meteoClient = nullptr;
static NimBLERemoteCharacteristic* g_meteoRemoteCh = nullptr;
static uint32_t g_meteoNextActionMs = 0;
static bool g_meteoScanning = false;
static uint32_t g_meteoNextDiscoverMs = 0;
// Keep frequently-changing MAC addresses in fixed buffers to reduce heap fragmentation.
static char g_meteoLastSeenMac[18] = {0};
static String g_meteoLastSeenName = "";
static int g_meteoLastSeenRssi = -999;
static int g_meteoLastSeenAddrType = -1;
static uint32_t g_meteoLastSeenMs = 0;
static char g_meteoRuntimeMac[18] = {0};
static int g_meteoRuntimeAddrType = -1;
static uint32_t g_meteoLastFixMs = 0;
// Fast bootstrap: after S3 reboot we want the first meteo fix ASAP.
// Keep retries aggressive until the first valid frame is received.
static uint32_t g_bleBootMs = 0;
static uint32_t g_meteoFastUntilMs = 0;
static bool g_meteoEverFix = false;
static uint8_t g_meteoFailCount = 0;
static uint8_t g_meteoConnectFails = 0;
static bool g_meteoDiscoverRequested = false;
static bool g_meteoRefreshOnlyRequested = false;
static bool g_meteoRefreshOnlyActive = false;
static char g_meteoRefreshTargetMac[18] = {0};
static bool g_meteoConnecting = false;
static uint32_t g_meteoLastConnectAttemptMs = 0;
static uint8_t g_meteoAddrTryIndex = 0; // rotate address type attempts to avoid long blocking sequences
static int g_meteoLastDisconnectReason = -1;
static bool g_meteoAutoSavePending = false;
static char g_meteoAutoSaveMac[18] = {0};
static String g_meteoAutoSaveName = "";
static uint32_t g_meteoSuspendUntilMs = 0;
static uint32_t g_meteoLastSuspendLogMs = 0;
static uint32_t g_meteoNextBcastScanMs = 0;
static uint32_t g_meteoLastBcastMs = 0;
static uint32_t g_meteoBcastWindowStartMs = 0;
static uint8_t  g_meteoBcastFramesInWindow = 0;
static bool     g_meteoPassiveBroadcastOnly = false; // if true and broadcast is stable, never connect (pure passive mode)

// scan best candidate
static char g_scanBestMac[18] = {0};
static String g_scanBestName = "";
static int g_scanBestRssi = -999;
static int g_scanBestAddrType = -1;

// stop scan early when we already have a good/target match
static bool g_scanStopIssued = false;

static const uint8_t METEO_FAILS_BEFORE_SCAN = 3;
static const uint32_t METEO_STALE_MS = 60000;
static const uint32_t METEO_MIN_CONNECT_INTERVAL_MS = 2500;
static const uint32_t METEO_SUSPEND_LOG_INTERVAL_MS = 5000;
static const char* METEO_NAME_PREFIX = "ESP-Meteostanice";
static const uint16_t METEO_MFG_ID = 0xFFFF;                 // Manufacturer ID for broadcast frames
static const uint8_t  METEO_BCAST_VER = 1;                  // Broadcast payload version
static const uint32_t METEO_BCAST_PERIOD_MS = 5000;         // C3 broadcast interval (expected)
static const uint32_t METEO_BCAST_STABLE_MAX_GAP_MS = 12000; // If we see frames with gaps <= this, consider "stable"
static const uint8_t  METEO_BCAST_STABLE_MIN_FRAMES = 3;    // Frames needed to enter passive mode
static const uint32_t METEO_BCAST_STABLE_WINDOW_MS = 20000; // Window for stability evaluation
static const uint32_t METEO_BCAST_SCAN_PERIOD_MS = 6000;    // Periodic short scan on S3 to catch advertisements

// ---------- Helpers ----------
static String nowIso() {
    // bez RTC: jen uptime v sekundách, ale UI stačí
    uint32_t s = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "uptime+%lus", (unsigned long)s);
    return String(buf);
}

static bool meteoFastBootstrapActive(uint32_t now) {
    if (g_meteoEverFix) return false;
    if (!g_meteoFastUntilMs) return false;
    return (int32_t)(now - g_meteoFastUntilMs) < 0;
}

static bool meteoBroadcastStable(uint32_t now) {
    if (!g_meteoLastBcastMs) return false;
    if ((now - g_meteoLastBcastMs) > METEO_BCAST_STABLE_MAX_GAP_MS) return false;
    return g_meteoPassiveBroadcastOnly;
}

static uint32_t meteoComputeRetryDelayMs(uint32_t now, uint8_t fails) {
    // Add a small jitter to avoid phase-lock with WiFi/ETH bursts.
    const uint32_t jitter = (uint32_t)(millis() & 0x7FU); // 0..127ms

    // Fast bootstrap: aggressive backoff until we get the first fix.
    if (meteoFastBootstrapActive(now)) {
        // 250, 500, 1000, 1500, 1500...
        uint32_t d = 250UL << (fails > 3 ? 3 : fails);
        if (d > 1500UL) d = 1500UL;
        return d + jitter;
    }

    // If the reading is stale, still retry faster than the normal reconnect.
    if (!g_meteoLastFixMs || (now - g_meteoLastFixMs) > 30000UL) {
        // 750, 1500, 3000, then cap to configured reconnect
        uint32_t d = 750UL << (fails > 2 ? 2 : fails);
        const uint32_t cap = g_cfg.meteoReconnectMs ? g_cfg.meteoReconnectMs : 8000UL;
        if (d > cap) d = cap;
        return d + jitter;
    }

    return (g_cfg.meteoReconnectMs ? g_cfg.meteoReconnectMs : 8000UL) + jitter;
}

static bool fsReadAll(const char* path, String& out) {
    if (!fsIsReady()) return false;
    fsLock();
    if (!LittleFS.exists(path)) {
        fsUnlock();
        return false;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        fsUnlock();
        return false;
    }
    out = f.readString();
    f.close();
    fsUnlock();
    out.trim();
    return out.length() > 0;
}

static bool macEquals(const String& a, const String& b) {
    // Avoid temporary String copies to reduce heap churn.
    return a.equalsIgnoreCase(b);
}

static void normalizeMacToBuf(const char* in, char out[18]) {
    // Normalize to uppercase and strip whitespace. Output is NUL-terminated.
    if (!in) {
        out[0] = 0;
        return;
    }
    while (*in && isspace((unsigned char)*in)) in++;
    size_t j = 0;
    for (; *in && j < 17; ++in) {
        char c = *in;
        if (isspace((unsigned char)c)) continue;
        if (c >= 'a' && c <= 'f') c = (char)(c - 32);
        out[j++] = c;
    }
    out[j] = 0;
}

static bool macEqualsBufStr(const char aNormUpper[18], const String& b) {
    if (!aNormUpper || !aNormUpper[0]) return b.length() == 0;
    char bNorm[18];
    normalizeMacToBuf(b.c_str(), bNorm);
    return strncmp(aNormUpper, bNorm, 18) == 0;
}

static String normalizeMac(String mac) {
    mac.trim();
    mac.toUpperCase();
    return mac;
}

static const char* meteoTargetMacCStr() {
    return g_meteoRuntimeMac[0] ? g_meteoRuntimeMac : g_cfg.meteoMac.c_str();
}

static bool meteoDiscoverAllowed() {
    return g_cfg.meteoAutoDiscover && (!g_cfg.meteoMac.length() || g_meteoDiscoverRequested);
}

static bool meteoAutoDiscoverOnFailAllowed() {
    return g_cfg.meteoAutoDiscover && !g_cfg.meteoMac.length();
}

static bool meteoCooldownActive(uint32_t now) {
    return g_meteoSuspendUntilMs != 0 && (int32_t)(now - g_meteoSuspendUntilMs) < 0;
}

static void meteoResetRetryState(const char* reason) {
    g_meteoSuspendUntilMs = 0;
    g_meteoLastSuspendLogMs = 0;
    g_meteoConnectFails = 0;
    g_meteoFailCount = 0;
    g_meteoNextActionMs = 0;
    g_meteoLastConnectAttemptMs = 0;
    g_meteoDiscoverRequested = false;
    g_meteoRefreshOnlyRequested = false;
    g_meteoRefreshOnlyActive = false;
    g_meteoRefreshTargetMac[0] = 0;
    g_meteoAddrTryIndex = 0;
    if (reason && strlen(reason)) {
        Serial.printf("[BLE] Meteo retry state reset (%s)\n", reason);
    }
}

static bool meteoSuspendIfNeeded(uint32_t now, uint8_t failCount) {
    if (g_cfg.meteoMaxConnectFails <= 0) return false;
    if (failCount < g_cfg.meteoMaxConnectFails) return false;
    g_meteoSuspendUntilMs = now + g_cfg.meteoCooldownMs;
    g_meteoNextActionMs = g_meteoSuspendUntilMs;
    g_meteoDiscoverRequested = false;
    g_meteoRefreshOnlyRequested = false;
    g_meteoRefreshOnlyActive = false;
    g_meteoRefreshTargetMac[0] = 0;
    Serial.printf("[BLE] Meteo suspended for %lus after %u fails\n",
                  (unsigned long)(g_cfg.meteoCooldownMs / 1000UL),
                  failCount);
    return true;
}

static bool isInAllowlist(const String& mac) {
    for (auto& d : g_allow) if (macEquals(d.mac, mac)) return true;
    return false;
}

static void loadAllowlistDefaults() {
    g_allow.clear();
}

static bool loadAllowlistFS() {
    String s;
    bool restored = false;
    if (!fsReadAll(BLE_PAIRED_PATH, s)) {
        if (!fsReadAll(BLE_PAIRED_BAK_PATH, s)) {
            loadAllowlistDefaults();
            return false;
        }
        restored = true;
    }
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, s)) {
        if (!fsReadAll(BLE_PAIRED_BAK_PATH, s) || deserializeJson(doc, s)) {
            loadAllowlistDefaults();
            return false;
        }
        restored = true;
    }
    g_allow.clear();
    JsonArray arr = doc["devices"].as<JsonArray>();
    for (JsonVariant v : arr) {
        JsonObject o = v.as<JsonObject>();
        PairedDevice d;
        const char* mac = o["mac"] | "";
        const char* name = o["name"] | "";
        const char* role = o["role"] | "other";
        d.mac = normalizeMac(String(mac));
        d.name = String(name);
        d.role = String(role);
        d.addedAt = (uint32_t)(o["addedAt"] | 0);
        if (d.mac.length()) g_allow.push_back(d);
    }
    if (restored) {
        fsWriteAtomicKeepBak(BLE_PAIRED_PATH, s, BLE_PAIRED_BAK_PATH, true);
    }
    return true;
}

static bool saveAllowlistFS() {
    DynamicJsonDocument doc(8192);
    JsonArray arr = doc.createNestedArray("devices");
    for (auto& d : g_allow) {
        JsonObject o = arr.createNestedObject();
        o["mac"] = normalizeMac(d.mac);
        o["name"] = d.name;
        o["role"] = d.role;
        o["addedAt"] = d.addedAt;
    }
    doc["schemaVersion"] = 1;
    String out;
    serializeJson(doc, out);
    return fsWriteAtomicKeepBak(BLE_PAIRED_PATH, out, BLE_PAIRED_BAK_PATH, true);
}

static void loadConfigDefaults() {
    g_cfg = BleConfig();
}

static bool loadConfigFS() {
    String s;
    bool restored = false;
    if (!fsReadAll(BLE_CFG_PATH, s)) {
        if (!fsReadAll(BLE_CFG_BAK_PATH, s)) {
            loadConfigDefaults();
            return false;
        }
        restored = true;
    }
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, s)) {
        if (!fsReadAll(BLE_CFG_BAK_PATH, s) || deserializeJson(doc, s)) {
            loadConfigDefaults();
            return false;
        }
        restored = true;
    }

    g_cfg.enabled = doc["enabled"] | true;
    g_cfg.deviceName = String((const char*)(doc["deviceName"] | "ESP32-S3 HeatCtrl"));
    g_cfg.advertise = doc["advertise"] | true;

    g_cfg.securityMode = String((const char*)(doc["securityMode"] | "bonding"));
    g_cfg.passkey = (uint32_t)(doc["passkey"] | 123456);
    g_cfg.allowlistEnforced = doc["allowlistEnforced"] | true;

    g_cfg.meteoEnabled = doc["meteoEnabled"] | g_cfg.meteoEnabled;
    g_cfg.meteoMac = normalizeMac(String((const char*)(doc["meteoMac"] | "")));
    g_cfg.meteoAutoDiscover = doc["meteoAutoDiscover"] | true;
    g_cfg.meteoAutoSave = doc["meteoAutoSave"] | true;
    g_cfg.meteoAutoPair = doc["meteoAutoPair"] | false;

    g_cfg.meteoFastScan = doc["meteoFastScan"] | true;
    g_cfg.meteoStopScanOnMatch = doc["meteoStopScanOnMatch"] | true;
    g_cfg.meteoPairMinRssi = (int)(doc["meteoPairMinRssi"] | -85);
    g_cfg.meteoScanInterval = (uint16_t)(doc["meteoScanInterval"] | 97);
    g_cfg.meteoScanWindow = (uint16_t)(doc["meteoScanWindow"] | 37);
    g_cfg.meteoFastScanInterval = (uint16_t)(doc["meteoFastScanInterval"] | 60);
    g_cfg.meteoFastScanWindow = (uint16_t)(doc["meteoFastScanWindow"] | 45);

    g_cfg.meteoConnectTimeoutMs = (uint32_t)(doc["meteoConnectTimeoutMs"] | 900);
    g_cfg.meteoDiscoverIntervalMs = (uint32_t)(doc["meteoDiscoverIntervalMs"] | 10000);
    g_cfg.meteoScanMs = (uint32_t)(doc["meteoScanMs"] | 4000);
    g_cfg.meteoReconnectMs = (uint32_t)(doc["meteoReconnectMs"] | 8000);
    g_cfg.meteoMaxConnectFails = (int)(doc["meteoMaxConnectFails"] | 3);
    g_cfg.meteoCooldownMs = (uint32_t)(doc["meteoCooldownMs"] | 30000);
    g_cfg.schemaVersion = (uint32_t)(doc["schemaVersion"] | 1);
    if (restored) {
        fsWriteAtomicKeepBak(BLE_CFG_PATH, s, BLE_CFG_BAK_PATH, true);
    }
    return true;
}

static bool saveConfigFS() {
    DynamicJsonDocument doc(8192);
    doc["enabled"] = g_cfg.enabled;
    doc["deviceName"] = g_cfg.deviceName;
    doc["advertise"] = g_cfg.advertise;
    doc["securityMode"] = g_cfg.securityMode;
    doc["passkey"] = g_cfg.passkey;
    doc["allowlistEnforced"] = g_cfg.allowlistEnforced;
    doc["serverConnectedCount"] = (uint32_t)(g_server ? g_server->getConnectedCount() : g_serverConnCount);
    doc["serverConnected"] = ((g_server ? g_server->getConnectedCount() : g_serverConnCount) > 0);
    doc["meteoEnabled"] = g_cfg.meteoEnabled;
    doc["meteoMac"] = g_cfg.meteoMac;
    doc["meteoAutoDiscover"] = g_cfg.meteoAutoDiscover;
    doc["meteoAutoSave"] = g_cfg.meteoAutoSave;
    doc["meteoAutoPair"] = g_cfg.meteoAutoPair;
    doc["meteoFastScan"] = g_cfg.meteoFastScan;
    doc["meteoStopScanOnMatch"] = g_cfg.meteoStopScanOnMatch;
    doc["meteoPairMinRssi"] = g_cfg.meteoPairMinRssi;
    doc["meteoScanInterval"] = g_cfg.meteoScanInterval;
    doc["meteoScanWindow"] = g_cfg.meteoScanWindow;
    doc["meteoFastScanInterval"] = g_cfg.meteoFastScanInterval;
    doc["meteoFastScanWindow"] = g_cfg.meteoFastScanWindow;
    doc["meteoConnectTimeoutMs"] = g_cfg.meteoConnectTimeoutMs;
    doc["meteoDiscoverIntervalMs"] = g_cfg.meteoDiscoverIntervalMs;
    doc["meteoScanMs"] = g_cfg.meteoScanMs;
    doc["meteoReconnectMs"] = g_cfg.meteoReconnectMs;
    doc["meteoMaxConnectFails"] = g_cfg.meteoMaxConnectFails;
    doc["meteoCooldownMs"] = g_cfg.meteoCooldownMs;
    doc["schemaVersion"] = g_cfg.schemaVersion;

    String out;
    serializeJson(doc, out);
    return fsWriteAtomicKeepBak(BLE_CFG_PATH, out, BLE_CFG_BAK_PATH, true);
}

static void setAck(const String& json) {
    g_lastAck = json;
    if (g_ackCh) {
        g_ackCh->setValue(g_lastAck.c_str());
        g_ackCh->notify(true);
    }
}

static String macToString(const NimBLEAddress& a) {
    return String(a.toString().c_str());
}

static const char* addrTypeToStr(int addrType) {
    if (addrType == BLE_ADDR_PUBLIC) return "public";
    if (addrType == BLE_ADDR_RANDOM) return "random";
    return "unknown";
}

static void meteoScanRequestStop(const char* reason) {
    if (g_scanStopIssued) return;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan) return;
    if (!scan->isScanning()) return;
    g_scanStopIssued = true;
    if (reason && reason[0]) {
        Serial.printf("[BLE] Meteo scan: early stop (%s)\n", reason);
    }
    scan->stop();
}


// ---------- Meteo auto-discovery (scan) ----------
static void meteoScanFinalize() {
    g_meteoScanning = false;
    g_scanStopIssued = false;

    if (g_scanBestMac[0]) {
        strncpy(g_meteoLastSeenMac, g_scanBestMac, sizeof(g_meteoLastSeenMac));
        g_meteoLastSeenMac[sizeof(g_meteoLastSeenMac) - 1] = 0;
        g_meteoLastSeenName = g_scanBestName;
        g_meteoLastSeenRssi = g_scanBestRssi;
        g_meteoLastSeenAddrType = g_scanBestAddrType;
        g_meteoLastSeenMs = millis();

        const char* scanMode = g_meteoRefreshOnlyActive ? "refresh" : (g_meteoPairingActive ? "pairing" : "auto-discovery");
        Serial.printf("[BLE] Meteo %s: found %s (RSSI %d, name=%s)\n",
                      scanMode,
                      g_scanBestMac, g_scanBestRssi, g_scanBestName.c_str());
        Serial.printf("[BLE] Meteo %s: addrType=%s\n", scanMode, addrTypeToStr(g_scanBestAddrType));

        const bool shouldAutoCommit = (g_meteoPairingActive) || (g_cfg.meteoAutoPair && !g_cfg.meteoMac.length());

        if (!g_meteoRefreshOnlyActive) {
            strncpy(g_meteoRuntimeMac, g_scanBestMac, sizeof(g_meteoRuntimeMac));
            g_meteoRuntimeMac[sizeof(g_meteoRuntimeMac) - 1] = 0;
            g_meteoRuntimeAddrType = g_scanBestAddrType;
            if (shouldAutoCommit) {
                // Pairing / auto-pair: commit immediately (do not wait for connection).
                const String macStr = normalizeMac(String(g_scanBestMac));
                g_cfg.meteoMac = macStr;
                g_meteoAutoSavePending = false;
                g_meteoAutoSaveMac[0] = 0;
                g_meteoAutoSaveName = "";

                if (!isInAllowlist(macStr)) {
                    PairedDevice d;
                    d.mac = macStr;
                    d.name = g_scanBestName.length() ? g_scanBestName : "meteo";
                    d.role = "meteo";
                    d.addedAt = (uint32_t)(millis() / 1000);
                    g_allow.push_back(d);
                    saveAllowlistFS();
                }
                saveConfigFS();

                if (g_meteoPairingActive) {
                    g_meteoPairingActive = false;
                    g_meteoPairingUntilMs = 0;
                    // close pairing window as soon as we have a meteo device
                    g_pairingWindow = false;
                    g_pairingRoleHint = "";
                    g_pairingUntilMs = 0;
                    Serial.printf("[BLE] Meteo paired: %s (saved to config + allowlist)\n", macStr.c_str());
                } else {
                    Serial.printf("[BLE] Meteo auto-pair: %s (saved to config + allowlist)\n", macStr.c_str());
                }
            } else if (g_cfg.meteoAutoSave) {
                g_meteoAutoSavePending = true;
                strncpy(g_meteoAutoSaveMac, g_scanBestMac, sizeof(g_meteoAutoSaveMac));
                g_meteoAutoSaveMac[sizeof(g_meteoAutoSaveMac) - 1] = 0;
                g_meteoAutoSaveName = g_scanBestName;
            } else {
                g_meteoAutoSavePending = false;
                g_meteoAutoSaveMac[0] = 0;
                g_meteoAutoSaveName = "";
            }
            g_meteoFailCount = 0;
            g_meteoConnectFails = 0;
        }

        // try connect ASAP
        g_meteoNextActionMs = millis() + 200;
        g_meteoNextDiscoverMs = 0;
    } else {
        if (g_meteoRefreshOnlyActive) {
            // Do NOT treat a scan miss as a connection failure.
            // The target may simply not have advertised during this short window.
            // We will try to connect anyway (client connect tries both PUBLIC and RANDOM addr types).
            const uint32_t now = millis();
// Pure passive mode: if broadcast frames are stable, do not connect (unless pairing is active).
if (g_meteoPassiveBroadcastOnly && g_meteoLastBcastMs && (now - g_meteoLastBcastMs) > METEO_BCAST_STABLE_MAX_GAP_MS) {
    // Broadcast went stale -> allow connections again.
    g_meteoPassiveBroadcastOnly = false;
    g_meteoBcastWindowStartMs = 0;
    g_meteoBcastFramesInWindow = 0;
}
if (!g_meteoPairingActive && g_meteoPassiveBroadcastOnly && g_meteoLastBcastMs && (now - g_meteoLastBcastMs) <= METEO_BCAST_STABLE_MAX_GAP_MS) {
    // If we happen to be connected, disconnect to keep the link purely passive.
    if (g_meteoClient && g_meteoClient->isConnected()) {
        g_meteoClient->disconnect();
    }
    g_meteoNextActionMs = now + 1000;
    return false;
}

            g_meteoNextActionMs = now + 250;
            Serial.printf("[BLE] Meteo refresh: target %s not seen (next=%lums)\n",
                          g_meteoRefreshTargetMac,
                          (unsigned long)g_meteoNextActionMs);
        } else {
            Serial.println(F("[BLE] Meteo auto-discovery: nothing found"));
            const uint32_t now = millis();
            g_meteoNextDiscoverMs = now + (g_cfg.meteoDiscoverIntervalMs ? g_cfg.meteoDiscoverIntervalMs : 10000);
        }
    }

    g_meteoDiscoverRequested = false;
    g_meteoRefreshOnlyRequested = false;
    g_meteoRefreshOnlyActive = false;
    g_meteoRefreshTargetMac[0] = 0;

    // reset best candidate
    g_scanBestMac[0] = 0;
    g_scanBestName = "";
    g_scanBestRssi = -999;
    g_scanBestAddrType = -1;
}


static bool meteoTryParseBroadcast(const NimBLEAdvertisedDevice* dev) {
    if (!dev) return false;

    const std::string md = dev->getManufacturerData();
    if (md.size() < (2 + 1 + 2 + 1 + 2)) return false;

    const uint8_t* b = (const uint8_t*)md.data();
    const uint16_t companyId = (uint16_t)(b[0] | (b[1] << 8));
    if (companyId != METEO_MFG_ID) return false;

    const uint8_t ver = b[2];
    if (ver != METEO_BCAST_VER) return false;

    const int16_t tempX10 = (int16_t)(b[3] | (b[4] << 8));
    const uint8_t hum = b[5];
    const uint16_t pressHpa = (uint16_t)(b[6] | (b[7] << 8));

    const uint32_t now = millis();

    // Update latest reading (broadcast acts as a "fix" without connecting)
    g_meteoTempX10 = tempX10;
    g_meteoHum = hum;
    g_meteoPress = pressHpa;
    g_meteoTrend = 0;
    g_meteoLastMs = now;
    g_meteoLastFixMs = now;
    g_meteoLastBcastMs = now;
    g_meteoFix = true;
    g_meteoEverFix = true;

    // Update last-seen metadata (useful for diagnostics)
    const String mac = normalizeMac(macToString(dev->getAddress()));
    strncpy(g_meteoLastSeenMac, mac.c_str(), sizeof(g_meteoLastSeenMac) - 1);
    g_meteoLastSeenMac[sizeof(g_meteoLastSeenMac) - 1] = 0;
    g_meteoLastSeenName = String(dev->getName().c_str());
    g_meteoLastSeenRssi = dev->getRSSI();
    g_meteoLastSeenMs = now;
    g_meteoLastSeenAddrType = dev->getAddressType();

    // Evaluate "stable broadcast" -> enable pure passive mode (never connect).
    // We require N frames within a time window and gaps not exceeding METEO_BCAST_STABLE_MAX_GAP_MS.
    if (!g_meteoBcastWindowStartMs || (now - g_meteoBcastWindowStartMs) > METEO_BCAST_STABLE_WINDOW_MS) {
        g_meteoBcastWindowStartMs = now;
        g_meteoBcastFramesInWindow = 1;
    } else {
        if (g_meteoBcastFramesInWindow < 250) g_meteoBcastFramesInWindow++;
    }
    if (g_meteoBcastFramesInWindow >= METEO_BCAST_STABLE_MIN_FRAMES) {
        g_meteoPassiveBroadcastOnly = true;
    }

    return true;
}

// NimBLE-Arduino 2.x: NimBLEAdvertisedDeviceCallbacks replaced by NimBLEScanCallbacks,
// scan end callback moved to onScanEnd().
class MeteoScanCallbacks : public NimBLEScanCallbacks {
public:
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev) return;

        const bool matchesService = dev->isAdvertisingService(UUID_SVC_METEO);
        const String name = String(dev->getName().c_str());
        const bool matchesName = name.startsWith(METEO_NAME_PREFIX);
        if (!matchesService && !matchesName) return;

        // Passive mode: parse meteo broadcast frames from advertising (Manufacturer Data).
        // This provides meteo readings without connecting.
        meteoTryParseBroadcast(dev);

        const int rssi = dev->getRSSI();
        const String mac = normalizeMac(macToString(dev->getAddress()));

        // refresh-only: ignore others, and stop scan as soon as exact target is seen
        if (g_meteoRefreshOnlyActive && g_meteoRefreshTargetMac[0]) {
            if (!macEqualsBufStr(g_meteoRefreshTargetMac, mac)) return;
        }

        if (rssi > g_scanBestRssi) {
            g_scanBestRssi = rssi;
            normalizeMacToBuf(mac.c_str(), g_scanBestMac);
            g_scanBestName = name;
            g_scanBestAddrType = dev->getAddressType();
        }

        if (g_cfg.meteoStopScanOnMatch) {
            if (g_meteoRefreshOnlyActive && g_meteoRefreshTargetMac[0] && macEqualsBufStr(g_meteoRefreshTargetMac, mac)) {
                meteoScanRequestStop("refresh exact match");
            } else if (g_meteoPairingActive && rssi >= g_cfg.meteoPairMinRssi) {
                meteoScanRequestStop("pairing: good RSSI");
            } else if (g_cfg.meteoAutoPair && !g_cfg.meteoMac.length() && rssi >= g_cfg.meteoPairMinRssi) {
                meteoScanRequestStop("auto-pair: good RSSI");
            }
        }
    }

    void onScanEnd(const NimBLEScanResults& scanResults, int reason) override {
        (void)scanResults;
        (void)reason;
        meteoScanFinalize();
    }
};

static MeteoScanCallbacks g_meteoScanCbs;

static bool meteoStartScanIfNeeded() {
    const bool pairing = g_meteoPairingActive;
    if (!g_cfg.meteoEnabled && !pairing) return false;
    const bool refreshOnly = g_meteoRefreshOnlyRequested;
    if (!refreshOnly && !pairing && !meteoDiscoverAllowed()) return false;
    if (!refreshOnly && !pairing && !g_meteoDiscoverRequested && meteoTargetMacCStr()[0]) return false; // prefer stored/runtime MAC
    if (g_meteoScanning) return false;

    const uint32_t now = millis();
    if (meteoCooldownActive(now)) return false;
    if (g_meteoNextDiscoverMs && (int32_t)(now - g_meteoNextDiscoverMs) < 0) return false;

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan) return false;

    if (refreshOnly && !meteoTargetMacCStr()[0]) return false;

    g_scanBestMac[0] = 0;
    g_scanBestName = "";
    g_scanBestRssi = -999;
    g_scanBestAddrType = -1;

    // callbacks-only scan (do not store results to save RAM)
    scan->setMaxResults(0);
    scan->setScanCallbacks(&g_meteoScanCbs, false);
    scan->setActiveScan(true);

    const bool useFast = g_cfg.meteoFastScan && (refreshOnly || pairing || (g_cfg.meteoAutoPair && !g_cfg.meteoMac.length()));
    const uint16_t interval = useFast ? g_cfg.meteoFastScanInterval : g_cfg.meteoScanInterval;
    uint16_t window = useFast ? g_cfg.meteoFastScanWindow : g_cfg.meteoScanWindow;
    // During bootstrapping we prefer a 100% scan duty cycle to discover the meteo sensor ASAP.
    if (useFast && meteoFastBootstrapActive(now)) {
        window = interval;
    }
    scan->setInterval(interval);
    scan->setWindow(window);

    g_scanStopIssued = false;

    uint32_t requestMs = g_cfg.meteoScanMs;
    if (refreshOnly) {
        const uint32_t fastRefresh = meteoFastBootstrapActive(now) ? 900UL : 1500UL;
        requestMs = min(requestMs, useFast ? fastRefresh : 3000UL);
    }
    if (pairing) requestMs = min(requestMs, 2500UL);
    uint32_t durSec = (requestMs + 999) / 1000;
    const uint32_t minDurSec = (useFast && meteoFastBootstrapActive(now)) ? 1UL : 2UL;
    if (durSec < minDurSec) durSec = minDurSec;
    if (durSec > 30) durSec = 30;

    const bool scanActive = scan->isScanning() || g_meteoScanning;
    const char* scanMode = refreshOnly ? "refresh" : (pairing ? "pairing" : "auto-discovery");
    Serial.printf("[BLE] Meteo %s: scan request %lums, used %lus, active=%s\n",
                  scanMode, (unsigned long)requestMs, (unsigned long)durSec, scanActive ? "yes" : "no");
    if (scanActive) {
        g_meteoScanning = true;
        return false;
    }

    g_meteoRefreshOnlyActive = refreshOnly;
    normalizeMacToBuf(meteoTargetMacCStr(), g_meteoRefreshTargetMac);

    g_meteoScanning = scan->start(durSec, false, true);
    if (!g_meteoScanning) {
        // failed to start scan -> retry later
        g_meteoNextDiscoverMs = now + (g_cfg.meteoDiscoverIntervalMs ? g_cfg.meteoDiscoverIntervalMs : 10000);
        g_meteoRefreshOnlyActive = false;
        g_meteoRefreshTargetMac[0] = 0;
        return false;
    }

    return true;
}

// ---------- Command handling (from display/app) ----------
static void handleCommandJson(const String& s) {
    DynamicJsonDocument doc(3072);
    if (deserializeJson(doc, s)) {
        setAck("{\"ok\":false,\"err\":\"bad_json\"}");
        return;
    }
    const char* cmd = doc["cmd"] | "";
    uint32_t seq = doc["seq"] | 0;

    if (!strcmp(cmd, "relay_set")) {
        int id = doc["id"] | -1;
        int state = doc["state"] | -1;
        if (id < 1 || id > 8 || (state != 0 && state != 1)) {
            DynamicJsonDocument a(256);
            a["ok"]=false; a["seq"]=seq; a["err"]="bad_args";
            String out; serializeJson(a,out);
            setAck(out);
            return;
        }
        RelayId rid = static_cast<RelayId>(id-1);
        relaySet(rid, state ? true : false);

        DynamicJsonDocument a(256);
        a["ok"]=true; a["seq"]=seq;
        String out; serializeJson(a,out);
        setAck(out);
        return;
    }

    if (!strcmp(cmd, "relay_toggle")) {
        int id = doc["id"] | -1;
        if (id < 1 || id > 8) {
            DynamicJsonDocument a(256);
            a["ok"]=false; a["seq"]=seq; a["err"]="bad_args";
            String out; serializeJson(a,out);
            setAck(out);
            return;
        }
        RelayId rid = static_cast<RelayId>(id-1);
        relayToggle(rid);

        DynamicJsonDocument a(256);
        a["ok"]=true; a["seq"]=seq;
        String out; serializeJson(a,out);
        setAck(out);
        return;
    }

    if (!strcmp(cmd, "mode_set")) {
        int mode = doc["mode"] | -1;
        if (mode < 0 || mode > 4) {
            DynamicJsonDocument a(256);
            a["ok"]=false; a["seq"]=seq; a["err"]="bad_args";
            String out; serializeJson(a,out);
            setAck(out);
            return;
        }
        logicSetControlMode(ControlMode::MANUAL);
        logicSetManualMode(static_cast<SystemMode>(mode));
        DynamicJsonDocument a(256);
        a["ok"]=true; a["seq"]=seq;
        String out; serializeJson(a,out);
        setAck(out);
        return;
    }

    // reserved / future
    DynamicJsonDocument a(256);
    a["ok"]=false; a["seq"]=seq; a["err"]="unknown_cmd";
    String out; serializeJson(a,out);
    setAck(out);
}


static void updateBleLed() {
    if (!g_cfg.enabled) {
        rgbLedSetMode(RgbLedMode::BLE_DISABLED);
        return;
    }

    if (g_pairingWindow) {
        rgbLedSetMode(RgbLedMode::BLE_PAIRING);
        return;
    }

    const bool serverConnected = (g_server && g_server->getConnectedCount() > 0);
    const bool meteoConnected = (g_meteoClient && g_meteoClient->isConnected());
    const uint8_t meteoFailureScore = (uint8_t)min(255, g_meteoFailCount + g_meteoConnectFails);
    const bool meteoError = g_cfg.meteoEnabled
        && !meteoConnected
        && !g_meteoScanning
        && meteoFailureScore >= METEO_FAILS_BEFORE_SCAN;

    if (serverConnected || meteoConnected) {
        rgbLedSetMode(RgbLedMode::BLE_CONNECTED);
        return;
    }

    if (g_meteoScanning) {
        rgbLedSetColor(0, 32, 32); // cyan while scanning
        return;
    }

    if (meteoError) {
        rgbLedSetMode(RgbLedMode::ERROR);
        return;
    }

    rgbLedSetMode(RgbLedMode::BLE_IDLE);
}

static void applyBleSecurityFromConfig(bool log = true) {
    if (g_cfg.securityMode != "off") {
        // bonding always for "bonding" and "passkey", SC only when passkey/MITM is used
        const bool usePasskey = (g_cfg.securityMode == "passkey");
        NimBLEDevice::setSecurityAuth(true, usePasskey, usePasskey); // bonding, MITM, SC
        NimBLEDevice::setSecurityIOCap(g_cfg.securityMode == "passkey" ? BLE_HS_IO_DISPLAY_ONLY : BLE_HS_IO_NO_INPUT_OUTPUT);
        NimBLEDevice::setSecurityPasskey(g_cfg.passkey);
        NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
        NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

        if (log) {
            Serial.printf("[BLE] Security: %s (passkey=%lu)\n", g_cfg.securityMode.c_str(), (unsigned long)g_cfg.passkey);
        }
    } else {
        NimBLEDevice::setSecurityAuth(false, false, false);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
        if (log) {
            Serial.println(F("[BLE] Security: off"));
        }
    }
}

static void applyBleSecurityRelaxedForMeteo() {
    NimBLEDevice::setSecurityAuth(false, false, false);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    Serial.println(F("[BLE] Meteo security: relaxed (no MITM/SC)"));
}

// ---------- BLE Server callbacks ----------
class CmdCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* ch, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        std::string v = ch->getValue();
        String s = String(v.c_str());
        s.trim();
        if (!s.length()) return;
        handleCommandJson(s);
    }
};

static CmdCallbacks g_cmdCb;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
        (void)s;
        const String mac = normalizeMac(macToString(connInfo.getAddress()));
        Serial.printf("[BLE] Client connected: %s\n", mac.c_str());
        g_serverConnCount = (uint16_t)(g_server ? g_server->getConnectedCount() : 1);
        g_serverLastConnectMs = millis();
        updateBleLed();

        // Pokud je otevřené párovací okno, zařadíme zařízení do allowlistu
        // (šifrování/bonding si NimBLE řeší interně dle nastavení securityAuth).
        if (g_pairingWindow && !isInAllowlist(mac)) {
            PairedDevice d;
            d.mac = normalizeMac(mac);
            d.name = "";
            d.role = g_pairingRoleHint.length() ? g_pairingRoleHint : "other";
            d.addedAt = millis() / 1000;
            g_allow.push_back(d);
            saveAllowlistFS();
            Serial.printf("[BLE] Added to allowlist: %s (%s)\n", mac.c_str(), d.role.c_str());
            g_pairingWindow = false;
            g_pairingRoleHint = "";
            g_pairingUntilMs = 0;
        }

        // allowlist enforcement mimo pairing window
        if (g_cfg.allowlistEnforced && g_cfg.securityMode != "off") {
            if (!g_pairingWindow && !isInAllowlist(mac)) {
                Serial.printf("[BLE] Not in allowlist -> disconnect: %s\n", mac.c_str());
                s->disconnect(connInfo.getConnHandle());
            }
        }
    }

    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
        (void)s;
        const String mac = normalizeMac(macToString(connInfo.getAddress()));
        Serial.printf("[BLE] Client disconnected: %s (reason=%d)\n", mac.c_str(), reason);
        g_serverConnCount = (uint16_t)(g_server ? g_server->getConnectedCount() : 0);
        g_serverLastDisconnectMs = millis();
        updateBleLed();
        if (g_cfg.advertise && g_cfg.enabled) {
            NimBLEDevice::startAdvertising();
        }
    }
};

static ServerCallbacks g_srvCb;


// ---------- Meteo client ----------
static void meteoOnNotify(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify) {
    (void)ch; (void)isNotify;
    if (len < 6) return;

    // jednoduchý binární rámec (příprava): int16 temp_x10, uint8 hum, uint16 press, int8 trend
    // min len = 2+1+2+1 = 6
    int16_t t = (int16_t)((data[1] << 8) | data[0]);
    uint8_t h = data[2];
    uint16_t p = (uint16_t)((data[4] << 8) | data[3]);
    int8_t tr = (int8_t)data[5];

    g_meteoTempX10 = t;
    g_meteoHum = h;
    g_meteoPress = p;
    g_meteoTrend = tr;
    g_meteoFix = true;
    g_meteoLastMs = millis();
    g_meteoLastFixMs = g_meteoLastMs;

    // First valid frame after boot: exit fast-bootstrap mode.
    if (!g_meteoEverFix) {
        g_meteoEverFix = true;
        g_meteoFastUntilMs = 0;
    }
    g_meteoFailCount = 0;
    g_meteoConnectFails = 0;
    g_meteoSuspendUntilMs = 0;
    g_meteoDiscoverRequested = false;

    // externí BLE teploměr (konfigurace "Teploměry")
    thermometersBleOnReading("meteo.tempC", (float)g_meteoTempX10 / 10.0f);
}

static void meteoCommitAutoSaveIfPending(const char* connectedMacNormUpper) {
    if (!g_cfg.meteoAutoSave) return;
    if (!g_meteoAutoSavePending) return;
    if (!connectedMacNormUpper || !connectedMacNormUpper[0]) return;
    if (!g_meteoAutoSaveMac[0]) return;
    if (strcmp(connectedMacNormUpper, g_meteoAutoSaveMac) != 0) return;

    g_cfg.meteoMac = String(g_meteoAutoSaveMac);
    g_meteoRuntimeMac[0] = 0;
    String macStr(g_meteoAutoSaveMac);
    if (!isInAllowlist(macStr)) {
        PairedDevice d;
        d.mac = macStr;
        d.name = g_meteoAutoSaveName.length() ? g_meteoAutoSaveName : "meteo";
        d.role = "meteo";
        d.addedAt = (uint32_t)(millis() / 1000);
        g_allow.push_back(d);
        saveAllowlistFS();
    }
    saveConfigFS();

    g_meteoAutoSavePending = false;
    g_meteoAutoSaveMac[0] = 0;
    g_meteoAutoSaveName = "";
}

class MeteoClientCallbacks : public NimBLEClientCallbacks {
    void handleDisconnect(NimBLEClient* pClient, int reason) {
        (void)pClient;
        g_meteoFix = false;
        g_meteoRemoteCh = nullptr;
        g_meteoLastDisconnectReason = reason;
        g_meteoConnectFails = (uint8_t)min(255, g_meteoConnectFails + 1);
        const uint32_t now = millis();
        g_meteoNextActionMs = now + meteoComputeRetryDelayMs(now, g_meteoConnectFails);
        const bool suspended = meteoSuspendIfNeeded(now, g_meteoConnectFails);
        if (!suspended && meteoAutoDiscoverOnFailAllowed() && g_meteoConnectFails >= METEO_FAILS_BEFORE_SCAN) {
            g_meteoDiscoverRequested = true;
            g_meteoNextDiscoverMs = 0;
        }
        updateBleLed();
    }

public:
    // NimBLE-Arduino API changed across versions:
    // - older: onDisconnect(NimBLEClient*)
    // - newer: onDisconnect(NimBLEClient*, int reason)
    // Implement both (without 'override') so it compiles and works with either.
    void onDisconnect(NimBLEClient* pClient) { handleDisconnect(pClient, -1); }
    void onDisconnect(NimBLEClient* pClient, int reason) { handleDisconnect(pClient, reason); }
    void onConnectFail(NimBLEClient* pClient, int reason) {
        (void)pClient;
        g_meteoLastDisconnectReason = reason;
        Serial.printf("[BLE] Meteo connect failed (reason=%d)\n", reason);
    }
};

static MeteoClientCallbacks g_meteoClientCbs;

static void logMeteoFailure(const char* step, uint8_t failCount, uint32_t nextActionMs, bool willDiscover) {
    Serial.printf("[BLE] Meteo %s (fails=%u, next=%lums, discover=%s)\n",
                  step,
                  failCount,
                  (unsigned long)nextActionMs,
                  willDiscover ? "yes" : "no");
}

static void logMeteoConnectDiagnostics(uint32_t now, int usedAddrType) {
    const bool deviceSeen = g_meteoLastSeenMs > 0;
    const uint32_t ageMs = deviceSeen ? (uint32_t)(now - g_meteoLastSeenMs) : 0;
    Serial.printf("[BLE] Meteo connect failed: deviceSeenRecently=%s ageMs=%lums lastRssi=%d lastAddrType=%s usedAddrType=%s lastReason=%d\n",
                  deviceSeen ? "yes" : "no",
                  (unsigned long)ageMs,
                  g_meteoLastSeenRssi,
                  addrTypeToStr(g_meteoLastSeenAddrType),
                  addrTypeToStr(usedAddrType),
                  g_meteoLastDisconnectReason);
}

static bool meteoConnectIfNeeded() {
    if (!g_cfg.meteoEnabled && !g_meteoPairingActive) return false;
    const char* targetMac = meteoTargetMacCStr();
    if (!targetMac || !targetMac[0]) return false;

    const uint32_t now = millis();
    if (meteoCooldownActive(now)) {
        if (!g_meteoLastSuspendLogMs || (now - g_meteoLastSuspendLogMs) > METEO_SUSPEND_LOG_INTERVAL_MS) {
            g_meteoLastSuspendLogMs = now;
            const uint32_t remainingMs = (uint32_t)max<int32_t>(0, (int32_t)(g_meteoSuspendUntilMs - now));
            Serial.printf("[BLE] Meteo suspended (remaining %lums)\n", (unsigned long)remainingMs);
        }
        return false;
    }
    if ((int32_t)(now - g_meteoNextActionMs) < 0) return false;
    // Limit connect attempts, but keep the very first fix after boot as fast as possible.
    uint32_t minConnectIntervalMs = METEO_MIN_CONNECT_INTERVAL_MS;
    if (meteoFastBootstrapActive(now)) {
        minConnectIntervalMs = 450;   // aggressive: converge quickly after reboot
    } else if (!g_meteoLastFixMs || (now - g_meteoLastFixMs) > 30000UL) {
        minConnectIntervalMs = 1200;  // stale -> retry quicker than normal
    }
    if (g_meteoLastConnectAttemptMs && (now - g_meteoLastConnectAttemptMs) < minConnectIntervalMs) return false;

    if (g_meteoClient && g_meteoClient->isConnected()) return true;

    // Optional "refresh" scan to quickly learn address type/RSSI before a connect attempt.
    // This improves connection reliability for devices using RANDOM addresses, without extra blocking.
    if (g_cfg.meteoFastScan
        && !g_meteoScanning
        && !g_meteoRefreshOnlyRequested
        && g_cfg.meteoMac.length()
        && !g_meteoRuntimeMac[0]
        && (g_meteoLastSeenAddrType < 0 || !g_meteoLastSeenMs || (now - g_meteoLastSeenMs) > 30000UL)) {
        g_meteoRefreshOnlyRequested = true;
        g_meteoNextDiscoverMs = 0;
        g_meteoNextActionMs = now + (meteoFastBootstrapActive(now) ? 120 : 500);
        return false;
    }

    // Avoid connect attempts while scanning.
    NimBLEScan* scan = NimBLEDevice::getScan();
    const bool scanRunning = (scan && scan->isScanning()) || g_meteoScanning;
    if (scanRunning) {
        g_meteoNextActionMs = now + (meteoFastBootstrapActive(now) ? 150 : 500);
        Serial.printf("[BLE] Meteo connect deferred: scan running (next in %lums)\n",
                      (unsigned long)(g_meteoNextActionMs - now));
        return false;
    }

    // Cleanup old client instance (fresh connect attempt).
    if (g_meteoClient) {
        NimBLEDevice::deleteClient(g_meteoClient);
        g_meteoClient = nullptr;
        g_meteoRemoteCh = nullptr;
    }

    g_meteoClient = NimBLEDevice::createClient();
    g_meteoClient->setClientCallbacks(&g_meteoClientCbs, false);

    // FIX(1): Make connect attempts short to keep the main loop responsive.
    // NimBLEClient::setConnectTimeout expects milliseconds.
    // Default can be ~30s; that would freeze HTTP/UI when the meteo sensor is offline.
    uint32_t connectTimeoutMs = g_cfg.meteoConnectTimeoutMs;
    if (connectTimeoutMs < 300UL) connectTimeoutMs = 300UL;
    if (connectTimeoutMs > 5000UL) connectTimeoutMs = 5000UL;
    g_meteoClient->setConnectTimeout(connectTimeoutMs);

    g_meteoConnecting = true;
    g_meteoLastConnectAttemptMs = now;

    const int preferredType = g_meteoRuntimeMac[0] ? g_meteoRuntimeAddrType : g_meteoLastSeenAddrType;

    // FIX(2): Do not try multiple addr types in a single loop iteration.
    // Rotate address-type attempts across retries to avoid worst-case blocking (PUBLIC+RANDOM back-to-back).
    int types[3];
    int nTypes = 0;
    auto addType = [&](int t) {
        for (int i = 0; i < nTypes; i++) if (types[i] == t) return;
        if (nTypes < 3) types[nTypes++] = t;
    };
    if (preferredType == BLE_ADDR_PUBLIC || preferredType == BLE_ADDR_RANDOM) addType(preferredType);
    addType(BLE_ADDR_PUBLIC);
    addType(BLE_ADDR_RANDOM);
    if (nTypes <= 0) {
        addType(BLE_ADDR_PUBLIC);
        nTypes = 1;
    }

    const int useType = types[g_meteoAddrTryIndex % (uint8_t)nTypes];

    const bool relaxSecurity = (g_cfg.securityMode != "off");
    if (relaxSecurity) applyBleSecurityRelaxedForMeteo();

    Serial.printf("[BLE] Meteo connect attempt: mac=%s preferred=%s try=%s rssi=%d scanRunning=%s timeout=%lums security=%s\n",
                  targetMac,
                  addrTypeToStr(preferredType),
                  addrTypeToStr(useType),
                  g_meteoLastSeenRssi,
                  scanRunning ? "yes" : "no",
                  (unsigned long)connectTimeoutMs,
                  relaxSecurity ? "relaxed" : "strict");

    auto tryConnect = [&](uint8_t addrType) -> bool {
        Serial.printf("[BLE] Meteo connect -> %s (addrType=%s)\n", targetMac, addrTypeToStr(addrType));
        NimBLEAddress addr(std::string(targetMac), addrType);
        return g_meteoClient->connect(addr);
    };

    bool connected = tryConnect((uint8_t)useType);
    const int connectedType = connected ? useType : -1;

    if (relaxSecurity) applyBleSecurityFromConfig(false);

    if (!connected) {
        // advance rotation for next attempt
        g_meteoAddrTryIndex = (uint8_t)((g_meteoAddrTryIndex + 1U) % (uint8_t)nTypes);

        g_meteoConnectFails = (uint8_t)min(255, g_meteoConnectFails + 1);
        const bool suspended = meteoSuspendIfNeeded(now, g_meteoConnectFails);
        const bool willDiscover = !suspended && meteoAutoDiscoverOnFailAllowed() && g_meteoConnectFails >= METEO_FAILS_BEFORE_SCAN;
        if (!suspended) {
            g_meteoNextActionMs = now + meteoComputeRetryDelayMs(now, g_meteoConnectFails);
            if (willDiscover) {
                g_meteoDiscoverRequested = true;
                g_meteoNextDiscoverMs = 0;
            }
        }
        logMeteoFailure("connect failed", g_meteoConnectFails, g_meteoNextActionMs, willDiscover);
        logMeteoConnectDiagnostics(now, useType);
        g_meteoConnecting = false;
        g_meteoAutoSavePending = false;
        g_meteoAutoSaveMac[0] = 0;
        g_meteoAutoSaveName = "";
        return false;
    }

    // success -> reset rotation
    g_meteoAddrTryIndex = 0;

    g_meteoConnectFails = 0;
    g_meteoSuspendUntilMs = 0;
    if (connectedType >= 0) {
        g_meteoLastSeenAddrType = connectedType;
        if (g_meteoRuntimeMac[0]) g_meteoRuntimeAddrType = connectedType;
    }

    NimBLERemoteService* svc = g_meteoClient->getService(UUID_SVC_METEO);
    if (!svc) {
        g_meteoClient->disconnect();
        g_meteoFailCount = (uint8_t)min(255, g_meteoFailCount + 1);
        const bool suspended = meteoSuspendIfNeeded(now, g_meteoFailCount);
        const bool willDiscover = !suspended && meteoAutoDiscoverOnFailAllowed() && g_meteoFailCount >= METEO_FAILS_BEFORE_SCAN;
        if (!suspended) {
            g_meteoNextActionMs = now + meteoComputeRetryDelayMs(now, g_meteoFailCount);
        }
        if (!suspended && willDiscover) {
            g_meteoDiscoverRequested = true;
            g_meteoNextDiscoverMs = 0;
        }
        logMeteoFailure("service not found", g_meteoFailCount, g_meteoNextActionMs, willDiscover);
        g_meteoConnecting = false;
        g_meteoAutoSavePending = false;
        g_meteoAutoSaveMac[0] = 0;
        g_meteoAutoSaveName = "";
        return false;
    }

    g_meteoRemoteCh = svc->getCharacteristic(UUID_CH_METEO);
    if (!g_meteoRemoteCh) {
        g_meteoClient->disconnect();
        g_meteoFailCount = (uint8_t)min(255, g_meteoFailCount + 1);
        const bool suspended = meteoSuspendIfNeeded(now, g_meteoFailCount);
        const bool willDiscover = !suspended && meteoAutoDiscoverOnFailAllowed() && g_meteoFailCount >= METEO_FAILS_BEFORE_SCAN;
        if (!suspended) {
            g_meteoNextActionMs = now + meteoComputeRetryDelayMs(now, g_meteoFailCount);
        }
        if (!suspended && willDiscover) {
            g_meteoDiscoverRequested = true;
            g_meteoNextDiscoverMs = 0;
        }
        logMeteoFailure("char not found", g_meteoFailCount, g_meteoNextActionMs, willDiscover);
        g_meteoConnecting = false;
        g_meteoAutoSavePending = false;
        g_meteoAutoSaveMac[0] = 0;
        g_meteoAutoSaveName = "";
        return false;
    }

    if (g_meteoRemoteCh->canNotify()) {
        if (!g_meteoRemoteCh->subscribe(true, meteoOnNotify)) {
            g_meteoClient->disconnect();
            g_meteoFailCount = (uint8_t)min(255, g_meteoFailCount + 1);
            const bool suspended = meteoSuspendIfNeeded(now, g_meteoFailCount);
            const bool willDiscover = !suspended && meteoAutoDiscoverOnFailAllowed() && g_meteoFailCount >= METEO_FAILS_BEFORE_SCAN;
            if (!suspended) {
                g_meteoNextActionMs = now + meteoComputeRetryDelayMs(now, g_meteoFailCount);
            }
            if (!suspended && willDiscover) {
                g_meteoDiscoverRequested = true;
                g_meteoNextDiscoverMs = 0;
            }
            logMeteoFailure("subscribe failed", g_meteoFailCount, g_meteoNextActionMs, willDiscover);
            g_meteoConnecting = false;
            g_meteoAutoSavePending = false;
            g_meteoAutoSaveMac[0] = 0;
            g_meteoAutoSaveName = "";
            return false;
        }
    }

    // Bootstrap: do a single initial read so we can get a value immediately after connect.
    // The C3 meteo sensor keeps the characteristic value updated even when no notifications were sent yet.
    if (g_meteoRemoteCh->canRead()) {
        std::string v = g_meteoRemoteCh->readValue();
        if (v.length() >= 6) {
            uint8_t tmp[6];
            memcpy(tmp, v.data(), 6);
            meteoOnNotify(g_meteoRemoteCh, tmp, 6, false);
            Serial.println(F("[BLE] Meteo initial read OK"));
        } else {
            Serial.printf("[BLE] Meteo initial read: short (%uB)\n", (unsigned)v.length());
        }
    }

    if (!g_meteoLastFixMs) {
        g_meteoLastFixMs = millis();
    }

    char connectedMacNorm[18];
    normalizeMacToBuf(targetMac, connectedMacNorm);
    strncpy(g_meteoLastSeenMac, connectedMacNorm, sizeof(g_meteoLastSeenMac));
    g_meteoLastSeenMac[sizeof(g_meteoLastSeenMac) - 1] = 0;
    meteoCommitAutoSaveIfPending(connectedMacNorm);

    g_meteoNextActionMs = now + 15000; // next action no sooner than 15s
    g_meteoLastDisconnectReason = -1;
    g_meteoConnecting = false;
    return true;
}

// ---------- Public API ----------
void bleInit() {
    loadConfigFS();
    loadAllowlistFS();
    updateBleLed();

    if (!g_cfg.enabled) {
        Serial.println(F("[BLE] disabled (config)"));
        return;
    }

    if (g_bleInitialized) return;

    // Pre-allocate frequently used Strings to reduce heap fragmentation
    g_lastAck.reserve(256);
    g_pairingRoleHint.reserve(24);
    g_meteoLastSeenName.reserve(48);
    g_meteoAutoSaveName.reserve(48);
    g_scanBestName.reserve(48);

    NimBLEDevice::init(g_cfg.deviceName.c_str());
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // max

    // Dual role
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);

    // Security setup
    applyBleSecurityFromConfig(true);

    // Server
    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(&g_srvCb);

    g_ctrlSvc = g_server->createService(UUID_SVC_CTRL);

    g_statusCh = g_ctrlSvc->createCharacteristic(UUID_CH_STATUS,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    g_cmdCh = g_ctrlSvc->createCharacteristic(UUID_CH_CMD,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);

    g_ackCh = g_ctrlSvc->createCharacteristic(UUID_CH_ACK,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    g_cmdCh->setCallbacks(&g_cmdCb);
    g_ackCh->setValue(g_lastAck.c_str());

    g_ctrlSvc->start();

    // Advertising
    if (g_cfg.advertise) {
        NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
        adv->addServiceUUID(UUID_SVC_CTRL);
        adv->start();
        Serial.println(F("[BLE] Advertising started"));
    }

    // Meteo client: initial scheduling
    g_bleBootMs = millis();
    g_meteoEverFix = false;
    g_meteoFastUntilMs = g_bleBootMs + 20000UL; // 20s to acquire first fix quickly
    g_meteoNextActionMs = g_bleBootMs + 50;
    g_meteoNextBcastScanMs = g_bleBootMs + 200;
    g_bleInitialized = true;
}

void bleLoop() {
    if (!g_cfg.enabled) return;

    const uint32_t now = millis();

    // pairing window timeout
    if (g_pairingWindow && (int32_t)(now - g_pairingUntilMs) >= 0) {
        g_pairingWindow = false;
        g_pairingRoleHint = "";
        Serial.println(F("[BLE] Pairing window closed"));
    }

    // meteo pairing timeout (can outlive g_pairingWindow if closed early)
    if (g_meteoPairingActive && (int32_t)(now - g_meteoPairingUntilMs) >= 0) {
        g_meteoPairingActive = false;
        g_meteoPairingUntilMs = 0;
        Serial.println(F("[BLE] Meteo pairing window closed"));
    }

    // periodic status notify (1 Hz)
    if (g_statusCh && (int32_t)(now - g_lastStatusNotifyMs) >= 1000) {
        g_lastStatusNotifyMs = now;
        String st = bleGetStatusJson();
        g_statusCh->setValue(st.c_str());
        g_statusCh->notify(true);
    }

    // meteo auto-discovery + client handling
// Periodic short refresh scans to catch advertising broadcast frames (pure passive mode).
// We run these even when a target MAC is known and we are not connected.
if (g_cfg.meteoEnabled && meteoTargetMacCStr()[0] && (!g_meteoClient || !g_meteoClient->isConnected())) {
    if (!g_meteoNextBcastScanMs) g_meteoNextBcastScanMs = now + 200;
    if ((int32_t)(now - g_meteoNextBcastScanMs) >= 0) {
        g_meteoNextBcastScanMs = now + METEO_BCAST_SCAN_PERIOD_MS;
        g_meteoRefreshOnlyRequested = true;
        g_meteoNextDiscoverMs = 0;
    }
}

    meteoStartScanIfNeeded();
    meteoConnectIfNeeded();

    if (g_meteoClient && g_meteoClient->isConnected()) {
        // if stale -> mark no fix (does not disconnect)
        if (g_meteoFix && (now - g_meteoLastMs) > METEO_STALE_MS) {
            g_meteoFix = false;
        }
    }

    if (g_cfg.meteoEnabled && meteoAutoDiscoverOnFailAllowed()) {
        if (g_meteoLastFixMs && (now - g_meteoLastFixMs) > METEO_STALE_MS) {
            g_meteoDiscoverRequested = true;
            g_meteoNextDiscoverMs = 0;
        }
    }

    updateBleLed();
}

String bleGetStatusJson() {
    DynamicJsonDocument doc(3072);
    doc["enabled"] = g_cfg.enabled;
    doc["securityMode"] = g_cfg.securityMode;
    doc["allowlistEnforced"] = g_cfg.allowlistEnforced;
    doc["deviceNamePendingRestart"] = g_bleNamePendingRestart;

    doc["pairingWindow"] = g_pairingWindow;
    doc["pairingRoleHint"] = g_pairingRoleHint;
    int32_t remainingMs = (int32_t)(g_pairingUntilMs - millis());
    doc["pairingRemainingSec"] = (g_pairingWindow && remainingMs > 0) ? (uint32_t)(remainingMs / 1000) : 0;

    JsonObject serverObj = doc.createNestedObject("server");
    serverObj["connectedCount"] = (uint32_t)(g_server ? g_server->getConnectedCount() : g_serverConnCount);
    serverObj["lastConnectAt"] = g_serverLastConnectMs;
    serverObj["lastDisconnectAt"] = g_serverLastDisconnectMs;

    JsonObject allowObj = doc.createNestedObject("allowlist");
    allowObj["count"] = (uint32_t)g_allow.size();

    JsonObject meteoObj = doc.createNestedObject("meteo");
    doc["meteoEnabled"] = g_cfg.meteoEnabled;
    doc["meteoAutoDiscover"] = g_cfg.meteoAutoDiscover;
    doc["meteoAutoSave"] = g_cfg.meteoAutoSave;
    doc["meteoAutoPair"] = g_cfg.meteoAutoPair;
    doc["meteoPairingActive"] = g_meteoPairingActive;
    doc["meteoFastScan"] = g_cfg.meteoFastScan;
    doc["meteoConnectTimeoutMs"] = g_cfg.meteoConnectTimeoutMs;
    doc["meteoScanning"] = g_meteoScanning;
    meteoObj["discoverRequested"] = g_meteoDiscoverRequested;
    meteoObj["mac"] = g_cfg.meteoMac;
    const char* activeMac = meteoTargetMacCStr();
    meteoObj["activeMac"] = activeMac;
    meteoObj["activeSource"] = (activeMac && activeMac[0]) ? (g_meteoRuntimeMac[0] ? "runtime" : "config") : "none";
    meteoObj["lastSeenMac"] = g_meteoLastSeenMac;
    meteoObj["lastSeenName"] = g_meteoLastSeenName;
    meteoObj["lastSeenRssi"] = g_meteoLastSeenRssi;
    meteoObj["lastRssi"] = g_meteoLastSeenRssi;
meteoObj["lastSeenMs"] = g_meteoLastSeenMs;
meteoObj["broadcastPassive"] = g_meteoPassiveBroadcastOnly;
meteoObj["lastBcastMs"] = g_meteoLastBcastMs;
meteoObj["bcastFramesInWindow"] = g_meteoBcastFramesInWindow;
    meteoObj["connected"] = (g_meteoClient && g_meteoClient->isConnected());
    meteoObj["fix"] = g_meteoFix;
    meteoObj["failCount"] = g_meteoFailCount;
    meteoObj["connectFails"] = g_meteoConnectFails;
    meteoObj["lastAttemptMs"] = g_meteoLastConnectAttemptMs;
    meteoObj["lastDisconnectReason"] = g_meteoLastDisconnectReason;
    meteoObj["suspendUntilMs"] = g_meteoSuspendUntilMs;

    if (!g_cfg.meteoEnabled) {
        meteoObj["state"] = "disabled";
    } else if (meteoCooldownActive(millis())) {
        meteoObj["state"] = "suspended";
    } else if (g_meteoClient && g_meteoClient->isConnected()) {
        meteoObj["state"] = "connected";
    } else if (g_meteoConnecting) {
        meteoObj["state"] = "connecting";
    } else if (g_meteoScanning) {
        meteoObj["state"] = "scanning";
    } else if ((g_meteoFailCount + g_meteoConnectFails) >= METEO_FAILS_BEFORE_SCAN) {
        meteoObj["state"] = "error";
    } else {
        meteoObj["state"] = "idle";
    }

    if (g_meteoFix) {
        JsonObject m = meteoObj.createNestedObject("reading");
        m["tempC"] = (float)g_meteoTempX10 / 10.0f;
        m["hum"] = g_meteoHum;
        m["press"] = g_meteoPress;
        m["trend"] = g_meteoTrend;
        m["ageMs"] = (uint32_t)(millis() - g_meteoLastMs);
    }

    doc["ts"] = nowIso();

    String out;
    serializeJson(doc, out);
    return out;
}

String bleGetConfigJson() {
    DynamicJsonDocument doc(2048);
    doc["enabled"] = g_cfg.enabled;
    doc["deviceName"] = g_cfg.deviceName;
    doc["advertise"] = g_cfg.advertise;

    doc["securityMode"] = g_cfg.securityMode;
    doc["passkey"] = g_cfg.passkey;
    doc["allowlistEnforced"] = g_cfg.allowlistEnforced;

    doc["meteoEnabled"] = g_cfg.meteoEnabled;
    doc["meteoMac"] = g_cfg.meteoMac;
    doc["meteoAutoDiscover"] = g_cfg.meteoAutoDiscover;
    doc["meteoAutoSave"] = g_cfg.meteoAutoSave;
    doc["meteoAutoPair"] = g_cfg.meteoAutoPair;
    doc["meteoFastScan"] = g_cfg.meteoFastScan;
    doc["meteoStopScanOnMatch"] = g_cfg.meteoStopScanOnMatch;
    doc["meteoPairMinRssi"] = g_cfg.meteoPairMinRssi;
    doc["meteoScanInterval"] = g_cfg.meteoScanInterval;
    doc["meteoScanWindow"] = g_cfg.meteoScanWindow;
    doc["meteoFastScanInterval"] = g_cfg.meteoFastScanInterval;
    doc["meteoFastScanWindow"] = g_cfg.meteoFastScanWindow;
    doc["meteoConnectTimeoutMs"] = g_cfg.meteoConnectTimeoutMs;
    doc["meteoDiscoverIntervalMs"] = g_cfg.meteoDiscoverIntervalMs;
    doc["meteoScanMs"] = g_cfg.meteoScanMs;
    doc["meteoReconnectMs"] = g_cfg.meteoReconnectMs;
    doc["meteoMaxConnectFails"] = g_cfg.meteoMaxConnectFails;
    doc["meteoCooldownMs"] = g_cfg.meteoCooldownMs;
    doc["schemaVersion"] = g_cfg.schemaVersion;

    String out;
    serializeJson(doc, out);
    return out;
}

bool bleSetConfigJson(const String& json, String* errorCode) {
    if (errorCode) *errorCode = "";
    Serial.printf("[BLE] Config JSON len=%u\n", (unsigned)json.length());
    size_t capacity = json.length();
    capacity = capacity + (capacity / 5) + 512;
    if (capacity < 2048) capacity = 2048;
    if (capacity > 16384) capacity = 16384;
    DynamicJsonDocument doc(capacity);
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[BLE] Config JSON parse failed: %s (len=%u)\n", err.c_str(), (unsigned)json.length());
        const String preview = json.substring(0, 200);
        if (preview.length()) {
            Serial.printf("[BLE] Config JSON preview: %s\n", preview.c_str());
        }
        if (errorCode) *errorCode = "bad_json";
        return false;
    }

    const bool prevEnabled = g_cfg.enabled;
    const bool prevAdvertise = g_cfg.advertise;
    const String prevName = g_cfg.deviceName;
    const bool prevMeteoEnabled = g_cfg.meteoEnabled;

    g_cfg.enabled = doc["enabled"] | g_cfg.enabled;
    g_cfg.deviceName = String((const char*)(doc["deviceName"] | g_cfg.deviceName.c_str()));
    g_cfg.advertise = doc["advertise"] | g_cfg.advertise;

    g_cfg.securityMode = String((const char*)(doc["securityMode"] | g_cfg.securityMode.c_str()));
    g_cfg.passkey = (uint32_t)(doc["passkey"] | g_cfg.passkey);
    g_cfg.allowlistEnforced = doc["allowlistEnforced"] | g_cfg.allowlistEnforced;

    g_cfg.meteoEnabled = doc["meteoEnabled"] | g_cfg.meteoEnabled;
    g_cfg.meteoMac = normalizeMac(String((const char*)(doc["meteoMac"] | g_cfg.meteoMac.c_str())));
    g_cfg.meteoAutoDiscover = doc["meteoAutoDiscover"] | g_cfg.meteoAutoDiscover;
    g_cfg.meteoAutoSave = doc["meteoAutoSave"] | g_cfg.meteoAutoSave;
    g_cfg.meteoAutoPair = doc["meteoAutoPair"] | g_cfg.meteoAutoPair;
    g_cfg.meteoFastScan = doc["meteoFastScan"] | g_cfg.meteoFastScan;
    g_cfg.meteoStopScanOnMatch = doc["meteoStopScanOnMatch"] | g_cfg.meteoStopScanOnMatch;
    g_cfg.meteoPairMinRssi = (int)(doc["meteoPairMinRssi"] | g_cfg.meteoPairMinRssi);
    g_cfg.meteoScanInterval = (uint16_t)(doc["meteoScanInterval"] | g_cfg.meteoScanInterval);
    g_cfg.meteoScanWindow = (uint16_t)(doc["meteoScanWindow"] | g_cfg.meteoScanWindow);
    g_cfg.meteoFastScanInterval = (uint16_t)(doc["meteoFastScanInterval"] | g_cfg.meteoFastScanInterval);
    g_cfg.meteoFastScanWindow = (uint16_t)(doc["meteoFastScanWindow"] | g_cfg.meteoFastScanWindow);
    g_cfg.meteoConnectTimeoutMs = (uint32_t)(doc["meteoConnectTimeoutMs"] | g_cfg.meteoConnectTimeoutMs);
    g_cfg.meteoDiscoverIntervalMs = (uint32_t)(doc["meteoDiscoverIntervalMs"] | g_cfg.meteoDiscoverIntervalMs);
    g_cfg.meteoScanMs = (uint32_t)(doc["meteoScanMs"] | g_cfg.meteoScanMs);
    g_cfg.meteoReconnectMs = (uint32_t)(doc["meteoReconnectMs"] | g_cfg.meteoReconnectMs);
    g_cfg.meteoMaxConnectFails = (int)(doc["meteoMaxConnectFails"] | g_cfg.meteoMaxConnectFails);
    g_cfg.meteoCooldownMs = (uint32_t)(doc["meteoCooldownMs"] | g_cfg.meteoCooldownMs);
    g_cfg.schemaVersion = (uint32_t)(doc["schemaVersion"] | g_cfg.schemaVersion);

    if (prevName != g_cfg.deviceName && g_bleInitialized) {
        g_bleNamePendingRestart = true;
    }

    if (doc.containsKey("meteoMac")) {
        g_meteoRuntimeMac[0] = 0;
        g_meteoRuntimeAddrType = -1;
        g_meteoDiscoverRequested = false;
        g_meteoFailCount = 0;
        g_meteoConnectFails = 0;
        g_meteoAutoSavePending = false;
        g_meteoLastSeenMac[0] = 0;
        g_meteoLastSeenName = "";
        g_meteoLastSeenRssi = -999;
        g_meteoLastSeenAddrType = -1;
        g_meteoLastSeenMs = 0;
        meteoResetRetryState("meteoMac changed");
    }
    if (!g_cfg.meteoEnabled) {
        g_meteoRuntimeMac[0] = 0;
        g_meteoRuntimeAddrType = -1;
        g_meteoDiscoverRequested = false;
        g_meteoFailCount = 0;
        g_meteoConnectFails = 0;
        g_meteoAutoSavePending = false;
        meteoResetRetryState("meteo disabled");
    }
    if (doc.containsKey("meteoEnabled") && g_cfg.meteoEnabled && !prevMeteoEnabled) {
        meteoResetRetryState("meteoEnabled");
    }

    if ((doc["meteoDiscoverNow"] | false) && g_cfg.meteoEnabled) {
        g_meteoDiscoverRequested = true;
        g_meteoNextDiscoverMs = 0;
        Serial.println(F("[BLE] Meteo discovery requested by API"));
    }

    if (!saveConfigFS()) {
        fsLock();
        const bool hasCfg = LittleFS.exists(BLE_CFG_PATH);
        fsUnlock();
        Serial.printf("[BLE] saveConfigFS failed (fsReady=%s, exists=%s)\n",
                      fsIsReady() ? "yes" : "no",
                      hasCfg ? "yes" : "no");
        if (errorCode) *errorCode = "fs_write_failed";
        return false;
    }

    if (g_cfg.enabled && !g_bleInitialized) {
        bleInit();
    }

    if (!g_cfg.enabled && g_bleInitialized) {
        if (g_meteoClient && g_meteoClient->isConnected()) {
            g_meteoClient->disconnect();
        }
        if (g_meteoScanning) {
            NimBLEScan* scan = NimBLEDevice::getScan();
            if (scan) scan->stop();
            g_meteoScanning = false;
        }
        NimBLEDevice::stopAdvertising();
        g_pairingWindow = false;
        g_meteoScanning = false;
        g_meteoConnecting = false;
        updateBleLed();
        return true;
    }

    if (g_bleInitialized && prevAdvertise != g_cfg.advertise) {
        if (g_cfg.advertise && g_cfg.enabled) {
            NimBLEDevice::startAdvertising();
        } else {
            NimBLEDevice::stopAdvertising();
        }
    }

    if (!prevEnabled && g_cfg.enabled && g_bleInitialized) {
        if (g_cfg.advertise) {
            NimBLEDevice::startAdvertising();
        }
        g_meteoNextActionMs = millis() + 500;
    }

    return true;
}

bool bleMeteoRetryNow() {
    meteoResetRetryState("manual retry");
    g_meteoNextActionMs = millis();
    return g_cfg.meteoEnabled;
}

String bleGetPairedJson() {
    DynamicJsonDocument doc(8192);
    doc["schemaVersion"] = 1;
    JsonArray arr = doc.createNestedArray("devices");
    for (auto& d : g_allow) {
        JsonObject o = arr.createNestedObject();
        o["mac"] = d.mac;
        o["name"] = d.name;
        o["role"] = d.role;
        o["addedAt"] = d.addedAt;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

bool bleStartPairing(uint32_t seconds, const String& roleHint) {
    if (!g_cfg.enabled) return false;
    g_pairingWindow = true;
    updateBleLed();
    g_pairingUntilMs = millis() + seconds * 1000UL;
    g_pairingRoleHint = roleHint;
    Serial.printf("[BLE] Pairing window opened for %lus (role=%s)\n", (unsigned long)seconds, roleHint.c_str());

    // Special case: meteo pairing = actively scan for a meteo sensor and auto-save it.
    if (roleHint == "meteo") {
        g_meteoPairingActive = true;
        g_meteoPairingUntilMs = g_pairingUntilMs;
        g_meteoDiscoverRequested = true;
        g_meteoNextDiscoverMs = 0;
        // If a scan is already running, we will use its results.
    }
    return true;
}

bool bleStopPairing() {
    g_pairingWindow = false;
    updateBleLed();
    g_pairingRoleHint = "";
    g_pairingUntilMs = 0;

    g_meteoPairingActive = false;
    g_meteoPairingUntilMs = 0;
    return true;
}

bool bleRemoveDevice(const String& mac) {
    const String normalized = normalizeMac(mac);
    bool removed = false;
    for (size_t i=0;i<g_allow.size();){
        if (macEquals(g_allow[i].mac, normalized)) {
            g_allow.erase(g_allow.begin()+i);
            removed = true;
        } else i++;
    }
    if (removed) saveAllowlistFS();

    // best-effort remove bond
    NimBLEAddress addrPublic(std::string(normalized.c_str()), BLE_ADDR_PUBLIC);
    NimBLEAddress addrRandom(std::string(normalized.c_str()), BLE_ADDR_RANDOM);
    NimBLEDevice::deleteBond(addrPublic);
    NimBLEDevice::deleteBond(addrRandom);

    if (g_meteoClient && g_meteoClient->isConnected()) {
        char act[18];
        char norm[18];
        normalizeMacToBuf(meteoTargetMacCStr(), act);
        normalizeMacToBuf(normalized.c_str(), norm);
        if (act[0] && norm[0] && strcmp(act, norm) == 0) {
            g_meteoClient->disconnect();
        }
    }

    return removed;
}

bool bleClearDevices() {
    g_allow.clear();
    saveAllowlistFS();
    NimBLEDevice::deleteAllBonds();
    return true;
}

bool bleHasMeteoFix() {
    return g_meteoFix;
}

bool bleGetMeteoTempC(float &outC) {
    outC = NAN;
    if (!g_meteoFix) return false;
    outC = (float)g_meteoTempX10 / 10.0f;
    return isfinite(outC);
}

bool bleGetTempCById(const String& id, float &outC) {
    // Default / legacy IDs
    if (!id.length()) return bleGetMeteoTempC(outC);

    String raw = id;
    raw.trim();
    if (!raw.length()) return bleGetMeteoTempC(outC);

    // Accept MAC as ID (maps to meteo if it matches the active target MAC)
    {
        char idNorm[18];
        char targetNorm[18];
        normalizeMacToBuf(raw.c_str(), idNorm);
        normalizeMacToBuf(meteoTargetMacCStr(), targetNorm);
        if (idNorm[0] && targetNorm[0] && strcmp(idNorm, targetNorm) == 0) {
            return bleGetMeteoTempC(outC);
        }
    }

    String s = raw;
    s.toLowerCase();

    if (s == "meteo" || s == "meteo.tempc" || s == "temp" || s == "tempc") {
        return bleGetMeteoTempC(outC);
    }

    // Unknown ID (reserved)
    outC = NAN;
    return false;
}
