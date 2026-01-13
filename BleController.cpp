#include "BleController.h"

#include <FS.h>
#include <LittleFS.h>
#include "FsController.h"
#include <ArduinoJson.h>

#include <string>

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
    bool meteoEnabled = false;
    String meteoMac = "";            // "AA:BB:CC:DD:EE:FF" (pokud je prázdné a auto-discovery je ON, pokusí se zařízení najít)
    bool meteoAutoDiscover = true;   // scan a vybere nejlepší RSSI meteostanici, pokud meteoMac není nastavené
    bool meteoAutoSave = true;       // po nalezení uloží meteoMac do /ble.json a přidá do allowlistu
    uint32_t meteoDiscoverIntervalMs = 10000; // jak často opakovat scan, když se nedaří nic najít
    uint32_t meteoScanMs = 4000;     // délka scan okna
    uint32_t meteoReconnectMs = 8000;
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
static String g_meteoLastSeenMac = "";
static String g_meteoLastSeenName = "";
static int g_meteoLastSeenRssi = -999;
static String g_meteoRuntimeMac = "";
static uint32_t g_meteoLastFixMs = 0;
static uint8_t g_meteoFailCount = 0;
static uint8_t g_meteoConnectFails = 0;
static bool g_meteoForceDiscover = false;
static bool g_meteoConnecting = false;
static uint32_t g_meteoLastConnectAttemptMs = 0;
static int g_meteoLastDisconnectReason = -1;
static bool g_meteoAutoSavePending = false;
static String g_meteoAutoSaveMac = "";
static String g_meteoAutoSaveName = "";

// scan best candidate
static String g_scanBestMac = "";
static String g_scanBestName = "";
static int g_scanBestRssi = -999;

static const uint8_t METEO_FAILS_BEFORE_SCAN = 3;
static const uint32_t METEO_STALE_MS = 180000;
static const uint32_t METEO_MIN_CONNECT_INTERVAL_MS = 2500;

// ---------- Helpers ----------
static String nowIso() {
    // bez RTC: jen uptime v sekundách, ale UI stačí
    uint32_t s = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "uptime+%lus", (unsigned long)s);
    return String(buf);
}

static bool fsReadAll(const char* path, String& out) {
    if (!fsIsReady()) return false;
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    out = f.readString();
    f.close();
    out.trim();
    return out.length() > 0;
}

static bool macEquals(const String& a, const String& b) {
    String A=a, B=b;
    A.toUpperCase(); B.toUpperCase();
    return A == B;
}

static String normalizeMac(String mac) {
    mac.trim();
    mac.toUpperCase();
    return mac;
}

static const String& meteoTargetMac() {
    return g_meteoRuntimeMac.length() ? g_meteoRuntimeMac : g_cfg.meteoMac;
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
    DynamicJsonDocument doc(4096);
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

    g_cfg.meteoEnabled = doc["meteoEnabled"] | false;
    g_cfg.meteoMac = normalizeMac(String((const char*)(doc["meteoMac"] | "")));
    g_cfg.meteoAutoDiscover = doc["meteoAutoDiscover"] | true;
    g_cfg.meteoAutoSave = doc["meteoAutoSave"] | true;
    g_cfg.meteoDiscoverIntervalMs = (uint32_t)(doc["meteoDiscoverIntervalMs"] | 10000);
    g_cfg.meteoScanMs = (uint32_t)(doc["meteoScanMs"] | 4000);
    g_cfg.meteoReconnectMs = (uint32_t)(doc["meteoReconnectMs"] | 8000);
    g_cfg.schemaVersion = (uint32_t)(doc["schemaVersion"] | 1);
    if (restored) {
        fsWriteAtomicKeepBak(BLE_CFG_PATH, s, BLE_CFG_BAK_PATH, true);
    }
    return true;
}

static bool saveConfigFS() {
    DynamicJsonDocument doc(4096);
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
    doc["meteoDiscoverIntervalMs"] = g_cfg.meteoDiscoverIntervalMs;
    doc["meteoScanMs"] = g_cfg.meteoScanMs;
    doc["meteoReconnectMs"] = g_cfg.meteoReconnectMs;
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


// ---------- Meteo auto-discovery (scan) ----------
static void meteoScanFinalize() {
    g_meteoScanning = false;

    if (g_scanBestMac.length()) {
        g_scanBestMac = normalizeMac(g_scanBestMac);
        g_meteoLastSeenMac = g_scanBestMac;
        g_meteoLastSeenName = g_scanBestName;
        g_meteoLastSeenRssi = g_scanBestRssi;

        Serial.printf("[BLE] Meteo auto-discovery: found %s (RSSI %d, name=%s)\n",
                      g_scanBestMac.c_str(), g_scanBestRssi, g_scanBestName.c_str());

        g_meteoRuntimeMac = g_scanBestMac;
        if (g_cfg.meteoAutoSave) {
            g_meteoAutoSavePending = true;
            g_meteoAutoSaveMac = g_scanBestMac;
            g_meteoAutoSaveName = g_scanBestName;
        } else {
            g_meteoAutoSavePending = false;
            g_meteoAutoSaveMac = "";
            g_meteoAutoSaveName = "";
        }
        g_meteoFailCount = 0;
        g_meteoConnectFails = 0;
        g_meteoForceDiscover = false;

        // try connect ASAP
        g_meteoNextActionMs = millis() + 200;
        g_meteoNextDiscoverMs = 0;
    } else {
        Serial.println(F("[BLE] Meteo auto-discovery: nothing found"));
        const uint32_t now = millis();
        g_meteoNextDiscoverMs = now + (g_cfg.meteoDiscoverIntervalMs ? g_cfg.meteoDiscoverIntervalMs : 10000);
    }

    // reset best candidate
    g_scanBestMac = "";
    g_scanBestName = "";
    g_scanBestRssi = -999;
}

// NimBLE-Arduino 2.x: NimBLEAdvertisedDeviceCallbacks replaced by NimBLEScanCallbacks,
// scan end callback moved to onScanEnd().
class MeteoScanCallbacks : public NimBLEScanCallbacks {
public:
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev) return;

        if (!dev->isAdvertisingService(UUID_SVC_METEO)) return;

        const int rssi = dev->getRSSI();
        if (rssi > g_scanBestRssi) {
            g_scanBestRssi = rssi;
            g_scanBestMac = macToString(dev->getAddress());
            g_scanBestName = String(dev->getName().c_str());
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
    if (!g_cfg.meteoEnabled) return false;
    if (!g_cfg.meteoAutoDiscover) return false;
    if (!g_meteoForceDiscover && meteoTargetMac().length()) return false; // prefer stored/runtime MAC
    if (g_meteoScanning) return false;

    const uint32_t now = millis();
    if (g_meteoNextDiscoverMs && (int32_t)(now - g_meteoNextDiscoverMs) < 0) return false;

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan) return false;

    g_scanBestMac = "";
    g_scanBestName = "";
    g_scanBestRssi = -999;

    // callbacks-only scan (do not store results to save RAM)
    scan->setMaxResults(0);
    scan->setScanCallbacks(&g_meteoScanCbs, false);
    scan->setActiveScan(true);
    scan->setInterval(97);
    scan->setWindow(37);

    uint32_t durMs = g_cfg.meteoScanMs;
    if (durMs < 2000) durMs = 2000;
    if (durMs > 30000) durMs = 30000;

    Serial.printf("[BLE] Meteo auto-discovery: scanning (%lums)\n", (unsigned long)durMs);
    g_meteoScanning = scan->start(durMs, false, true);
    if (!g_meteoScanning) {
        // failed to start scan -> retry later
        g_meteoNextDiscoverMs = now + (g_cfg.meteoDiscoverIntervalMs ? g_cfg.meteoDiscoverIntervalMs : 10000);
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
    g_meteoFailCount = 0;
    g_meteoConnectFails = 0;
    g_meteoForceDiscover = false;

    // externí BLE teploměr (konfigurace "Teploměry")
    thermometersBleOnReading("meteo.tempC", (float)g_meteoTempX10 / 10.0f);
}

static void meteoCommitAutoSaveIfPending(const String& connectedMac) {
    if (!g_cfg.meteoAutoSave) return;
    if (!g_meteoAutoSavePending) return;
    if (!macEquals(connectedMac, g_meteoAutoSaveMac)) return;

    g_cfg.meteoMac = g_meteoAutoSaveMac;
    g_meteoRuntimeMac = "";
    if (!isInAllowlist(g_meteoAutoSaveMac)) {
        PairedDevice d;
        d.mac = g_meteoAutoSaveMac;
        d.name = g_meteoAutoSaveName.length() ? g_meteoAutoSaveName : "meteo";
        d.role = "meteo";
        d.addedAt = (uint32_t)(millis() / 1000);
        g_allow.push_back(d);
        saveAllowlistFS();
    }
    saveConfigFS();

    g_meteoAutoSavePending = false;
    g_meteoAutoSaveMac = "";
    g_meteoAutoSaveName = "";
}

class MeteoClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pClient) override {
        (void)pClient;
        g_meteoFix = false;
        g_meteoRemoteCh = nullptr;
        g_meteoLastDisconnectReason = -1;
        g_meteoConnectFails = (uint8_t)min(255, g_meteoConnectFails + 1);
        const uint32_t now = millis();
        g_meteoNextActionMs = now + 250;
        if (g_cfg.meteoAutoDiscover && g_meteoConnectFails >= METEO_FAILS_BEFORE_SCAN) {
            g_meteoForceDiscover = true;
            g_meteoNextDiscoverMs = 0;
        }
        updateBleLed();
    }
};

static MeteoClientCallbacks g_meteoClientCbs;

static bool meteoConnectIfNeeded() {
    if (!g_cfg.meteoEnabled) return false;
    const String& targetMac = meteoTargetMac();
    if (!targetMac.length()) return false;

    const uint32_t now = millis();
    if ((int32_t)(now - g_meteoNextActionMs) < 0) return false;
    if (g_meteoLastConnectAttemptMs && (now - g_meteoLastConnectAttemptMs) < METEO_MIN_CONNECT_INTERVAL_MS) return false;

    if (g_meteoClient && g_meteoClient->isConnected()) return true;

    // cleanup old
    if (g_meteoClient) {
        NimBLEDevice::deleteClient(g_meteoClient);
        g_meteoClient = nullptr;
        g_meteoRemoteCh = nullptr;
    }

    g_meteoClient = NimBLEDevice::createClient();
    g_meteoClient->setClientCallbacks(&g_meteoClientCbs, false);

    // short timeouts
    g_meteoClient->setConnectTimeout(4);

    Serial.printf("[BLE] Meteo connect -> %s\n", targetMac.c_str());
    g_meteoConnecting = true;
    g_meteoLastConnectAttemptMs = now;

    auto tryConnect = [&](uint8_t addrType) -> bool {
        // NimBLE-Arduino 2.x requires address type; most peripherals use public or random static.
        NimBLEAddress addr(std::string(targetMac.c_str()), addrType);
        return g_meteoClient->connect(addr);
    };

    if (!tryConnect(BLE_ADDR_PUBLIC)) {
        // Retry as random static (common for many ESP/Nordic peripherals)
        if (!tryConnect(BLE_ADDR_RANDOM)) {
            Serial.println("[BLE] Meteo connect failed");
            g_meteoNextActionMs = now + g_cfg.meteoReconnectMs;
            g_meteoConnectFails = (uint8_t)min(255, g_meteoConnectFails + 1);
            if (g_cfg.meteoAutoDiscover && g_meteoConnectFails >= METEO_FAILS_BEFORE_SCAN) {
                g_meteoForceDiscover = true;
                g_meteoNextDiscoverMs = 0;
            }
            g_meteoConnecting = false;
            g_meteoAutoSavePending = false;
            g_meteoAutoSaveMac = "";
            g_meteoAutoSaveName = "";
            return false;
        }
    }
    g_meteoConnectFails = 0;

    NimBLERemoteService* svc = g_meteoClient->getService(UUID_SVC_METEO);
    if (!svc) {
        Serial.println("[BLE] Meteo service not found");
        g_meteoClient->disconnect();
        g_meteoNextActionMs = now + g_cfg.meteoReconnectMs;
        g_meteoFailCount = (uint8_t)min(255, g_meteoFailCount + 1);
        if (g_cfg.meteoAutoDiscover && g_meteoFailCount >= METEO_FAILS_BEFORE_SCAN) {
            g_meteoForceDiscover = true;
            g_meteoNextDiscoverMs = 0;
        }
        g_meteoConnecting = false;
        g_meteoAutoSavePending = false;
        g_meteoAutoSaveMac = "";
        g_meteoAutoSaveName = "";
        return false;
    }

    g_meteoRemoteCh = svc->getCharacteristic(UUID_CH_METEO);
    if (!g_meteoRemoteCh) {
        Serial.println("[BLE] Meteo char not found");
        g_meteoClient->disconnect();
        g_meteoNextActionMs = now + g_cfg.meteoReconnectMs;
        g_meteoFailCount = (uint8_t)min(255, g_meteoFailCount + 1);
        if (g_cfg.meteoAutoDiscover && g_meteoFailCount >= METEO_FAILS_BEFORE_SCAN) {
            g_meteoForceDiscover = true;
            g_meteoNextDiscoverMs = 0;
        }
        g_meteoConnecting = false;
        g_meteoAutoSavePending = false;
        g_meteoAutoSaveMac = "";
        g_meteoAutoSaveName = "";
        return false;
    }

    if (g_meteoRemoteCh->canNotify()) {
        if (!g_meteoRemoteCh->subscribe(true, meteoOnNotify)) {
            Serial.println("[BLE] Meteo subscribe failed");
            g_meteoClient->disconnect();
            g_meteoNextActionMs = now + g_cfg.meteoReconnectMs;
            g_meteoFailCount = (uint8_t)min(255, g_meteoFailCount + 1);
            if (g_cfg.meteoAutoDiscover && g_meteoFailCount >= METEO_FAILS_BEFORE_SCAN) {
                g_meteoForceDiscover = true;
                g_meteoNextDiscoverMs = 0;
            }
            g_meteoConnecting = false;
            g_meteoAutoSavePending = false;
            g_meteoAutoSaveMac = "";
            g_meteoAutoSaveName = "";
            return false;
        }
    }

    // initial read (if possible)
    if (g_meteoRemoteCh->canRead()) {
        std::string v = g_meteoRemoteCh->readValue();
        if (v.size() >= 6) {
            meteoOnNotify(g_meteoRemoteCh, (uint8_t*)v.data(), v.size(), false);
        }
    }

    if (!g_meteoLastFixMs) {
        g_meteoLastFixMs = millis();
    }

    g_meteoLastSeenMac = normalizeMac(targetMac);
    meteoCommitAutoSaveIfPending(normalizeMac(targetMac));

    g_meteoNextActionMs = now + 15000; // další pokus o akci nejdříve za 15s
    g_meteoLastDisconnectReason = -1;
    g_meteoConnecting = false;
    return true;
}

// ---------- Public API ----------
void bleInit() {
    loadConfigFS();
    loadAllowlistFS();
    rgbLedInit();
    updateBleLed();

    if (!g_cfg.enabled) {
        Serial.println(F("[BLE] disabled (config)"));
        return;
    }

    if (g_bleInitialized) return;

    NimBLEDevice::init(g_cfg.deviceName.c_str());
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // max

    // Dual role
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);

    // Security setup
    if (g_cfg.securityMode != "off") {
        // bonding always for "bonding" and "passkey"
        NimBLEDevice::setSecurityAuth(true, g_cfg.securityMode == "passkey", true); // bonding, MITM, SC
        NimBLEDevice::setSecurityIOCap(g_cfg.securityMode == "passkey" ? BLE_HS_IO_DISPLAY_ONLY : BLE_HS_IO_NO_INPUT_OUTPUT);
        NimBLEDevice::setSecurityPasskey(g_cfg.passkey);
        NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
        NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

        Serial.printf("[BLE] Security: %s (passkey=%lu)\n", g_cfg.securityMode.c_str(), (unsigned long)g_cfg.passkey);
    } else {
        Serial.println(F("[BLE] Security: off"));
    }

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
    g_meteoNextActionMs = millis() + 3000;
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

    // periodic status notify (1 Hz)
    if (g_statusCh && (int32_t)(now - g_lastStatusNotifyMs) >= 1000) {
        g_lastStatusNotifyMs = now;
        String st = bleGetStatusJson();
        g_statusCh->setValue(st.c_str());
        g_statusCh->notify(true);
    }

    // meteo auto-discovery + client handling
    meteoStartScanIfNeeded();
    meteoConnectIfNeeded();

    if (g_meteoClient && g_meteoClient->isConnected()) {
        // if stale -> mark no fix (does not disconnect)
        if (g_meteoFix && (now - g_meteoLastMs) > METEO_STALE_MS) {
            g_meteoFix = false;
        }
    }

    if (g_cfg.meteoEnabled && g_cfg.meteoAutoDiscover) {
        if (g_meteoLastFixMs && (now - g_meteoLastFixMs) > METEO_STALE_MS) {
            g_meteoForceDiscover = true;
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
    doc["meteoScanning"] = g_meteoScanning;
    meteoObj["mac"] = g_cfg.meteoMac;
    meteoObj["activeMac"] = meteoTargetMac();
    meteoObj["activeSource"] = meteoTargetMac().length() ? (g_meteoRuntimeMac.length() ? "runtime" : "config") : "none";
    meteoObj["lastSeenMac"] = g_meteoLastSeenMac;
    meteoObj["lastSeenName"] = g_meteoLastSeenName;
    meteoObj["lastSeenRssi"] = g_meteoLastSeenRssi;
    meteoObj["lastRssi"] = g_meteoLastSeenRssi;
    meteoObj["connected"] = (g_meteoClient && g_meteoClient->isConnected());
    meteoObj["fix"] = g_meteoFix;
    meteoObj["failCount"] = g_meteoFailCount;
    meteoObj["connectFails"] = g_meteoConnectFails;
    meteoObj["lastAttemptMs"] = g_meteoLastConnectAttemptMs;
    meteoObj["lastDisconnectReason"] = g_meteoLastDisconnectReason;

    if (!g_cfg.meteoEnabled) {
        meteoObj["state"] = "disabled";
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
    doc["meteoDiscoverIntervalMs"] = g_cfg.meteoDiscoverIntervalMs;
    doc["meteoScanMs"] = g_cfg.meteoScanMs;
    doc["meteoReconnectMs"] = g_cfg.meteoReconnectMs;
    doc["schemaVersion"] = g_cfg.schemaVersion;

    String out;
    serializeJson(doc, out);
    return out;
}

bool bleSetConfigJson(const String& json) {
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, json)) return false;

    const bool prevEnabled = g_cfg.enabled;
    const bool prevAdvertise = g_cfg.advertise;
    const String prevName = g_cfg.deviceName;

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
    g_cfg.meteoDiscoverIntervalMs = (uint32_t)(doc["meteoDiscoverIntervalMs"] | g_cfg.meteoDiscoverIntervalMs);
    g_cfg.meteoScanMs = (uint32_t)(doc["meteoScanMs"] | g_cfg.meteoScanMs);
    g_cfg.meteoReconnectMs = (uint32_t)(doc["meteoReconnectMs"] | g_cfg.meteoReconnectMs);
    g_cfg.schemaVersion = (uint32_t)(doc["schemaVersion"] | g_cfg.schemaVersion);

    if (prevName != g_cfg.deviceName && g_bleInitialized) {
        g_bleNamePendingRestart = true;
    }

    if (doc.containsKey("meteoMac")) {
        g_meteoRuntimeMac = "";
        g_meteoForceDiscover = false;
        g_meteoFailCount = 0;
        g_meteoConnectFails = 0;
        g_meteoAutoSavePending = false;
    }
    if (!g_cfg.meteoEnabled) {
        g_meteoRuntimeMac = "";
        g_meteoForceDiscover = false;
        g_meteoFailCount = 0;
        g_meteoConnectFails = 0;
        g_meteoAutoSavePending = false;
    }

    if (!saveConfigFS()) return false;

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
    return true;
}

bool bleStopPairing() {
    g_pairingWindow = false;
    updateBleLed();
    g_pairingRoleHint = "";
    g_pairingUntilMs = 0;
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

    if (g_meteoClient && g_meteoClient->isConnected() && macEquals(meteoTargetMac(), normalized)) {
        g_meteoClient->disconnect();
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

    String s = id;
    s.trim();
    s.toLowerCase();

    if (s == "meteo" || s == "meteo.tempc" || s == "temp" || s == "tempc") {
        return bleGetMeteoTempC(outC);
    }

    // Unknown ID (reserved)
    outC = NAN;
    return false;
}
