#include "BleController.h"

#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include <string>

// NimBLE (Arduino-ESP32 core 3.x)
#include <NimBLEDevice.h>

#include "RelayController.h"
#include "LogicController.h"
#include "RgbLedController.h"

// ---------- Files ----------
static const char* BLE_CFG_PATH = "/ble.json";
static const char* BLE_PAIRED_PATH = "/ble_paired.json";

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
    String meteoMac = "";            // "AA:BB:CC:DD:EE:FF"
    uint32_t meteoScanMs = 4000;
    uint32_t meteoReconnectMs = 8000;
};

static BleConfig g_cfg;

// ---------- State ----------
static NimBLEServer* g_server = nullptr;
static uint16_t g_serverConnCount = 0;
static NimBLEService* g_ctrlSvc = nullptr;
static NimBLECharacteristic* g_statusCh = nullptr;
static NimBLECharacteristic* g_cmdCh = nullptr;
static NimBLECharacteristic* g_ackCh = nullptr;

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

// ---------- Helpers ----------
static String nowIso() {
    // bez RTC: jen uptime v sekundách, ale UI stačí
    uint32_t s = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "uptime+%lus", (unsigned long)s);
    return String(buf);
}

static bool fsReadAll(const char* path, String& out) {
    if (!LittleFS.begin()) return false;
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    out = f.readString();
    f.close();
    out.trim();
    return out.length() > 0;
}

static bool fsWriteAll(const char* path, const String& data) {
    if (!LittleFS.begin()) return false;
    File f = LittleFS.open(path, "w");
    if (!f) return false;
    f.print(data);
    f.close();
    return true;
}

static bool macEquals(const String& a, const String& b) {
    String A=a, B=b;
    A.toUpperCase(); B.toUpperCase();
    return A == B;
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
    if (!fsReadAll(BLE_PAIRED_PATH, s)) {
        loadAllowlistDefaults();
        return false;
    }
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, s)) {
        loadAllowlistDefaults();
        return false;
    }
    g_allow.clear();
    JsonArray arr = doc["devices"].as<JsonArray>();
    for (JsonVariant v : arr) {
        JsonObject o = v.as<JsonObject>();
        PairedDevice d;
        const char* mac = o["mac"] | "";
        const char* name = o["name"] | "";
        const char* role = o["role"] | "other";
        d.mac = String(mac);
        d.name = String(name);
        d.role = String(role);
        d.addedAt = (uint32_t)(o["addedAt"] | 0);
        if (d.mac.length()) g_allow.push_back(d);
    }
    return true;
}

static bool saveAllowlistFS() {
    DynamicJsonDocument doc(8192);
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
    return fsWriteAll(BLE_PAIRED_PATH, out);
}

static void loadConfigDefaults() {
    g_cfg = BleConfig();
}

static bool loadConfigFS() {
    String s;
    if (!fsReadAll(BLE_CFG_PATH, s)) {
        loadConfigDefaults();
        return false;
    }
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, s)) {
        loadConfigDefaults();
        return false;
    }

    g_cfg.enabled = doc["enabled"] | true;
    g_cfg.deviceName = String((const char*)(doc["deviceName"] | "ESP32-S3 HeatCtrl"));
    g_cfg.advertise = doc["advertise"] | true;

    g_cfg.securityMode = String((const char*)(doc["securityMode"] | "bonding"));
    g_cfg.passkey = (uint32_t)(doc["passkey"] | 123456);
    g_cfg.allowlistEnforced = doc["allowlistEnforced"] | true;

    g_cfg.meteoEnabled = doc["meteoEnabled"] | false;
    g_cfg.meteoMac = String((const char*)(doc["meteoMac"] | ""));
    g_cfg.meteoScanMs = (uint32_t)(doc["meteoScanMs"] | 4000);
    g_cfg.meteoReconnectMs = (uint32_t)(doc["meteoReconnectMs"] | 8000);
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
    doc["meteoScanMs"] = g_cfg.meteoScanMs;
    doc["meteoReconnectMs"] = g_cfg.meteoReconnectMs;

    String out;
    serializeJson(doc, out);
    return fsWriteAll(BLE_CFG_PATH, out);
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

// ---------- Command handling (from display/app) ----------
static void handleCommandJson(const String& s) {
    DynamicJsonDocument doc(2048);
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

    const bool connected = (g_server && g_server->getConnectedCount() > 0);
    if (connected) rgbLedSetMode(RgbLedMode::BLE_CONNECTED);
    else rgbLedSetMode(RgbLedMode::BLE_IDLE);
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
        const String mac = macToString(connInfo.getAddress());
        Serial.printf("[BLE] Client connected: %s\n", mac.c_str());
        g_serverConnCount = (uint16_t)(g_server ? g_server->getConnectedCount() : 1);
        updateBleLed();

        // Pokud je otevřené párovací okno, zařadíme zařízení do allowlistu
        // (šifrování/bonding si NimBLE řeší interně dle nastavení securityAuth).
        if (g_pairingWindow && !isInAllowlist(mac)) {
            PairedDevice d;
            d.mac = mac;
            d.name = "";
            d.role = g_pairingRoleHint.length() ? g_pairingRoleHint : "other";
            d.addedAt = millis() / 1000;
            g_allow.push_back(d);
            saveAllowlistFS();
            Serial.printf("[BLE] Added to allowlist: %s (%s)\n", mac.c_str(), d.role.c_str());
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
        const String mac = macToString(connInfo.getAddress());
        Serial.printf("[BLE] Client disconnected: %s (reason=%d)\n", mac.c_str(), reason);
        g_serverConnCount = (uint16_t)(g_server ? g_server->getConnectedCount() : 0);
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
}

static bool meteoConnectIfNeeded() {
    if (!g_cfg.meteoEnabled) return false;
    if (!g_cfg.meteoMac.length()) return false;

    const uint32_t now = millis();
    if ((int32_t)(now - g_meteoNextActionMs) < 0) return false;

    if (g_meteoClient && g_meteoClient->isConnected()) return true;

    // cleanup old
    if (g_meteoClient) {
        NimBLEDevice::deleteClient(g_meteoClient);
        g_meteoClient = nullptr;
        g_meteoRemoteCh = nullptr;
    }

    NimBLEAddress addr(std::string(g_cfg.meteoMac.c_str()), BLE_ADDR_PUBLIC);
    g_meteoClient = NimBLEDevice::createClient();

    // short timeouts
    g_meteoClient->setConnectTimeout(4);

    Serial.printf("[BLE] Meteo connect -> %s\n", g_cfg.meteoMac.c_str());
    if (!g_meteoClient->connect(addr)) {
        Serial.println("[BLE] Meteo connect failed");
        g_meteoNextActionMs = now + g_cfg.meteoReconnectMs;
        return false;
    }

    NimBLERemoteService* svc = g_meteoClient->getService(UUID_SVC_METEO);
    if (!svc) {
        Serial.println("[BLE] Meteo service not found");
        g_meteoClient->disconnect();
        g_meteoNextActionMs = now + g_cfg.meteoReconnectMs;
        return false;
    }

    g_meteoRemoteCh = svc->getCharacteristic(UUID_CH_METEO);
    if (!g_meteoRemoteCh) {
        Serial.println("[BLE] Meteo char not found");
        g_meteoClient->disconnect();
        g_meteoNextActionMs = now + g_cfg.meteoReconnectMs;
        return false;
    }

    if (g_meteoRemoteCh->canNotify()) {
        g_meteoRemoteCh->subscribe(true, meteoOnNotify);
    }

    // initial read (if possible)
    if (g_meteoRemoteCh->canRead()) {
        std::string v = g_meteoRemoteCh->readValue();
        if (v.size() >= 6) {
            meteoOnNotify(g_meteoRemoteCh, (uint8_t*)v.data(), v.size(), false);
        }
    }

    g_meteoNextActionMs = now + 15000; // další pokus o akci nejdříve za 15s
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
}

void bleLoop() {
    if (!g_cfg.enabled) return;

    const uint32_t now = millis();

    // pairing window timeout
    if (g_pairingWindow && (int32_t)(now - g_pairingUntilMs) >= 0) {
        g_pairingWindow = false;
    updateBleLed();
        g_pairingRoleHint = "";
        Serial.println(F("[BLE] Pairing window closed"));
        updateBleLed();
    }

    // periodic status notify (1 Hz)
    if (g_statusCh && (int32_t)(now - g_lastStatusNotifyMs) >= 1000) {
        g_lastStatusNotifyMs = now;
        String st = bleGetStatusJson();
        g_statusCh->setValue(st.c_str());
        g_statusCh->notify(true);
    }

    // meteo client handling
    meteoConnectIfNeeded();

    if (g_meteoClient && g_meteoClient->isConnected()) {
        // if stale -> mark no fix (does not disconnect)
        if (g_meteoFix && (now - g_meteoLastMs) > 120000) {
            g_meteoFix = false;
        }
    }
}

String bleGetStatusJson() {
    DynamicJsonDocument doc(2048);
    doc["enabled"] = g_cfg.enabled;
    doc["securityMode"] = g_cfg.securityMode;
    doc["allowlistEnforced"] = g_cfg.allowlistEnforced;

    doc["pairingWindow"] = g_pairingWindow;
    doc["pairingRoleHint"] = g_pairingRoleHint;
    doc["pairingRemainingSec"] = g_pairingWindow ? (uint32_t)((g_pairingUntilMs - millis())/1000) : 0;

    doc["meteoEnabled"] = g_cfg.meteoEnabled;
    doc["meteoConnected"] = (g_meteoClient && g_meteoClient->isConnected());
    doc["meteoFix"] = g_meteoFix;

    if (g_meteoFix) {
        JsonObject m = doc.createNestedObject("meteo");
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
    doc["meteoScanMs"] = g_cfg.meteoScanMs;
    doc["meteoReconnectMs"] = g_cfg.meteoReconnectMs;

    String out;
    serializeJson(doc, out);
    return out;
}

bool bleSetConfigJson(const String& json) {
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, json)) return false;

    g_cfg.enabled = doc["enabled"] | g_cfg.enabled;
    g_cfg.deviceName = String((const char*)(doc["deviceName"] | g_cfg.deviceName.c_str()));
    g_cfg.advertise = doc["advertise"] | g_cfg.advertise;

    g_cfg.securityMode = String((const char*)(doc["securityMode"] | g_cfg.securityMode.c_str()));
    g_cfg.passkey = (uint32_t)(doc["passkey"] | g_cfg.passkey);
    g_cfg.allowlistEnforced = doc["allowlistEnforced"] | g_cfg.allowlistEnforced;

    g_cfg.meteoEnabled = doc["meteoEnabled"] | g_cfg.meteoEnabled;
    g_cfg.meteoMac = String((const char*)(doc["meteoMac"] | g_cfg.meteoMac.c_str()));
    g_cfg.meteoScanMs = (uint32_t)(doc["meteoScanMs"] | g_cfg.meteoScanMs);
    g_cfg.meteoReconnectMs = (uint32_t)(doc["meteoReconnectMs"] | g_cfg.meteoReconnectMs);

    return saveConfigFS();
}

String bleGetPairedJson() {
    DynamicJsonDocument doc(8192);
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
    bool removed = false;
    for (size_t i=0;i<g_allow.size();){
        if (macEquals(g_allow[i].mac, mac)) {
            g_allow.erase(g_allow.begin()+i);
            removed = true;
        } else i++;
    }
    if (removed) saveAllowlistFS();

    // best-effort remove bond
    NimBLEAddress addr(std::string(mac.c_str()), BLE_ADDR_PUBLIC);
    NimBLEDevice::deleteBond(addr);

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
