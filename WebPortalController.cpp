#include "Features.h"
#include "WebPortalController.h"

#if defined(FEATURE_WEBPORTAL)

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <algorithm>
#include <functional>
#include <stdio.h>
#include <string.h>

#include "WebPortalAssets.h"

#include "RelayController.h"
#include "InputController.h"
#include "ConfigStore.h"
#include "DallasController.h"
#include "TemperatureManager.h"

#include "OpenThermController.h"
#include "BleController.h"
#include "NetworkController.h"
#include "OtaController.h"
#include "EquithermController.h"
#include "MqttController.h"
#include "DhwController.h"
#include "PressureAlarmController.h"
#include "BuzzerController.h"
#include "EventLog.h"
#include "HistoryBuffer.h"
#include "ConfigRuntime.h"
#include "config_pins.h"

namespace {
  WebServer g_srv(80);
  WebSocketsServer g_ws(81);
  bool g_started = false;

  bool g_fsMounted = false;
  unsigned long g_wsLastPushMs = 0;
  uint8_t g_wsClientCount = 0;

  static constexpr size_t kFsReadChunkDefault = 4096;
  static constexpr size_t kFsReadChunkMax = 32768;
  static constexpr bool kServePrecompressedAssets = true;
  static constexpr size_t kActionLogCapacity = 16;
  static constexpr unsigned long kActionLogRetentionMs = 15UL * 60UL * 1000UL;
  static constexpr uint8_t kRelayMixOpenIdx = 0;       // R1
  static constexpr uint8_t kRelayMixCloseIdx = 1;      // R2
  static constexpr uint8_t kRelayDhwValveIdx = 2;      // R3
  static constexpr uint8_t kRelayDhwCircIdx = 3;       // R4
  static constexpr uint8_t kRelayDhwBoilerIdx = 4;     // R5
  static constexpr uint8_t kRelayAccuHeaterIdx = 7;    // R8
  static constexpr uint8_t kRelayMixOpenBit = (uint8_t)(1u << kRelayMixOpenIdx);
  static constexpr uint8_t kRelayMixCloseBit = (uint8_t)(1u << kRelayMixCloseIdx);
  static constexpr uint8_t kRelayAccuHeaterBit = (uint8_t)(1u << kRelayAccuHeaterIdx);

  struct RateLimitEntry {
    const char* key = nullptr;
    unsigned long windowStartMs = 0;
    unsigned long lastAttemptMs = 0;
    uint16_t attemptsInWindow = 0;
  };

  struct AdminActionLogEntry {
    unsigned long ms = 0;
    char ip[24] = {0};
    char action[24] = {0};
    char result[20] = {0};
    char detail[80] = {0};
  };

  AdminActionLogEntry g_actionLog[kActionLogCapacity];
  size_t g_actionLogCount = 0;
  size_t g_actionLogHead = 0;

  struct UploadContext {
    bool active = false;
    bool ok = false;
    bool rebootAfter = false;
    String targetPath;
    String message;
    File file;
    size_t bytesReceived = 0;
    size_t expectedSize = 0;
    size_t partitionSize = 0;
  };

  UploadContext g_fsUpload;
  UploadContext g_fwUpload;
  UploadContext g_fsImageUpload;

  struct FastSectionCache {
    String sys;
    String temps;
    String rel;
    String in;
    String ot;
    String ble;
    String ota;
    String time;
    String eq;
    String dhw;
    String alerts;
    bool primed = false;
    uint32_t seq = 0;
  };

  FastSectionCache g_fastCache;

  struct ConfigSectionDef {
    const char* name;
    size_t postDocCap;
    const char* rateKey;
  };

  static constexpr ConfigSectionDef kConfigSections[] = {
    {"inputs", 1024, "cfg_inputs"},
    {"opentherm", 2048, "cfg_ot"},
    {"ble", 1024, "cfg_ble"},
    {"dallas", 4096, "cfg_dallas"},
    {"ota", 1024, "cfg_ota"},
    {"mqtt", 4096, "cfg_mqtt"},
    {"time", 1024, "cfg_time"},
    {"equitherm", 4096, "cfg_eq"},
    {"dhw", 8192, "cfg_dhw"},
    {"alerts", 1024, "cfg_alerts"},
  };

  static const ConfigSectionDef* findConfigSection(const char* section) {
    if (!section || !*section) return nullptr;
    for (const auto& def : kConfigSections) {
      if (strcmp(def.name, section) == 0) return &def;
    }
    return nullptr;
  }

  static const char* kCollectedHeaders[] = {"Accept-Encoding"};

  static const char* getContentType(const String& path) {
    if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html; charset=utf-8";
    if (path.endsWith(".css")) return "text/css; charset=utf-8";
    if (path.endsWith(".js")) return "application/javascript; charset=utf-8";
    if (path.endsWith(".json")) return "application/json; charset=utf-8";
    if (path.endsWith(".svg")) return "image/svg+xml";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".ico")) return "image/x-icon";
    if (path.endsWith(".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
  }

  static bool clientAcceptsGzip() {
    String enc = g_srv.header("Accept-Encoding");
    enc.toLowerCase();
    return enc.indexOf("gzip") >= 0;
  }

  static bool isValidGzipFile(const String& path) {
    // Validate precompressed LittleFS assets only once per request candidate.
    // A bad .gz served with Content-Encoding would break the whole UI load.
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    const int b0 = f.read();
    const int b1 = f.read();
    f.close();
    return b0 == 0x1f && b1 == 0x8b;
  }

  static String ensureLeadingSlash(String path);

  static void sendStaticCacheHeaders(const String& path) {
    g_srv.sendHeader("Vary", "Accept-Encoding");
    if (path.endsWith(".css") || path.endsWith(".js")) {
      g_srv.sendHeader("Cache-Control", "public, max-age=86400, immutable");
      return;
    }
    if (path.endsWith(".html")) {
      g_srv.sendHeader("Cache-Control", "no-cache");
      return;
    }
    g_srv.sendHeader("Cache-Control", "public, max-age=300");
  }

  static bool tryServeFsFile(const String& requestPath) {
    if (!g_fsMounted) return false;

    String path = requestPath;
    const int qmark = path.indexOf('?');
    if (qmark >= 0) path.remove(qmark);
    const int hash = path.indexOf('#');
    if (hash >= 0) path.remove(hash);
    if (!path.length()) path = "/";
    path = ensureLeadingSlash(path);
    if (path == "/") path = "/index.html";
    if (path.endsWith("/")) path += "index.html";

    String servePath = path;
    bool gzipped = false;
    const bool gzipEligible = kServePrecompressedAssets && (path.endsWith(".css") || path.endsWith(".js") || path.endsWith(".svg") || path.endsWith(".json") || path.endsWith(".txt"));
    if (gzipEligible && clientAcceptsGzip()) {
      const String gzPath = path + ".gz";
      if (LittleFS.exists(gzPath) && isValidGzipFile(gzPath)) {
        servePath = gzPath;
        gzipped = true;
      }
    }
    if (!LittleFS.exists(servePath)) {
      if (!LittleFS.exists(path)) return false;
      servePath = path;
      gzipped = false;
    }

    File f = LittleFS.open(servePath, "r");
    if (!f) return false;

    sendStaticCacheHeaders(path);
    g_srv.sendHeader("X-Content-Type-Options", "nosniff");
    if (gzipped) g_srv.sendHeader("Content-Encoding", "gzip");
    g_srv.streamFile(f, getContentType(path));
    f.close();
    return true;
  }

  static String ensureLeadingSlash(String path) {
    path.trim();
    path.replace('\\', '/');
    if (!path.length()) return String("/");
    if (!path.startsWith("/")) path = "/" + path;
    while (path.indexOf("//") >= 0) path.replace("//", "/");
    return path;
  }

  static bool isSafeFsPath(const String& path) {
    if (!path.length() || path[0] != '/') return false;
    if (path.indexOf("..") >= 0) return false;
    return true;
  }

  static String pickUploadPath(const String& requestedPath, const String& filename) {
    String path = requestedPath.length() ? requestedPath : filename;
    path = ensureLeadingSlash(path);
    if (path.endsWith("/")) path += filename;
    if (!isSafeFsPath(path)) return String();
    return path;
  }

  static String jsonResponse(DynamicJsonDocument& doc);
  static void sendJson(int code, const String& body);
  static void sendJsonDoc(int code, DynamicJsonDocument& doc);
  static String getBestIpString();
  static void fillInputs(JsonObject out);
  static void fillTemps(JsonObject out);
  static bool ensureConfigDir();
  static bool writeJsonVariantToFile(const char* path, JsonVariantConst value);
  static void applyInputsSection(JsonObjectConst in);
  static void applyOpenThermSection(JsonObjectConst o);
  static void applyBleSection(JsonObjectConst b);
  static void applyOtaSection(JsonObjectConst o);
  static void applyTimeSection(JsonObjectConst t);
  static void applyDallasSection(JsonObjectConst d);
  static void applyMqttSection(JsonObjectConst m);
  static void applyEquithermSection(JsonObjectConst e);
  static void applyDhwSection(JsonObjectConst d);
  static void applyAlertsSection(JsonObjectConst a);
  static void applySectionByName(const String& section, JsonObjectConst root);
  static void fillHeapJson(JsonObject out);
  static void fillAdminActionsJson(JsonArray out);
  static void recordAdminAction(const char* action, bool ok, const char* detail = nullptr);
  static bool allowAction(const char* key, unsigned long minIntervalMs, uint16_t maxPerWindow, unsigned long windowMs, const char* detailOnBlock = nullptr);
  static size_t getFsReadLimitArg();
  static size_t getFsReadOffsetArg();
  static void sendFsReadChunk(File& f, const String& path, size_t totalSize, size_t offset, size_t limit);

  static void writeUploadJson(int code, bool ok, const String& msg, const String& path = String()) {
    DynamicJsonDocument doc(512);
    doc["ok"] = ok;
    doc["msg"] = msg;
    if (path.length()) doc["path"] = path;
    sendJsonDoc(code, doc);
  }

  static bool isTextLikePath(const String& path) {
    return path.endsWith(".txt") || path.endsWith(".log") || path.endsWith(".json") ||
           path.endsWith(".js") || path.endsWith(".css") || path.endsWith(".html") ||
           path.endsWith(".htm") || path.endsWith(".md") || path.endsWith(".csv") ||
           path.endsWith(".xml") || path.endsWith(".ini") || path.endsWith(".yaml") ||
           path.endsWith(".yml") || path.endsWith(".svg");
  }

  static size_t getFirmwarePartitionLimit() {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (part && part->size > 0) return (size_t)part->size;
    return (size_t)ESP.getFreeSketchSpace();
  }

  static size_t getFilesystemPartitionLimit() {
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (part && part->size > 0) return (size_t)part->size;
    return 0;
  }

  static void fillUploadJson(JsonObject out, const UploadContext& u) {
    out["active"] = u.active;
    out["ok"] = u.ok;
    out["message"] = u.message.length() ? u.message : (u.active ? "uploading" : "idle");
    out["receivedBytes"] = (uint32_t)u.bytesReceived;
    out["expectedBytes"] = (uint32_t)u.expectedSize;
    out["partitionBytes"] = (uint32_t)u.partitionSize;
    if (u.targetPath.length()) out["path"] = u.targetPath; else out["path"] = nullptr;
  }

  static const char* getRemoteIpCstr() {
    static char ipBuf[24];
    ipBuf[0] = 0;
    WiFiClient client = g_srv.client();
    IPAddress ip = client.remoteIP();
    snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return ipBuf;
  }

  static void copyTrunc(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    if (!src) src = "";
    snprintf(dst, dstSize, "%s", src);
  }

  static void recordAdminAction(const char* action, bool ok, const char* detail) {
    AdminActionLogEntry& e = g_actionLog[g_actionLogHead];
    e.ms = millis();
    copyTrunc(e.ip, sizeof(e.ip), getRemoteIpCstr());
    copyTrunc(e.action, sizeof(e.action), action ? action : "unknown");
    copyTrunc(e.result, sizeof(e.result), ok ? "ok" : "blocked");
    copyTrunc(e.detail, sizeof(e.detail), detail ? detail : "");
    EventLog::record("admin", action ? action : "unknown", detail ? detail : "", ok ? "info" : "warn");
    g_actionLogHead = (g_actionLogHead + 1) % kActionLogCapacity;
    if (g_actionLogCount < kActionLogCapacity) ++g_actionLogCount;
  }

  static void fillAdminActionsJson(JsonArray out) {
    const unsigned long now = millis();
    const size_t start = (g_actionLogHead + kActionLogCapacity - g_actionLogCount) % kActionLogCapacity;
    for (size_t i = 0; i < g_actionLogCount; ++i) {
      const AdminActionLogEntry& e = g_actionLog[(start + i) % kActionLogCapacity];
      if (e.ms == 0 || (now - e.ms) > kActionLogRetentionMs) continue;
      JsonObject item = out.createNestedObject();
      item["ms"] = (uint32_t)e.ms;
      item["ip"] = e.ip;
      item["action"] = e.action;
      item["result"] = e.result;
      if (e.detail[0] != 0) item["detail"] = e.detail;
    }
  }

  static bool allowAction(const char* key, unsigned long minIntervalMs, uint16_t maxPerWindow, unsigned long windowMs, const char* detailOnBlock) {
    static RateLimitEntry entries[] = {
      {"relay"}, {"config"}, {"reboot"}, {"system_cmd"}, {"dhw_cmd"}, {"ot_cmd"}, {"eq_cmd"}, {"ot_scan_start"}, {"ot_scan_stop"},
      {"ot_data_write"}, {"cfg_inputs"}, {"cfg_ot"}, {"cfg_ble"}, {"cfg_dallas"}, {"cfg_ota"}, {"cfg_mqtt"},
      {"cfg_time"}, {"cfg_eq"}, {"cfg_dhw"}, {"cfg_alerts"}, {"cfg_apply"}, {"cfg_import"}, {"cfg_export"},
      {"fs_write"}, {"fs_mkdir"}, {"fs_rename"}, {"fs_delete"}, {"fs_upload"}, {"fw_update"}, {"fs_update"}
    };
    const unsigned long now = millis();
    for (RateLimitEntry& e : entries) {
      if (strcmp(e.key, key) != 0) continue;
      if (e.windowStartMs == 0 || (now - e.windowStartMs) > windowMs) {
        e.windowStartMs = now;
        e.attemptsInWindow = 0;
      }
      if (e.lastAttemptMs != 0 && (now - e.lastAttemptMs) < minIntervalMs) {
        recordAdminAction(key, false, detailOnBlock ? detailOnBlock : "debounced");
        return false;
      }
      if (maxPerWindow > 0 && e.attemptsInWindow >= maxPerWindow) {
        recordAdminAction(key, false, detailOnBlock ? detailOnBlock : "rate_limited");
        return false;
      }
      e.lastAttemptMs = now;
      ++e.attemptsInWindow;
      return true;
    }
    return true;
  }

  static void fillHeapJson(JsonObject out) {
    out["free"] = (uint32_t)ESP.getFreeHeap();
    out["minFree"] = (uint32_t)ESP.getMinFreeHeap();
    out["maxAlloc"] = (uint32_t)ESP.getMaxAllocHeap();
    out["psramFree"] = (uint32_t)ESP.getFreePsram();
  }

  static String serializeJsonObject(const std::function<void(JsonObject&)>& filler, size_t cap = 512) {
    DynamicJsonDocument doc(cap);
    JsonObject obj = doc.to<JsonObject>();
    filler(obj);
    String out;
    serializeJson(doc, out);
    return out;
  }

  static void fillFastWsStateObject(JsonObject out) {
    JsonObject sys = out.createNestedObject("sys");
    const bool wifiOk = networkIsWifiConnected();
    const bool ethOk = networkIsEthernetConnected();
    const String ip = getBestIpString();

    sys["wifi"] = wifiOk;
    sys["eth"] = ethOk;
    sys["ip"] = ip;
    sys["uptimeSec"] = (uint32_t)(millis() / 1000UL);
    const int rssi = wifiOk ? WiFi.RSSI() : 0;
    if (wifiOk) sys["rssi"] = rssi; else sys["rssi"] = nullptr;

    // Backward-compatible root aliases used by older UI code.
    out["wifi"] = wifiOk;
    out["eth"] = ethOk;
    out["ip"] = ip;
    if (wifiOk) out["rssi"] = rssi; else out["rssi"] = nullptr;
    JsonObject system = out.createNestedObject("system");
    system["uptimeSec"] = (uint32_t)(millis() / 1000UL);

    JsonObject temps = out.createNestedObject("temps");
    fillTemps(temps);

    JsonObject rel = out.createNestedObject("rel");
    rel["mask"] = relayGetMask();
    rel["ok"] = relayIsOk();
    rel["i2cErr"] = relayGetI2cErrorCount();
    rel["i2cRec"] = relayGetI2cRecoveryCount();

    JsonObject in = out.createNestedObject("in");
    fillInputs(in);

    JsonObject ot = out.createNestedObject("ot");
    openthermFillFastJson(ot);

    JsonObject ble = out.createNestedObject("ble");
    bleFillFastJson(ble);

    JsonObject ota = out.createNestedObject("ota");
    otaFillFastJson(ota);
    JsonObject upload = ota.createNestedObject("upload");
    JsonObject fw = upload.createNestedObject("fw");
    fillUploadJson(fw, g_fwUpload);
    JsonObject fs = upload.createNestedObject("fs");
    fillUploadJson(fs, g_fsImageUpload);

    JsonObject time = out.createNestedObject("time");
    time["valid"] = networkIsTimeValid();
    time["src"] = networkGetTimeSource();
    if (networkIsTimeValid()) time["epochMin"] = (uint32_t)(millis() / 60000UL);
    else time["epochMin"] = nullptr;

    JsonObject eq = out.createNestedObject("eq");
    equithermFillFastJson(eq);

    JsonObject dhw = out.createNestedObject("dhw");
    dhwFillFastJson(dhw);

    JsonObject alerts = out.createNestedObject("alerts");
    pressureAlarmFillFastJson(alerts);
  }

  static String serializeJsonVariant(JsonVariantConst value) {
    String out;
    serializeJson(value, out);
    return out;
  }

  static String buildFastWsFrame(bool forceFull) {
    // Build the current fast snapshot once and reuse its JsonVariant values for
    // the outgoing WebSocket frame. The previous version serialized each
    // section into a String and then parsed it back into another JSON document;
    // avoiding that parse cycle reduces heap churn and CPU load on ESP32.
    DynamicJsonDocument curDoc(4096);
    JsonObject cur = curDoc.to<JsonObject>();
    fillFastWsStateObject(cur);

    const String sysStr = serializeJsonVariant(cur["sys"]);
    const String tempsStr = serializeJsonVariant(cur["temps"]);
    const String relStr = serializeJsonVariant(cur["rel"]);
    const String inStr = serializeJsonVariant(cur["in"]);
    const String otStr = serializeJsonVariant(cur["ot"]);
    const String bleStr = serializeJsonVariant(cur["ble"]);
    const String otaStr = serializeJsonVariant(cur["ota"]);
    const String timeStr = serializeJsonVariant(cur["time"]);
    const String eqStr = serializeJsonVariant(cur["eq"]);
    const String dhwStr = serializeJsonVariant(cur["dhw"]);
    const String alertsStr = serializeJsonVariant(cur["alerts"]);

    DynamicJsonDocument doc(4096);
    doc["seq"] = ++g_fastCache.seq;
    if (forceFull || !g_fastCache.primed) {
      doc["type"] = "fast_full";
      doc.createNestedObject("data").set(cur);
      g_fastCache = {sysStr, tempsStr, relStr, inStr, otStr, bleStr, otaStr, timeStr, eqStr, dhwStr, alertsStr, true, g_fastCache.seq};
    } else {
      JsonObject changed = doc.createNestedObject("changed");
      auto maybe = [&](const char* key, String& prev, const String& curSerialized){
        if (prev != curSerialized) {
          changed[key].set(cur[key].as<JsonVariantConst>());
          prev = curSerialized;
        }
      };
      doc["type"] = "fast_patch";
      maybe("temps", g_fastCache.temps, tempsStr);
      maybe("rel", g_fastCache.rel, relStr);
      maybe("in", g_fastCache.in, inStr);
      maybe("ot", g_fastCache.ot, otStr);
      maybe("ble", g_fastCache.ble, bleStr);
      maybe("ota", g_fastCache.ota, otaStr);
      maybe("time", g_fastCache.time, timeStr);
      maybe("eq", g_fastCache.eq, eqStr);
      maybe("dhw", g_fastCache.dhw, dhwStr);
      maybe("alerts", g_fastCache.alerts, alertsStr);
      if (g_fastCache.sys != sysStr) {
        changed["wifi"] = cur["wifi"];
        changed["eth"] = cur["eth"];
        changed["ip"] = cur["ip"];
        changed["rssi"] = cur["rssi"];
        changed["sys"].set(cur["sys"].as<JsonVariantConst>());
        g_fastCache.sys = sysStr;
      }
      if (changed.size() == 0) return String();
    }
    String out;
    serializeJson(doc, out);
    return out;
  }

  static bool removeFsEntryRecursive(const String& path) {
    if (!g_fsMounted) return false;
    if (!LittleFS.exists(path)) return false;

    File entry = LittleFS.open(path, "r");
    if (!entry) return false;

    if (!entry.isDirectory()) {
      entry.close();
      return LittleFS.remove(path);
    }

    File child = entry.openNextFile();
    while (child) {
      String childPath = String(child.name());
      const bool isDir = child.isDirectory();
      child.close();
      if (!removeFsEntryRecursive(childPath)) {
        entry.close();
        return false;
      }
      child = entry.openNextFile();
    }
    entry.close();
    return LittleFS.rmdir(path);
  }

  static bool writeTextFile(const String& path, const String& content) {
    if (!g_fsMounted || !isSafeFsPath(path) || path == "/") return false;
    File f = LittleFS.open(path, "w");
    if (!f) return false;
    const size_t expected = content.length();
    const size_t written = f.print(content);
    f.close();
    return written == expected;
  }


  static String joinFsPath(const String& parent, const String& childName) {
    String base = parent.length() ? parent : String("/");
    if (!base.startsWith("/")) base = "/" + base;
    if (base.length() > 1 && base.endsWith("/")) base.remove(base.length() - 1);

    String name = childName;
    name.trim();
    name.replace('\\', '/');
    while (name.startsWith("./")) name.remove(0, 2);
    while (name.startsWith("/")) name.remove(0, 1);
    if (!name.length()) return base;
    return base == "/" ? "/" + name : base + "/" + name;
  }

  static String fsBaseName(const String& path) {
    if (path == "/") return String("/");
    const int slash = path.lastIndexOf('/');
    if (slash < 0) return path;
    if (slash == (int)path.length() - 1) return String();
    return path.substring(slash + 1);
  }

  static void appendFsEntries(JsonArray arr, File dir, const String& parentPath = String("/")) {
    if (!dir) return;
    File entry = dir.openNextFile();
    while (entry) {
      const bool isDir = entry.isDirectory();
      const String rawName = String(entry.name());
      String entryPath = ensureLeadingSlash(rawName);
      if (parentPath != "/" && !entryPath.startsWith(parentPath + "/")) entryPath = joinFsPath(parentPath, rawName);
      entryPath = ensureLeadingSlash(entryPath);
      JsonObject item = arr.createNestedObject();
      item["path"] = entryPath;
      item["name"] = fsBaseName(entryPath);
      item["dir"] = isDir;
      item["size"] = isDir ? 0 : (uint32_t)entry.size();
      if (isDir) appendFsEntries(arr, entry, entryPath);
      entry = dir.openNextFile();
    }
  }

  static String jsonResponse(DynamicJsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    return out;
  }

  static void sendJson(int code, const String& body) {
    g_srv.sendHeader("Cache-Control", "no-store");
    g_srv.send(code, "application/json; charset=utf-8", body);
  }

  static void sendJsonDoc(int code, DynamicJsonDocument& doc) {
    sendJson(code, jsonResponse(doc));
  }

  static String getBestIpString() {
    const String ip = networkGetIp();
    if (ip.length()) return ip;
    IPAddress ap = WiFi.softAPIP();
    if (ap[0] != 0) return ap.toString();
    return String("0.0.0.0");
  }

  static void fillInputs(JsonObject out) {
    uint8_t rawMask = 0;
    uint8_t actMask = 0;
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
      const bool raw = inputGetRaw((InputId)i);
      const bool act = inputGetState((InputId)i);
      if (raw) rawMask |= (1u << i);
      if (act) actMask |= (1u << i);
    }
    out["rawMask"] = rawMask;
    out["actMask"] = actMask;

    JsonArray lvl = out.createNestedArray("lvl");
    for (uint8_t i = 0; i < INPUT_COUNT; i++) {
      lvl.add((int)ConfigStore::getInputActiveLevel(i));
    }
  }

  static void fillTemps(JsonObject out) {
    TemperatureManager::fillTempsJson(out);
  }

  static void applyDallasEnabled(bool en) {
    DallasController::configureGpio(DALLAS_IO0_PIN, en ? TEMP_INPUT_DALLAS : TEMP_INPUT_NONE);
    DallasController::configureGpio(DALLAS_DHW_RETURN_PIN, en ? TEMP_INPUT_DALLAS : TEMP_INPUT_NONE);
    DallasController::configureGpio(DALLAS_RETURN_PIN, en ? TEMP_INPUT_DALLAS : TEMP_INPUT_NONE);
    DallasController::configureGpio(DALLAS_TANK_PIN, en ? TEMP_INPUT_DALLAS : TEMP_INPUT_NONE);
  }

  static void sendEmbeddedAsset(const char* contentType, const char* data, const char* cacheControl) {
    g_srv.sendHeader("Cache-Control", cacheControl);
    g_srv.send_P(200, contentType, data);
  }

  static void handleFileManager() {
    if (tryServeFsFile("/index.html")) return;
    sendEmbeddedAsset("text/html; charset=utf-8", WEB_INDEX_HTML, "no-cache");
  }

  static void handleRoot() {
    if (tryServeFsFile("/index.html")) return;
    handleFileManager();
  }

  static void handleCss() {
    if (tryServeFsFile("/app.css")) return;
    sendEmbeddedAsset("text/css; charset=utf-8", WEB_APP_CSS, "public, max-age=86400");
  }

  static void handleJs() {
    if (tryServeFsFile("/app.js")) return;
    sendEmbeddedAsset("application/javascript; charset=utf-8", WEB_APP_JS, "public, max-age=86400");
  }


  static bool fillConfigSectionDoc(const String& section, DynamicJsonDocument& doc);
  static bool loadConfigSectionFromSnapshot(const char* section, DynamicJsonDocument& doc);
  static bool loadConfigSectionLiveOrSnapshot(const char* section, DynamicJsonDocument& doc);
  static bool fillConfigSectionObject(const String& section, JsonObject out);
  static void fillInputsSectionJson(JsonObject out);
  static void fillOpenThermSectionJson(JsonObject out);
  static void fillBleSectionJson(JsonObject out);
  static void fillDallasSectionJson(JsonObject out);
  static void fillOtaSectionJson(JsonObject out);
  static void fillMqttSectionJson(JsonObject out);
  static void fillTimeSectionJson(JsonObject out);
  static void fillEquithermSectionJson(JsonObject out);
  static void fillDhwSectionJson(JsonObject out);
  static void fillAlertsSectionJson(JsonObject out);
  static void fillFastStateObject(JsonObject out);

  static void fillFastStateObject(JsonObject out) {
    out["ms"] = (uint32_t)millis();
    const bool wifiOk = networkIsWifiConnected();
    out["wifi"] = wifiOk;
    out["eth"] = networkIsEthernetConnected();
    out["ip"] = getBestIpString();
    if (wifiOk) out["rssi"] = WiFi.RSSI(); else out["rssi"] = nullptr;
    JsonObject system = out.createNestedObject("system");
    system["uptimeSec"] = (uint32_t)(millis() / 1000UL);

    JsonObject temps = out.createNestedObject("temps");
    fillTemps(temps);

    JsonObject rel = out.createNestedObject("rel");
    rel["mask"] = relayGetMask();
    rel["ok"] = relayIsOk();
    rel["i2cErr"] = relayGetI2cErrorCount();
    rel["i2cRec"] = relayGetI2cRecoveryCount();

    JsonObject in = out.createNestedObject("in");
    fillInputs(in);

    JsonObject ot = out.createNestedObject("ot");
    openthermFillFastJson(ot);

    JsonObject ble = out.createNestedObject("ble");
    bleFillFastJson(ble);

    JsonObject ota = out.createNestedObject("ota");
    otaFillFastJson(ota);
    JsonObject upload = out.createNestedObject("upload");
    JsonObject fw = upload.createNestedObject("fw");
    fillUploadJson(fw, g_fwUpload);
    JsonObject fs = upload.createNestedObject("fs");
    fillUploadJson(fs, g_fsImageUpload);

    JsonObject time = out.createNestedObject("time");
    const bool timeValid = networkIsTimeValid();
    time["valid"] = timeValid;
    if (timeValid) time["iso"] = networkGetTimeIso(); else time["iso"] = nullptr;
    time["src"] = networkGetTimeSource();

    JsonObject eq = out.createNestedObject("eq");
    equithermFillFastJson(eq);

    JsonObject dhw = out.createNestedObject("dhw");
    dhwFillFastJson(dhw);

    JsonObject heap = out.createNestedObject("heap");
    fillHeapJson(heap);

    JsonArray adminActions = out.createNestedArray("adminActions");
    fillAdminActionsJson(adminActions);
  }

  static String buildFastStateJson() {
    DynamicJsonDocument doc(4096);
    fillFastStateObject(doc.to<JsonObject>());
    return jsonResponse(doc);
  }

  static void handleWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    (void)payload;
    (void)length;
    if (type == WStype_CONNECTED) {
      if (g_wsClientCount < 255) ++g_wsClientCount;
      const String msg = buildFastWsFrame(true);
      if (msg.length()) g_ws.sendTXT(num, msg.c_str(), msg.length());
    } else if (type == WStype_DISCONNECTED) {
      if (g_wsClientCount > 0) --g_wsClientCount;
    }
  }

  static void handleFast() {
    DynamicJsonDocument doc(4096);
    fillFastStateObject(doc.to<JsonObject>());
    sendJsonDoc(200, doc);
  }

  static void handleBootstrap() {
    DynamicJsonDocument doc(24576);
    JsonObject fast = doc.createNestedObject("fast");
    fillFastStateObject(fast);

    const char* sections[] = {"time", "dallas", "equitherm", "opentherm", "dhw", "alerts"};
    for (size_t i = 0; i < sizeof(sections)/sizeof(sections[0]); ++i) {
      DynamicJsonDocument secDoc(6144);
      const bool ok = loadConfigSectionLiveOrSnapshot(sections[i], secDoc);
      if (ok) doc[sections[i]] = secDoc.as<JsonVariantConst>();
      else doc[sections[i]] = JsonVariant();
      yield();
    }

    sendJsonDoc(200, doc);
  }

  static bool rejectActionRateLimit(const char* actionKey, unsigned long minIntervalMs, uint16_t maxPerWindow, unsigned long windowMs, const char* detail = nullptr) {
    if (allowAction(actionKey, minIntervalMs, maxPerWindow, windowMs, detail)) return false;
    DynamicJsonDocument doc(256);
    doc["ok"] = false;
    doc["err"] = "rate_limited";
    doc["action"] = actionKey;
    doc["retryAfterMs"] = (uint32_t)minIntervalMs;
    if (detail && *detail) doc["detail"] = detail;
    sendJsonDoc(429, doc);
    return true;
  }

  static uint8_t relayMaskBitForIndex(uint8_t relayIndex) {
    if (relayIndex > 7) return 0;
    return (uint8_t)(1u << relayIndex);
  }

  static bool relayMaskHasMixingConflict(uint8_t mask) {
    return (mask & (uint8_t)(kRelayMixOpenBit | kRelayMixCloseBit)) == (uint8_t)(kRelayMixOpenBit | kRelayMixCloseBit);
  }

  static uint8_t getControllerManagedRelayMask() {
    const DhwConfig dc = dhwGetConfig();
    uint8_t mask = 0;
    if (dc.heat.driveValveRelay) mask |= relayMaskBitForIndex(dc.heat.valveRelayIndex);
    if (dc.heat.relayRequest) mask |= relayMaskBitForIndex(dc.heat.boilerRelayIndex);
    mask |= relayMaskBitForIndex(dc.circ.relayIndex);
    return mask;
  }

  static void handleRelayPost() {
    if (rejectActionRateLimit("relay", 150UL, 20, 10000UL, "relay_guard")) return;
    const String body = g_srv.arg("plain");
    DynamicJsonDocument docIn(512);
    if (deserializeJson(docIn, body)) {
      DynamicJsonDocument err(256);
      err["ok"] = false; err["err"] = "bad_json";
      sendJsonDoc(400, err);
      return;
    }

    const uint8_t managedMask = getControllerManagedRelayMask();
    bool managedSkipped = false;
    bool heaterSkipped = false;

    JsonObjectConst o = docIn.as<JsonObjectConst>();
    if (o.containsKey("mask")) {
      const uint8_t requestedMask = (uint8_t)(o["mask"] | 0);
      if (relayMaskHasMixingConflict(requestedMask)) {
        DynamicJsonDocument err(384);
        err["ok"] = false;
        err["err"] = "mixing_relays_mutually_exclusive";
        err["detail"] = "R1 and R2 must not be ON at the same time";
        sendJsonDoc(409, err);
        return;
      }

      // /api/relay must not override relays managed by higher-level controllers
      // (DHW valve / DHW request / DHW circulation). Keep current managed bits as-is.
      const uint8_t currentMask = relayGetMask();
      uint8_t safeMask = (uint8_t)((requestedMask & ~managedMask) | (currentMask & managedMask));
      if (safeMask & kRelayAccuHeaterBit) {
        safeMask &= (uint8_t)~kRelayAccuHeaterBit;
        heaterSkipped = true;
      }
      managedSkipped = ((requestedMask ^ safeMask) & managedMask) != 0;
      relaySetMask(safeMask);
    } else if (o.containsKey("id")) {
      int id = (int)(o["id"] | 0);
      if (id < 1 || id > 8) {
        DynamicJsonDocument err(256);
        err["ok"] = false; err["err"] = "bad_id";
        sendJsonDoc(400, err);
        return;
      }
      const uint8_t relayIndex = (uint8_t)(id - 1);
      const uint8_t relayBit = relayMaskBitForIndex(relayIndex);
      if (managedMask & relayBit) {
        DynamicJsonDocument err(384);
        err["ok"] = false;
        err["err"] = "relay_managed_by_controller";
        err["managedMask"] = managedMask;
        sendJsonDoc(409, err);
        return;
      }
      RelayId rid = (RelayId)relayIndex;
      const bool toggle = (bool)(o["toggle"] | false);
      const bool wantOn = (toggle || !o.containsKey("on")) ? !relayGetState(rid) : (bool)o["on"];
      if (relayIndex == kRelayAccuHeaterIdx && wantOn) {
        DynamicJsonDocument err(384);
        err["ok"] = false;
        err["err"] = "relay_requires_service_command";
        err["detail"] = "R8 heater cannot be enabled through generic relay API";
        sendJsonDoc(409, err);
        return;
      }
      if (toggle || !o.containsKey("on")) {
        relayToggle(rid);
      } else {
        relaySet(rid, wantOn);
      }
    }

    // Return quick snapshot
    DynamicJsonDocument doc(1024);
    doc["ok"] = true;
    doc["managedMask"] = managedMask;
    if (managedSkipped) doc["warn"] = "managed_relays_ignored";
    if (heaterSkipped) doc["heaterWarn"] = "heater_relay_ignored";
    JsonObject rel = doc.createNestedObject("rel");
    rel["mask"] = relayGetMask();

    JsonObject fast = doc.createNestedObject("fast");
    fast["ip"] = getBestIpString();
    fast["wifi"] = networkIsWifiConnected();
    fast["eth"] = networkIsEthernetConnected();
    JsonObject rel2 = fast.createNestedObject("rel");
    rel2["mask"] = relayGetMask();
    sendJsonDoc(200, doc);
    recordAdminAction("relay", true, managedSkipped ? "managed_relays_ignored" : (heaterSkipped ? "heater_relay_ignored" : "applied"));
  }

  static void handleSystemCmd() {
    if (rejectActionRateLimit("system_cmd", 500UL, 8, 10000UL, "system_guard")) return;
    const String body = g_srv.arg("plain");
    DynamicJsonDocument docIn(256);
    if (deserializeJson(docIn, body) || !docIn.is<JsonObject>()) {
      DynamicJsonDocument err(256);
      err["ok"] = false; err["err"] = "bad_json";
      sendJsonDoc(400, err);
      return;
    }

    JsonObjectConst o = docIn.as<JsonObjectConst>();
    const char* command = o["command"] | "";
    String cmd(command);
    cmd.toLowerCase();
    if (cmd != "safestop" && cmd != "safe_stop") {
      DynamicJsonDocument err(256);
      err["ok"] = false;
      err["err"] = "bad_command";
      sendJsonDoc(400, err);
      return;
    }

    String dhwErr;
    const bool dhwOk = dhwHandleCmdJson("{\"command\":\"safeStop\"}", dhwErr);
    relaySet((RelayId)kRelayMixOpenIdx, false);
    relaySet((RelayId)kRelayMixCloseIdx, false);
    relaySet((RelayId)kRelayDhwValveIdx, false);
    relaySet((RelayId)kRelayDhwCircIdx, false);
    relaySet((RelayId)kRelayDhwBoilerIdx, false);
    relaySet((RelayId)kRelayAccuHeaterIdx, false);

    DynamicJsonDocument out(512);
    out["ok"] = dhwOk;
    out["state"] = dhwOk ? "safe" : "partial";
    if (!dhwOk) out["err"] = dhwErr;
    JsonArray actions = out.createNestedArray("actions");
    actions.add("mixing_stop");
    actions.add("dhw_valve_ch");
    actions.add("dhw_request_off");
    actions.add("circulation_off");
    actions.add("heater_off");
    JsonObject rel = out.createNestedObject("rel");
    rel["mask"] = relayGetMask();
    sendJsonDoc(dhwOk ? 200 : 500, out);
    recordAdminAction("system_cmd", dhwOk, dhwOk ? "safeStop" : dhwErr.c_str());
  }


  static String normalizedTopic(const String& src) {
    String out = src;
    out.trim();
    while (out.startsWith("/")) out.remove(0, 1);
    while (out.endsWith("/")) out.remove(out.length() - 1);
    return out;
  }

  static void fillMqttPreview(JsonObject mqtt) {
    const String baseTopic = normalizedTopic(ConfigStore::getMqttBaseTopic());
    const String discoveryPrefix = normalizedTopic(ConfigStore::getMqttDiscoveryPrefix());
    const String nodeId = normalizedTopic(ConfigStore::getMqttNodeId());
    const String stateTopic = baseTopic + "/state";
    const String availabilityTopic = baseTopic + "/availability";

    mqtt["enabled"] = ConfigStore::getMqttEnabled();
    mqtt["connected"] = mqttIsConnected();
    mqtt["implemented"] = true;
    mqtt["runtime"] = mqttIsConnected() ? "connected" : (ConfigStore::getMqttEnabled() ? "enabled" : "disabled");
    mqtt["host"] = ConfigStore::getMqttHost();
    mqtt["port"] = (uint32_t)ConfigStore::getMqttPort();
    mqtt["username"] = ConfigStore::getMqttUsername();
    mqtt["passwordSet"] = ConfigStore::getMqttPassword().length() > 0;
    mqtt["clientId"] = ConfigStore::getMqttClientId();
    mqtt["baseTopic"] = baseTopic;
    mqtt["publishIntervalMs"] = (uint32_t)ConfigStore::getMqttPublishIntervalMs();
    mqtt["stateTopic"] = stateTopic;
    mqtt["availabilityTopic"] = availabilityTopic;

    JsonObject ha = mqtt.createNestedObject("homeAssistant");
    ha["enabled"] = ConfigStore::getMqttHaEnabled();
    ha["discovery"] = ConfigStore::getMqttHaDiscovery();
    ha["discoveryPrefix"] = discoveryPrefix;
    ha["nodeId"] = nodeId;

    JsonObject preview = mqtt.createNestedObject("preview");
    preview["stateTopic"] = stateTopic;
    preview["availabilityTopic"] = availabilityTopic;
    preview["discoveryPrefix"] = discoveryPrefix;

    JsonObject device = preview.createNestedObject("device");
    JsonArray identifiers = device.createNestedArray("identifiers");
    identifiers.add(nodeId);
    device["name"] = "ESP32 Controller";
    device["manufacturer"] = "Waveshare";
    device["model"] = "ESP32-S3-ETH-8DI-8RO";
    device["ip"] = getBestIpString();

    JsonArray entities = preview.createNestedArray("entities");
    struct EntDef {
      const char* component;
      const char* objectId;
      const char* name;
      const char* valueTemplate;
      const char* unit;
      const char* deviceClass;
      const char* stateClass;
    };
    static const EntDef defs[] = {
      {"sensor", "outside_temp", "Venkovni teplota", "{{ value_json.temps.outside }}", "°C", "temperature", "measurement"},
      {"sensor", "flow_temp", "Topna voda", "{{ value_json.temps.flow }}", "°C", "temperature", "measurement"},
      {"sensor", "return_temp", "Zpatecka", "{{ value_json.temps.return }}", "°C", "temperature", "measurement"},
      {"sensor", "dhw_temp", "TUV", "{{ value_json.temps.dhw_tank }}", "°C", "temperature", "measurement"},
      {"sensor", "mix_position", "Smesovaci ventil", "{{ value_json.eq.mix.pct }}", "%", nullptr, "measurement"},
      {"binary_sensor", "input1_night", "Vstup Den Noc", "{{ value_json.in.actMask | int(0) & 1 > 0 }}", nullptr, nullptr, nullptr},
    };

    for (const auto &def : defs) {
      JsonObject ent = entities.createNestedObject();
      ent["component"] = def.component;
      ent["objectId"] = def.objectId;
      ent["name"] = def.name;
      ent["stateTopic"] = stateTopic;
      ent["availabilityTopic"] = availabilityTopic;
      ent["valueTemplate"] = def.valueTemplate;
      if (def.unit) ent["unit"] = def.unit;
      if (def.deviceClass) ent["deviceClass"] = def.deviceClass;
      if (def.stateClass) ent["stateClass"] = def.stateClass;
      ent["discoveryTopic"] = discoveryPrefix + "/" + String(def.component) + "/" + nodeId + "/" + String(def.objectId) + "/config";
    }
  }

  static void handleMqttStatus() {
    sendJson(200, mqttGetStatusJson());
  }

  static void fillInputsConfigJson(JsonObject out) {
    JsonArray lvl = out.createNestedArray("activeLevel");
    for (uint8_t i = 0; i < INPUT_COUNT; i++) lvl.add((int)ConfigStore::getInputActiveLevel(i));
  }


  static void fillInputsSectionJson(JsonObject out) {
    fillInputsConfigJson(out);
  }

  static void fillOpenThermSectionJson(JsonObject out) {
    OpenThermConfig oc = openthermGetConfig();
    out["enabled"] = oc.enabled;
    out["autoStart"] = oc.autoStart;
    out["pollMs"] = (uint32_t)oc.pollMs;
    out["bootDelayMs"] = (uint32_t)oc.bootDelayMs;
    out["mode"] = oc.mode;
    out["boilerControl"] = oc.boilerControl;
    out["allowRawWrite"] = oc.allowRawWrite;
  }

  static void fillBleSectionJson(JsonObject out) {
    BleConfig bc = bleGetConfig();
    out["enabled"] = bc.enabled;
    out["namePrefix"] = bc.namePrefix;
    out["scanIntervalMs"] = (uint32_t)bc.scanIntervalMs;
  }

  static void fillDallasSectionJson(JsonObject out) {
    out["enabled"] = ConfigStore::getDallasEnabled();
    JsonObject roles = out.createNestedObject("roles");
    size_t roleCount = 0;
    const auto* bindings = TemperatureManager::getDallasRoleBindings(roleCount);
    JsonArray availableRoles = out.createNestedArray("availableRoles");
    for (size_t i = 0; i < roleCount; i++) {
      const auto &binding = bindings[i];
      if (!binding.assignable) continue;
      roles[binding.key] = TemperatureManager::romToHex(TemperatureManager::getRoleRom(binding.role));
      JsonObject info = availableRoles.createNestedObject();
      info["key"] = binding.key;
      info["label"] = binding.label;
      info["gpio"] = (int)binding.gpio;
      yield();
    }
  }

  static void fillOtaSectionJson(JsonObject out) {
    OtaConfig ocfg = otaGetConfig();
    out["enabled"] = ocfg.enabled;
    out["hostname"] = ocfg.hostname;
    out["port"] = (uint32_t)ocfg.port;
    out["passwordSet"] = ocfg.passwordSet;
  }

  static void fillMqttSectionJson(JsonObject out) {
    fillMqttPreview(out);
  }

  static void fillTimeSectionJson(JsonObject out) {
    out["enabled"] = ConfigStore::getTimeEnabled();
    out["tz"] = ConfigStore::getTimeTz();
    JsonArray ntp = out.createNestedArray("ntp");
    ntp.add(ConfigStore::getTimeNtp1());
    ntp.add(ConfigStore::getTimeNtp2());
    ntp.add(ConfigStore::getTimeNtp3());
    out["valid"] = networkIsTimeValid();
    if (networkIsTimeValid()) out["iso"] = networkGetTimeIso(); else out["iso"] = nullptr;
    out["src"] = networkGetTimeSource();
  }

  static void fillEquithermSectionJson(JsonObject eq) {
    EquithermConfig ec = equithermGetConfig();
    eq["enabled"] = ec.enabled;
    eq["mode"] = ec.mode;
    eq["useIn1NightOverride"] = ec.useIn1NightOverride;
    eq["summerModeEnabled"] = ec.summerModeEnabled;
    eq["summerOffAboveC"] = ec.summerOffAboveC;
    eq["summerOnBelowC"] = ec.summerOnBelowC;

    JsonObject sched = eq.createNestedObject("schedule");
    sched["enabled"] = ec.scheduleEnabled;
    JsonArray week = sched.createNestedArray("week");
    static const char* dayNames[7] = {"mon","tue","wed","thu","fri","sat","sun"};
    for (int i = 0; i < 7; i++) {
      JsonObject dday = week.createNestedObject();
      dday["day"] = dayNames[i];
      const uint8_t count = ec.intervalCount[i];
      dday["intervalCount"] = count;
      JsonArray intervals = dday.createNestedArray("intervals");
      for (uint8_t k = 0; k < count && k < HEATING_MAX_INTERVALS_PER_DAY; k++) {
        JsonObject iv = intervals.createNestedObject();
        iv["startMin"] = ec.intervals[i][k].startMin;
        iv["endMin"] = ec.intervals[i][k].endMin;
      }
      if (count > 0) {
        dday["dayStartMin"] = ec.intervals[i][0].startMin;
        dday["nightStartMin"] = ec.intervals[i][0].endMin;
      } else {
        dday["dayStartMin"] = 360;
        dday["nightStartMin"] = 1320;
      }
      yield();
    }

    JsonObject day = eq.createNestedObject("day");
    day["outColdC"] = ec.day.outColdC;
    day["flowColdC"] = ec.day.flowColdC;
    day["outWarmC"] = ec.day.outWarmC;
    day["flowWarmC"] = ec.day.flowWarmC;

    JsonObject night = eq.createNestedObject("night");
    night["outColdC"] = ec.night.outColdC;
    night["flowColdC"] = ec.night.flowColdC;
    night["outWarmC"] = ec.night.outWarmC;
    night["flowWarmC"] = ec.night.flowWarmC;

    JsonObject lim = eq.createNestedObject("limits");
    lim["minFlowC"] = ec.minFlowC;
    lim["maxFlowC"] = ec.maxFlowC;
    lim["minChSetpointC"] = ec.minChSetpointC;
    lim["maxChSetpointC"] = ec.maxChSetpointC;

    JsonObject temps = eq.createNestedObject("temps");
    temps["maxAgeMs"] = ec.tempMaxAgeMs;

    JsonObject send = eq.createNestedObject("send");
    send["minIntervalMs"] = ec.minSendIntervalMs;
    send["minDeltaC"] = ec.minSendDeltaC;

    JsonObject outm = eq.createNestedObject("output");
    outm["useOpenTherm"] = ec.useOpenTherm;
    outm["applyBoilerMaxCh"] = ec.applyBoilerMaxCh;
    outm["boilerMaxChC"] = ec.boilerMaxChC;
    outm["driveNightRelay"] = ec.driveNightRelay;
    outm["nightRelay"] = (uint32_t)(ec.nightRelayIndex + 1);
    outm["nightRelayOnWhenNight"] = ec.nightRelayOnWhenNight;

    JsonObject mix = eq.createNestedObject("mixing");
    mix["enabled"] = ec.mixingEnabled;
    mix["openRelay"] = 1;
    mix["closeRelay"] = 2;
    mix["deadbandC"] = ec.mixDeadbandC;
    mix["targetOffsetC"] = ec.mixTargetOffsetC;
    mix["pulseMs"] = (uint32_t)ec.mixPulseMs;
    mix["minIntervalMs"] = (uint32_t)ec.mixMinIntervalMs;
    mix["travelMs"] = (uint32_t)ec.mixTravelMs;

    JsonObject ba = eq.createNestedObject("boilerAssist");
    ba["enabled"] = ec.boilerAssistEnabled;
    ba["deltaC"] = ec.boilerAssistDeltaC;
    ba["forceChEnable"] = ec.boilerAssistForceChEnable;
  }

  static void fillDhwSectionJson(JsonObject dhw) {
    DhwConfig dc = dhwGetConfig();
    dhw["enabled"] = dc.enabled;
    dhw["disableEquithermDuringHeat"] = dc.disableEquithermDuringHeat;
    dhw["tempMaxAgeMs"] = dc.tempMaxAgeMs;
    JsonObject dhwHeat = dhw.createNestedObject("heat");
    dhwHeat["useInput"] = dc.heat.useInput;
    dhwHeat["useSchedule"] = dc.heat.useSchedule;
    dhwHeat["scheduleEnabled"] = dc.heat.scheduleEnabled;
    dhwHeat["targetTempC"] = dc.heat.targetTempC;
    dhwHeat["hysteresisC"] = dc.heat.hysteresisC;
    dhwHeat["requestMode"] = dc.heat.requestMode;
    dhwHeat["otEnableDhw"] = dc.heat.otEnableDhw;
    dhwHeat["otDhwSetpointC"] = dc.heat.otDhwSetpointC;
    dhwHeat["relayRequest"] = dc.heat.relayRequest;
    dhwHeat["driveValveRelay"] = dc.heat.driveValveRelay;
    dhwHeat["valveRelay"] = (uint32_t)(dc.heat.valveRelayIndex + 1);
    dhwHeat["boilerRelay"] = (uint32_t)(dc.heat.boilerRelayIndex + 1);
    dhwHeat["valveLeadMs"] = (uint32_t)dc.heat.valveLeadMs;
    dhwHeat["valveSwitchBackMs"] = (uint32_t)dc.heat.valveSwitchBackMs;
    dhwHeat["boilerOffHoldMs"] = (uint32_t)dc.heat.boilerOffHoldMs;
    JsonObject dhwHeatSched = dhwHeat.createNestedObject("schedule");
    JsonArray dhwHeatWeek = dhwHeatSched.createNestedArray("week");
    static const char* dayNames2[7] = {"mon","tue","wed","thu","fri","sat","sun"};
    for (int i = 0; i < 7; i++) {
      JsonObject d = dhwHeatWeek.createNestedObject();
      d["day"] = dayNames2[i];
      JsonArray ints = d.createNestedArray("intervals");
      for (uint8_t k = 0; k < dc.heat.week[i].count && k < DHW_MAX_INTERVALS_PER_DAY; k++) {
        const auto &it = dc.heat.week[i].items[k];
        if (!it.valid) continue;
        JsonObject iv = ints.createNestedObject();
        iv["startMin"] = it.startMin;
        iv["endMin"] = it.endMin;
      }
      yield();
    }
    JsonObject al = dhw.createNestedObject("antiLegionella");
    al["enabled"] = dc.antiLegionella.enabled;
    al["weekday"] = dc.antiLegionella.weekday;
    al["startMin"] = dc.antiLegionella.startMin;
    al["targetTempC"] = dc.antiLegionella.targetTempC;
    al["holdMin"] = dc.antiLegionella.holdMin;

    JsonObject dhwCirc = dhw.createNestedObject("circ");
    dhwCirc["useInput"] = dc.circ.useInput;
    dhwCirc["useSchedule"] = dc.circ.useSchedule;
    dhwCirc["scheduleEnabled"] = dc.circ.scheduleEnabled;
    dhwCirc["pulseEnabled"] = dc.circ.pulseEnabled;
    dhwCirc["pulseOnMin"] = dc.circ.pulseOnMin;
    dhwCirc["pulseOffMin"] = dc.circ.pulseOffMin;
    dhwCirc["relay"] = (uint32_t)(dc.circ.relayIndex + 1);
    JsonObject dhwCircSched = dhwCirc.createNestedObject("schedule");
    JsonArray dhwCircWeek = dhwCircSched.createNestedArray("week");
    for (int i = 0; i < 7; i++) {
      JsonObject d = dhwCircWeek.createNestedObject();
      d["day"] = dayNames2[i];
      JsonArray ints = d.createNestedArray("intervals");
      for (uint8_t k = 0; k < dc.circ.week[i].count && k < DHW_MAX_INTERVALS_PER_DAY; k++) {
        const auto &it = dc.circ.week[i].items[k];
        if (!it.valid) continue;
        JsonObject iv = ints.createNestedObject();
        iv["startMin"] = it.startMin;
        iv["endMin"] = it.endMin;
      }
      yield();
    }
  }

  static void fillAlertsSectionJson(JsonObject alerts) {
    JsonObject pressure = alerts.createNestedObject("pressure");
    pressure["enabled"] = ConfigStore::getPressureAlarmEnabled();
    pressure["minBar"] = ConfigStore::getPressureAlarmMinBar();
    pressure["maxBar"] = ConfigStore::getPressureAlarmMaxBar();
    pressure["hysteresisBar"] = ConfigStore::getPressureAlarmHysteresisBar();
    PressureAlarmStatus ps = pressureAlarmGetStatus();
    pressure["active"] = ps.active;
    pressure["sensorValid"] = ps.sensorValid;
    if (isfinite(ps.pressureBar)) pressure["pressureBar"] = ps.pressureBar; else pressure["pressureBar"] = nullptr;
    pressure["state"] = ps.state.length() ? ps.state : "init";
  }

  static bool fillConfigSectionObject(const String& section, JsonObject out) {
    if (section == "inputs") { fillInputsSectionJson(out); return true; }
    if (section == "opentherm") { fillOpenThermSectionJson(out); return true; }
    if (section == "ble") { fillBleSectionJson(out); return true; }
    if (section == "dallas") { fillDallasSectionJson(out); return true; }
    if (section == "ota") { fillOtaSectionJson(out); return true; }
    if (section == "mqtt") { fillMqttSectionJson(out); return true; }
    if (section == "time") { fillTimeSectionJson(out); return true; }
    if (section == "equitherm") { fillEquithermSectionJson(out); return true; }
    if (section == "dhw") { fillDhwSectionJson(out); return true; }
    if (section == "alerts") { fillAlertsSectionJson(out); return true; }
    return false;
  }

  static void fillConfigDoc(DynamicJsonDocument& doc) {
    doc["ok"] = true;
    for (const auto& def : kConfigSections) {
      JsonObject out = doc.createNestedObject(def.name);
      fillConfigSectionObject(String(def.name), out);
      yield();
    }
  }

  static bool saveConfigSnapshot() {
    if (!g_fsMounted) return false;
    ensureConfigDir();
    DynamicJsonDocument doc(14336);
    fillConfigDoc(doc);
    bool ok = writeJsonVariantToFile("/config.json", doc.as<JsonVariantConst>());
    for (const auto& def : kConfigSections) {
      const String path = String("/config/") + def.name + ".json";
      ok = writeJsonVariantToFile(path.c_str(), doc[def.name].as<JsonVariantConst>()) && ok;
      yield();
    }
    return ok;
  }


  static bool fillConfigSectionDoc(const String& section, DynamicJsonDocument& doc) {
    doc.clear();
    JsonObject out = doc.to<JsonObject>();
    return fillConfigSectionObject(section, out);
  }

  static bool loadConfigSectionFromSnapshot(const char* section, DynamicJsonDocument& doc) {
    if (!g_fsMounted || !section || !*section) return false;
    const String path = String("/config/") + section + ".json";
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    return !err && !doc.isNull();
  }

  static bool loadConfigSectionLiveOrSnapshot(const char* section, DynamicJsonDocument& doc) {
    if (!section || !*section) return false;
    if (fillConfigSectionDoc(String(section), doc)) return true;
    return loadConfigSectionFromSnapshot(section, doc);
  }

  static void handleConfigSectionGet(const char* section) {
    const ConfigSectionDef* def = findConfigSection(section);
    if (!def) {
      DynamicJsonDocument err(128);
      err["ok"] = false;
      err["err"] = "unknown_section";
      sendJsonDoc(404, err);
      return;
    }
    DynamicJsonDocument doc(def->postDocCap > 4096 ? def->postDocCap : 4096);
    const bool ok = loadConfigSectionLiveOrSnapshot(def->name, doc);
    if (!ok) {
      DynamicJsonDocument err(128);
      err["ok"] = false;
      err["err"] = "unknown_section";
      sendJsonDoc(404, err);
      return;
    }
    sendJsonDoc(200, doc);
  }

  static void handleInputsConfigGet() { handleConfigSectionGet("inputs"); }
  static void handleEquithermConfigGet() { handleConfigSectionGet("equitherm"); }
  static void handleOpenThermConfigGet() { handleConfigSectionGet("opentherm"); }
  static void handleBleConfigGet() { handleConfigSectionGet("ble"); }
  static void handleOtaConfigGet() { handleConfigSectionGet("ota"); }
  static void handleDhwConfigGet() { handleConfigSectionGet("dhw"); }
  static void handleDallasConfigGet() { handleConfigSectionGet("dallas"); }
  static void handleAlertsConfigGet() { handleConfigSectionGet("alerts"); }
  static void handleMqttConfigGet() { handleConfigSectionGet("mqtt"); }
  static void handleTimeConfigGet() { handleConfigSectionGet("time"); }

  static void applySectionByName(const String& section, JsonObjectConst root) {
    if (section == "inputs") applyInputsSection(root);
    else if (section == "equitherm") applyEquithermSection(root);
    else if (section == "opentherm") applyOpenThermSection(root);
    else if (section == "ble") applyBleSection(root);
    else if (section == "ota") applyOtaSection(root);
    else if (section == "dhw") applyDhwSection(root);
    else if (section == "dallas") applyDallasSection(root);
    else if (section == "alerts") { applyAlertsSection(root); pressureAlarmReloadFromStore(); }
    else if (section == "mqtt") { applyMqttSection(root); mqttApplyConfig(String()); }
    else if (section == "time") applyTimeSection(root);
  }

  static size_t applyImportedRootConfig(JsonObjectConst root) {
    size_t applied = 0;
    auto applyIfPresent = [&](const char* key) {
      if (!root.containsKey(key) || !root[key].is<JsonObjectConst>()) return;
      applySectionByName(String(key), root[key].as<JsonObjectConst>());
      applied++;
    };

    for (const auto& def : kConfigSections) {
      applyIfPresent(def.name);
      yield();
    }
    return applied;
  }

  static bool loadJsonFileToDoc(const char* path, DynamicJsonDocument& doc) {
    if (!g_fsMounted || !path || !*path || !LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    doc.clear();
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    return !err && !doc.isNull();
  }

  static size_t bootImportConfigFromLittleFS() {
    if (!g_fsMounted) return 0;

    size_t applied = 0;
    bool importedAny = false;

    DynamicJsonDocument rootDoc(16384);
    if (loadJsonFileToDoc("/config.json", rootDoc)) {
      JsonObjectConst root = rootDoc.as<JsonObjectConst>();
      if (!root.isNull()) {
        const size_t rootApplied = applyImportedRootConfig(root);
        if (rootApplied > 0) {
          applied += rootApplied;
          importedAny = true;
          Serial.printf("[WEB] Boot import: /config.json applied (%u sections)\n", (unsigned)rootApplied);
        }
      }
    }

    for (const auto& def : kConfigSections) {
      const String path = String("/config/") + def.name + ".json";
      DynamicJsonDocument sectionDoc(def.postDocCap > 4096 ? def.postDocCap : 4096);
      if (!loadJsonFileToDoc(path.c_str(), sectionDoc)) continue;
      JsonObjectConst obj = sectionDoc.as<JsonObjectConst>();
      if (obj.isNull()) continue;
      applySectionByName(String(def.name), obj);
      applied++;
      importedAny = true;
      yield();
    }

    if (importedAny) {
      const bool snapshotOk = saveConfigSnapshot();
      Serial.printf("[WEB] Boot import from LittleFS complete (%u applied, snapshot=%s)\n",
                    (unsigned)applied, snapshotOk ? "ok" : "failed");
    }
    return applied;
  }

  static void handleSectionPost(const char* section) {
    const ConfigSectionDef* def = findConfigSection(section);
    if (!def) { writeUploadJson(404, false, "unknown_section"); return; }
    if (rejectActionRateLimit(def->rateKey, 1000UL, 8, 60000UL, "config_guard")) return;
    DynamicJsonDocument docIn(def->postDocCap);
    if (deserializeJson(docIn, g_srv.arg("plain"))) { writeUploadJson(400, false, "bad_json"); return; }
    JsonObject root = docIn.as<JsonObject>();
    if (root.isNull()) { writeUploadJson(400, false, "bad_body"); return; }
    applySectionByName(String(def->name), root);
    const bool snapshotOk = saveConfigSnapshot();
    DynamicJsonDocument doc(192);
    doc["ok"] = true;
    doc["snapshotSaved"] = snapshotOk;
    if (!snapshotOk) doc["warn"] = "snapshot_failed";
    sendJsonDoc(200, doc);
    recordAdminAction(def->rateKey, true, snapshotOk ? "saved" : "saved_no_snapshot");
  }

  static void handleConfigExport() {
    if (rejectActionRateLimit("cfg_export", 1000UL, 6, 60000UL, "config_export_guard")) return;
    const bool ok = saveConfigSnapshot();
    DynamicJsonDocument doc(256);
    doc["ok"] = ok;
    doc["msg"] = ok ? "snapshot_saved" : "snapshot_failed";
    sendJsonDoc(ok ? 200 : 500, doc);
    recordAdminAction("cfg_export", ok, ok ? "snapshot_saved" : "snapshot_failed");
  }

  static void handleConfigApply() {
    if (rejectActionRateLimit("cfg_apply", 1000UL, 8, 60000UL, "config_apply_guard")) return;
    DynamicJsonDocument docIn(16384);
    if (deserializeJson(docIn, g_srv.arg("plain"))) { writeUploadJson(400, false, "bad_json"); return; }
    JsonObject root = docIn.as<JsonObject>();
    if (root.isNull()) { writeUploadJson(400, false, "bad_body"); return; }
    const size_t applied = applyImportedRootConfig(root);
    if (!applied) { writeUploadJson(400, false, "no_known_sections"); return; }
    const bool snapshotOk = saveConfigSnapshot();
    DynamicJsonDocument doc(256);
    doc["ok"] = true;
    doc["appliedSections"] = (uint32_t)applied;
    doc["snapshotSaved"] = snapshotOk;
    if (!snapshotOk) doc["warn"] = "snapshot_failed";
    sendJsonDoc(200, doc);
    recordAdminAction("cfg_apply", true, snapshotOk ? "applied" : "applied_no_snapshot");
  }

  static void handleConfigImport() {
    if (rejectActionRateLimit("cfg_import", 1500UL, 4, 60000UL, "config_import_guard")) return;
    DynamicJsonDocument docIn(16384);
    if (deserializeJson(docIn, g_srv.arg("plain"))) { writeUploadJson(400, false, "bad_json"); return; }
    JsonObject root = docIn.as<JsonObject>();
    if (root.isNull()) { writeUploadJson(400, false, "bad_body"); return; }
    const size_t applied = applyImportedRootConfig(root);
    if (!applied) { writeUploadJson(400, false, "no_known_sections"); return; }
    const bool snapshotOk = saveConfigSnapshot();
    DynamicJsonDocument doc(256);
    doc["ok"] = true;
    doc["importedSections"] = (uint32_t)applied;
    doc["snapshotSaved"] = snapshotOk;
    if (!snapshotOk) doc["warn"] = "snapshot_failed";
    sendJsonDoc(200, doc);
    recordAdminAction("cfg_import", true, snapshotOk ? "imported" : "imported_no_snapshot");
  }

  static void handleInputsConfigPost() { handleSectionPost("inputs"); }
  static void handleEquithermConfigPost() { handleSectionPost("equitherm"); }
  static void handleOpenThermConfigPost() { handleSectionPost("opentherm"); }
  static void handleBleConfigPost() { handleSectionPost("ble"); }
  static void handleOtaConfigPost() { handleSectionPost("ota"); }
  static void handleDhwConfigPost() { handleSectionPost("dhw"); }
  static void handleDallasConfigPost() { handleSectionPost("dallas"); }
  static void handleAlertsConfigPost() { handleSectionPost("alerts"); }
  static void handleMqttConfigPost() { handleSectionPost("mqtt"); }
  static void handleTimeConfigPost() { handleSectionPost("time"); }

  static void handleConfigGet() {
    DynamicJsonDocument doc(14336);
    fillConfigDoc(doc);
    sendJsonDoc(200, doc);
  }

  static bool ensureConfigDir() {
    if (!g_fsMounted) return false;
    if (LittleFS.exists("/config")) return true;
    return LittleFS.mkdir("/config");
  }

  static bool writeJsonVariantToFile(const char* path, JsonVariantConst value) {
    if (!g_fsMounted || !path || !*path) return false;
    String finalPath(path);
    String tempPath = finalPath + ".tmp";

    if (LittleFS.exists(tempPath)) {
      LittleFS.remove(tempPath);
    }

    File f = LittleFS.open(tempPath, "w");
    if (!f) return false;
    const size_t written = serializeJsonPretty(value, f);
    f.flush();
    f.close();
    if (written == 0) {
      LittleFS.remove(tempPath);
      return false;
    }

    if (LittleFS.exists(finalPath)) {
      if (!LittleFS.remove(finalPath)) {
        LittleFS.remove(tempPath);
        return false;
      }
    }
    if (!LittleFS.rename(tempPath, finalPath)) {
      LittleFS.remove(tempPath);
      return false;
    }
    return true;
  }

  static void applyInputsSection(JsonObjectConst in) {
    if (in.isNull()) return;
    if (!in.containsKey("activeLevel") || !in["activeLevel"].is<JsonArrayConst>()) return;
    JsonArrayConst a = in["activeLevel"].as<JsonArrayConst>();
    uint8_t levels[8] = {0,0,0,0,0,0,0,0};
    uint8_t n = 0;
    for (JsonVariantConst v : a) {
      if (n >= 8) break;
      levels[n++] = (uint8_t)((int)v != 0);
    }
    ConfigStore::setInputActiveLevels(levels, 8);
  }


  static void applyOpenThermSection(JsonObjectConst o) {
    if (o.isNull()) return;
    ConfigStore::BatchGuard storeBatch;
    DynamicJsonDocument wrap(2048);
    wrap["opentherm"] = o;
    String js;
    serializeJson(wrap, js);
    openthermApplyConfig(js);

    if (o.containsKey("enabled")) ConfigStore::setOtEnabled((bool)o["enabled"]);
    if (o.containsKey("autoStart")) ConfigStore::setOtAutoStart((bool)o["autoStart"]);
    if (o.containsKey("pollMs")) ConfigStore::setOtPollMs((uint32_t)(o["pollMs"] | 2000));
    if (o.containsKey("bootDelayMs")) ConfigStore::setOtBootDelayMs((uint32_t)(o["bootDelayMs"] | 15000));
    if (o.containsKey("mode")) ConfigStore::setOtMode(String((const char*)o["mode"]));
    if (o.containsKey("allowRawWrite")) ConfigStore::setOtAllowRawWrite((bool)(o["allowRawWrite"] | false));
  }

  static void applyBleSection(JsonObjectConst b) {
    if (b.isNull()) return;
    ConfigStore::BatchGuard storeBatch;
    String js;
    serializeJson(b, js);
    bleApplyConfig(js);

    if (b.containsKey("enabled")) ConfigStore::setBleEnabled((bool)b["enabled"]);
    if (b.containsKey("namePrefix")) ConfigStore::setBleNamePrefix(String((const char*)(b["namePrefix"] | "")));
    if (b.containsKey("scanIntervalMs")) ConfigStore::setBleScanIntervalMs((uint32_t)(b["scanIntervalMs"] | 10000));
  }

  static void applyOtaSection(JsonObjectConst o) {
    if (o.isNull()) return;
    ConfigStore::BatchGuard storeBatch;
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
      ConfigStore::setOtaPassword(pw);
    }
    String js;
    serializeJson(o, js);
    otaApplyConfig(js);
  }

  static void applyTimeSection(JsonObjectConst t) {
    if (t.isNull()) return;
    ConfigStore::BatchGuard storeBatch;
    if (t.containsKey("enabled")) ConfigStore::setTimeEnabled((bool)(t["enabled"] | true));
    if (t.containsKey("tz")) {
      String tz = String((const char*)(t["tz"] | ""));
      tz.trim();
      ConfigStore::setTimeTz(tz);
    }
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
    }
    DynamicJsonDocument wrap(512);
    wrap["time"] = t;
    String js;
    serializeJson(wrap, js);
    networkApplyConfig(js);
  }

  static void applyDallasSection(JsonObjectConst d) {
    if (d.isNull()) return;
    bool changed = false;
    ConfigStore::BatchGuard storeBatch;
    if (d.containsKey("enabled")) {
      const bool en = (bool)d["enabled"];
      ConfigStore::setDallasEnabled(en);
      applyDallasEnabled(en);
      changed = true;
    }
    if (d.containsKey("roles") && d["roles"].is<JsonObjectConst>()) {
      JsonObjectConst rr = d["roles"].as<JsonObjectConst>();
      size_t roleCount = 0;
      const auto* bindings = TemperatureManager::getDallasRoleBindings(roleCount);
      for (size_t i = 0; i < roleCount; i++) {
        const auto &binding = bindings[i];
        if (!binding.assignable || !rr.containsKey(binding.key)) continue;

        String ss;
        JsonVariantConst rv = rr[binding.key];
        if (rv.is<JsonObjectConst>()) {
          JsonObjectConst ro = rv.as<JsonObjectConst>();
          const char* s = ro["rom"] | "";
          if (!s || !*s) s = ro["resolvedRom"] | "";
          ss = String(s ? s : "");
        } else {
          const char* s = rv | "";
          ss = String(s ? s : "");
        }
        ss.trim();
        if (!ss.length()) {
          TemperatureManager::setRoleRom(binding.role, 0);
          changed = true;
          continue;
        }
        uint64_t rom = 0;
        if (TemperatureManager::parseRomHex(ss, rom)) {
          TemperatureManager::setRoleRom(binding.role, rom);
          changed = true;
        }
      }
    }
    if (changed) {
      TemperatureManager::invalidateDallasBackedRoles();
      TemperatureManager::loop();
    }
  }

  static void applyMqttSection(JsonObjectConst m) {
    if (m.isNull()) return;
    ConfigStore::BatchGuard storeBatch;
    if (m.containsKey("enabled")) ConfigStore::setMqttEnabled((bool)(m["enabled"] | false));
    if (m.containsKey("host")) {
      String host = String((const char*)(m["host"] | ""));
      host.trim();
      ConfigStore::setMqttHost(host);
    }
    if (m.containsKey("port")) {
      uint32_t port = (uint32_t)(m["port"] | 1883);
      if (port == 0) port = 1883;
      if (port > 65535UL) port = 65535UL;
      ConfigStore::setMqttPort((uint16_t)port);
    }
    if (m.containsKey("username")) {
      String user = String((const char*)(m["username"] | ""));
      user.trim();
      ConfigStore::setMqttUsername(user);
    }
    const bool clearPassword = (bool)(m["clearPassword"] | false);
    if (clearPassword) {
      ConfigStore::setMqttPassword("");
    } else if (m.containsKey("password")) {
      String pw = String((const char*)(m["password"] | ""));
      if (pw.length()) ConfigStore::setMqttPassword(pw);
    }
    if (m.containsKey("clientId")) {
      String cid = String((const char*)(m["clientId"] | ""));
      cid.trim();
      ConfigStore::setMqttClientId(cid);
    }
    if (m.containsKey("baseTopic")) {
      String base = String((const char*)(m["baseTopic"] | ""));
      base.trim();
      ConfigStore::setMqttBaseTopic(base);
    }
    if (m.containsKey("publishIntervalMs")) {
      uint32_t ms = (uint32_t)(m["publishIntervalMs"] | 10000);
      if (ms < 1000UL) ms = 1000UL;
      if (ms > 600000UL) ms = 600000UL;
      ConfigStore::setMqttPublishIntervalMs(ms);
    }
    if (m.containsKey("homeAssistant") && m["homeAssistant"].is<JsonObjectConst>()) {
      JsonObjectConst ha = m["homeAssistant"].as<JsonObjectConst>();
      if (ha.containsKey("enabled")) ConfigStore::setMqttHaEnabled((bool)(ha["enabled"] | false));
      if (ha.containsKey("discovery")) ConfigStore::setMqttHaDiscovery((bool)(ha["discovery"] | true));
      if (ha.containsKey("discoveryPrefix")) {
        String prefix = String((const char*)(ha["discoveryPrefix"] | ""));
        prefix.trim();
        ConfigStore::setMqttDiscoveryPrefix(prefix);
      }
      if (ha.containsKey("nodeId")) {
        String nodeId = String((const char*)(ha["nodeId"] | ""));
        nodeId.trim();
        ConfigStore::setMqttNodeId(nodeId);
      }
    }
  }

  static void applyEquithermSection(JsonObjectConst e) {
    if (e.isNull()) return;
    String js;
    serializeJson(e, js);
    equithermApplyConfig(js);
  }

  static void applyDhwSection(JsonObjectConst d) {
    if (d.isNull()) return;
    DynamicJsonDocument wrap(8192);
    wrap["dhw"] = d;
    String js;
    serializeJson(wrap, js);
    dhwApplyConfig(js);
  }

  static void applyAlertsSection(JsonObjectConst a) {
    if (a.isNull()) return;
    ConfigStore::BatchGuard storeBatch;
    JsonObjectConst p = a.containsKey("pressure") && a["pressure"].is<JsonObjectConst>() ? a["pressure"].as<JsonObjectConst>() : a;
    if (p.isNull()) return;
    if (p.containsKey("enabled")) ConfigStore::setPressureAlarmEnabled((bool)(p["enabled"] | true));
    if (p.containsKey("minBar")) ConfigStore::setPressureAlarmMinBar((float)(p["minBar"] | ConfigStore::getPressureAlarmMinBar()));
    if (p.containsKey("maxBar")) ConfigStore::setPressureAlarmMaxBar((float)(p["maxBar"] | ConfigStore::getPressureAlarmMaxBar()));
    if (p.containsKey("hysteresisBar")) ConfigStore::setPressureAlarmHysteresisBar((float)(p["hysteresisBar"] | ConfigStore::getPressureAlarmHysteresisBar()));
  }


  static void handleReboot() {
    if (rejectActionRateLimit("reboot", 10000UL, 2, 60000UL, "reboot_guard")) return;
    DynamicJsonDocument doc(128);
    doc["ok"] = true;
    sendJsonDoc(200, doc);
    recordAdminAction("reboot", true, "scheduled");

    delay(250);
    ESP.restart();
  }

  static void handleDhwStatus() {
    sendJson(200, dhwGetStatusJson());
  }

  static void handleDhwCmd() {
    if (rejectActionRateLimit("dhw_cmd", 250UL, 12, 10000UL, "dhw_guard")) return;
    const String body = g_srv.arg("plain");
    String err;
    const bool ok = dhwHandleCmdJson(body, err);
    DynamicJsonDocument doc(256);
    doc["ok"] = ok;
    if (!ok) doc["err"] = err;
    sendJsonDoc(ok ? 200 : 400, doc);
    recordAdminAction("dhw_cmd", ok, ok ? "applied" : err.c_str());
  }

  static void handleOpenThermStatus() {
    sendJson(200, openthermGetStatusJson());
  }

  static void handleOpenThermCmd() {
    if (rejectActionRateLimit("ot_cmd", 250UL, 12, 10000UL, "ot_guard")) return;
    const String body = g_srv.arg("plain");
    String err;
    const bool ok = openthermHandleCmdJson(body, err);
    DynamicJsonDocument doc(512);
    doc["ok"] = ok;
    if (!ok) doc["err"] = err;
    sendJsonDoc(ok ? 200 : 400, doc);
    recordAdminAction("ot_cmd", ok, ok ? "applied" : err.c_str());
  }

  static void handleOpenThermScanStatus() {
    // includeAll=true so UI sees everything it has in cache
    sendJson(200, openthermScanGetStatusJson(true));
  }

  static void handleOpenThermScanProfile() {
    sendJson(200, openthermGetScanProfileJson());
  }

  static void handleOpenThermScanStart() {
    if (rejectActionRateLimit("ot_scan_start", 3000UL, 3, 60000UL, "scan_start_guard")) return;
    const String body = g_srv.arg("plain");
    bool includeAll = false;
    uint16_t delayMs = 60;
    if (body.length()) {
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, body) && doc.is<JsonObject>()) {
        includeAll = (bool)(doc["includeAll"] | false);
        delayMs = (uint16_t)(doc["delayMs"] | 60);
      }
    }
    if (delayMs < 10) delayMs = 10;
    if (delayMs > 500) delayMs = 500;
    const bool ok = openthermScanStart(0, 127, delayMs, includeAll);
    DynamicJsonDocument out(256);
    out["ok"] = ok;
    out["includeAll"] = includeAll;
    out["delayMs"] = delayMs;
    sendJsonDoc(ok ? 200 : 400, out);
    recordAdminAction("ot_scan_start", ok, ok ? "started" : "start_failed");
  }

  static void handleOpenThermScanStop() {
    if (rejectActionRateLimit("ot_scan_stop", 1000UL, 6, 60000UL, "scan_stop_guard")) return;
    openthermScanStop();
    DynamicJsonDocument out(128);
    out["ok"] = true;
    sendJsonDoc(200, out);
    recordAdminAction("ot_scan_stop", true, "stopped");
  }

  static void handleOpenThermDataIdRead() {
    const String body = g_srv.arg("plain");
    uint8_t id = 0;
    uint16_t reqValue = 0;
    if (body.length()) {
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, body) && doc.is<JsonObject>()) {
        id = (uint8_t)(doc["id"] | 0);
        reqValue = (uint16_t)(doc["reqValue"] | 0);
      }
    }
    if (id > 127) {
      DynamicJsonDocument err(128);
      err["ok"] = false; err["err"] = "bad_id";
      sendJsonDoc(400, err);
      return;
    }
    sendJson(200, openthermReadDataIdJson(id, reqValue));
  }

  static void handleOpenThermDataIdWrite() {
    if (rejectActionRateLimit("ot_data_write", 500UL, 8, 30000UL, "write_guard")) return;
    const String body = g_srv.arg("plain");
    uint8_t id = 0;
    uint16_t value = 0;
    bool have = false;

    if (body.length()) {
      StaticJsonDocument<384> doc;
      if (!deserializeJson(doc, body) && doc.is<JsonObject>()) {
        id = (uint8_t)(doc["id"] | 0);

        if (doc.containsKey("value")) {
          value = (uint16_t)(doc["value"] | 0);
          have = true;
        } else if (doc.containsKey("valueRaw")) {
          value = (uint16_t)(doc["valueRaw"] | 0);
          have = true;
        } else if (doc.containsKey("hb") || doc.containsKey("lb")) {
          uint8_t hb = (uint8_t)(doc["hb"] | 0);
          uint8_t lb = (uint8_t)(doc["lb"] | 0);
          value = (uint16_t)(((uint16_t)hb << 8) | (uint16_t)lb);
          have = true;
        } else if (doc.containsKey("valueF88")) {
          float v = doc["valueF88"].as<float>();
          // encode as signed f8.8
          int32_t raw = (int32_t)lroundf(v * 256.0f);
          if (raw < -32768) raw = -32768;
          if (raw > 32767) raw = 32767;
          value = (uint16_t)(raw & 0xFFFF);
          have = true;
        }
      }
    }

    if (id > 127 || !have) {
      DynamicJsonDocument err(160);
      err["ok"] = false; err["err"] = (!have ? "missing_value" : "bad_id");
      sendJsonDoc(400, err);
      return;
    }

    sendJson(200, openthermWriteDataIdJson(id, value));
    recordAdminAction("ot_data_write", true, "submitted");
  }

  static void handleDallasStatus() {
    DynamicJsonDocument doc(4096);
    doc["ok"] = true;
    JsonObject d = doc.createNestedObject("dallas");
    TemperatureManager::fillDallasJson(d);
    sendJsonDoc(200, doc);
  }

  static void handleBleStatus() {
    sendJson(200, bleGetStatusJson());
  }

  static void handleOtaStatus() {
    sendJson(200, otaGetStatusJson());
  }

  static void handleEquithermStatus() {
    sendJson(200, equithermGetStatusJson());
  }

  static void handleEquithermCmd() {
    if (rejectActionRateLimit("eq_cmd", 250UL, 12, 10000UL, "eq_guard")) return;
    const String body = g_srv.arg("plain");
    String err;
    const bool ok = equithermHandleCmdJson(body, err);
    DynamicJsonDocument doc(512);
    doc["ok"] = ok;
    if (!ok) doc["err"] = err;
    sendJsonDoc(ok ? 200 : 400, doc);
    recordAdminAction("eq_cmd", ok, ok ? "applied" : err.c_str());
  }

  static void handleFsList() {
    DynamicJsonDocument doc(12288);
    doc["ok"] = true;
    doc["fsMounted"] = g_fsMounted;
    doc["fsPartitionBytes"] = (uint32_t)getFilesystemPartitionLimit();
    doc["firmwarePartitionBytes"] = (uint32_t)getFirmwarePartitionLimit();
    JsonArray files = doc.createNestedArray("files");
    size_t total = 0;

    if (g_fsMounted) {
      File root = LittleFS.open("/", "r");
      if (root) {
        appendFsEntries(files, root);
      }
      for (JsonObject item : files) {
        if (!(bool)(item["dir"] | false)) total += (size_t)(uint32_t)(item["size"] | 0);
      }
    }
    doc["totalBytes"] = (uint32_t)total;
    sendJsonDoc(200, doc);
  }

  static size_t getFsReadOffsetArg() {
    if (!g_srv.hasArg("offset")) return 0;
    const long v = g_srv.arg("offset").toInt();
    return v > 0 ? (size_t)v : 0;
  }

  static size_t getFsReadLimitArg() {
    if (!g_srv.hasArg("limit")) return kFsReadChunkDefault;
    const long v = g_srv.arg("limit").toInt();
    if (v <= 0) return kFsReadChunkDefault;
    size_t limit = (size_t)v;
    if (limit > kFsReadChunkMax) limit = kFsReadChunkMax;
    return limit;
  }

  static void writeEscapedJsonStringChunk(const uint8_t* data, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    char out[256];
    size_t o = 0;
    auto flush = [&]() {
      if (o == 0) return;
      out[o] = 0;
      g_srv.sendContent(out);
      o = 0;
    };
    for (size_t i = 0; i < len; ++i) {
      const uint8_t c = data[i];
      if (o > sizeof(out) - 8) flush();
      switch (c) {
        case '\\': out[o++]='\\'; out[o++]='\\'; break;
        case '"': out[o++]='\\'; out[o++]='"'; break;
        case '\b': out[o++]='\\'; out[o++]='b'; break;
        case '\f': out[o++]='\\'; out[o++]='f'; break;
        case '\n': out[o++]='\\'; out[o++]='n'; break;
        case '\r': out[o++]='\\'; out[o++]='r'; break;
        case '\t': out[o++]='\\'; out[o++]='t'; break;
        default:
          if (c < 0x20) {
            out[o++]='\\'; out[o++]='u'; out[o++]='0'; out[o++]='0';
            out[o++]=hex[(c >> 4) & 0x0F]; out[o++]=hex[c & 0x0F];
          } else {
            out[o++]=(char)c;
          }
          break;
      }
    }
    flush();
  }

  static void sendFsReadChunk(File& f, const String& path, size_t totalSize, size_t offset, size_t limit) {
    const size_t boundedOffset = offset > totalSize ? totalSize : offset;
    const size_t remaining = totalSize - boundedOffset;
    const size_t chunkBytes = remaining < limit ? remaining : limit;
    const bool truncated = (boundedOffset + chunkBytes) < totalSize;

    g_srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
    g_srv.send(200, "application/json; charset=utf-8", "");
    g_srv.sendContent("{\"ok\":true,\"path\":\"");
    writeEscapedJsonStringChunk((const uint8_t*)path.c_str(), path.length());
    char meta[256];
    snprintf(meta, sizeof(meta), "\",\"size\":%u,\"offset\":%u,\"limit\":%u,\"returnedBytes\":%u,\"truncated\":%s,\"content\":\"",
             (unsigned)totalSize, (unsigned)boundedOffset, (unsigned)limit, (unsigned)chunkBytes, truncated ? "true" : "false");
    g_srv.sendContent(meta);

    if (chunkBytes > 0) {
      f.seek(boundedOffset, SeekSet);
      uint8_t buf[512];
      size_t left = chunkBytes;
      while (left > 0 && f.available()) {
        const size_t want = left < sizeof(buf) ? left : sizeof(buf);
        const size_t n = f.read(buf, want);
        if (n == 0) break;
        writeEscapedJsonStringChunk(buf, n);
        left -= n;
      }
    }

    if (truncated) {
      char tail[80];
      snprintf(tail, sizeof(tail), "\",\"nextOffset\":%u}", (unsigned)(boundedOffset + chunkBytes));
      g_srv.sendContent(tail);
    } else {
      g_srv.sendContent("\"}");
    }
  }

  static void handleFsRead() {
    String path = g_srv.hasArg("path") ? g_srv.arg("path") : String();
    path = ensureLeadingSlash(path);
    if (!g_fsMounted) { writeUploadJson(500, false, "littlefs_not_mounted"); return; }
    if (!isSafeFsPath(path) || path == "/") { writeUploadJson(400, false, "bad_path"); return; }
    if (!LittleFS.exists(path)) { writeUploadJson(404, false, "not_found", path); return; }
    if (!isTextLikePath(path)) { writeUploadJson(415, false, "not_text_file", path); return; }

    File f = LittleFS.open(path, "r");
    if (!f || f.isDirectory()) { writeUploadJson(400, false, "not_file", path); return; }
    const size_t totalSize = (size_t)f.size();
    const size_t offset = getFsReadOffsetArg();
    const size_t limit = getFsReadLimitArg();
    g_srv.sendHeader("Cache-Control", "no-store");
    sendFsReadChunk(f, path, totalSize, offset, limit);
    f.close();
  }

  static void handleFsMkdir() {
    if (rejectActionRateLimit("fs_mkdir", 500UL, 10, 60000UL, "fs_guard")) return;
    String path = g_srv.hasArg("path") ? g_srv.arg("path") : String();
    path = ensureLeadingSlash(path);
    if (!g_fsMounted) { writeUploadJson(500, false, "littlefs_not_mounted"); return; }
    if (!isSafeFsPath(path) || path == "/") { writeUploadJson(400, false, "bad_path"); return; }
    if (LittleFS.exists(path)) { writeUploadJson(409, false, "already_exists", path); return; }
    const bool ok = LittleFS.mkdir(path);
    writeUploadJson(ok ? 200 : 500, ok, ok ? "created" : "mkdir_failed", path);
    recordAdminAction("fs_mkdir", ok, path.c_str());
  }

  static void handleFsRename() {
    if (rejectActionRateLimit("fs_rename", 500UL, 10, 60000UL, "fs_guard")) return;
    String from = g_srv.hasArg("from") ? g_srv.arg("from") : String();
    String to = g_srv.hasArg("to") ? g_srv.arg("to") : String();
    from = ensureLeadingSlash(from);
    to = ensureLeadingSlash(to);
    if (!g_fsMounted) { writeUploadJson(500, false, "littlefs_not_mounted"); return; }
    if (!isSafeFsPath(from) || !isSafeFsPath(to) || from == "/" || to == "/") { writeUploadJson(400, false, "bad_path"); return; }
    if (!LittleFS.exists(from)) { writeUploadJson(404, false, "not_found", from); return; }
    if (LittleFS.exists(to)) { writeUploadJson(409, false, "target_exists", to); return; }
    const bool ok = LittleFS.rename(from, to);
    DynamicJsonDocument doc(512);
    doc["ok"] = ok;
    doc["msg"] = ok ? "renamed" : "rename_failed";
    doc["from"] = from;
    doc["to"] = to;
    sendJsonDoc(ok ? 200 : 500, doc);
    recordAdminAction("fs_rename", ok, ok ? to.c_str() : from.c_str());
  }

  static void handleFsWrite() {
    if (rejectActionRateLimit("fs_write", 500UL, 12, 60000UL, "fs_guard")) return;
    String path = g_srv.hasArg("path") ? g_srv.arg("path") : String();
    path = ensureLeadingSlash(path);
    if (!g_fsMounted) { writeUploadJson(500, false, "littlefs_not_mounted"); return; }
    if (!isSafeFsPath(path) || path == "/") { writeUploadJson(400, false, "bad_path"); return; }

    String content;
    if (g_srv.hasArg("plain")) {
      content = g_srv.arg("plain");
    } else {
      DynamicJsonDocument in(1024);
      if (deserializeJson(in, g_srv.arg("plain")) == DeserializationError::Ok) {
        content = String((const char*)(in["content"] | ""));
      }
    }

    const bool existedBefore = LittleFS.exists(path);
    const bool ok = writeTextFile(path, content);
    DynamicJsonDocument doc(512);
    doc["ok"] = ok;
    doc["msg"] = ok ? (existedBefore ? "updated" : "created") : "write_failed";
    doc["path"] = path;
    doc["size"] = (uint32_t)content.length();
    sendJsonDoc(ok ? 200 : 500, doc);
    recordAdminAction("fs_write", ok, path.c_str());
  }

  static void handleFsDelete() {
    if (rejectActionRateLimit("fs_delete", 750UL, 8, 60000UL, "fs_guard")) return;
    String path = g_srv.hasArg("path") ? g_srv.arg("path") : String();
    path = ensureLeadingSlash(path);
    if (!g_fsMounted) { writeUploadJson(500, false, "littlefs_not_mounted"); return; }
    if (!isSafeFsPath(path) || path == "/") { writeUploadJson(400, false, "bad_path"); return; }
    if (!LittleFS.exists(path)) { writeUploadJson(404, false, "not_found", path); return; }
    const bool ok = removeFsEntryRecursive(path);
    writeUploadJson(ok ? 200 : 500, ok, ok ? "deleted" : "delete_failed", path);
    recordAdminAction("fs_delete", ok, path.c_str());
  }

  static void handleFsUpload() {
    UploadContext& u = g_fsUpload;
    int code = (u.ok && u.message == "uploaded") ? 200 : 400;
    if (!u.ok && u.message == "littlefs_not_mounted") code = 500;
    writeUploadJson(code, u.ok, u.message.length() ? u.message : String("upload_failed"), u.targetPath);
    u = UploadContext();
  }

  static void handleFsUploadData() {

    HTTPUpload& up = g_srv.upload();
    if (up.status == UPLOAD_FILE_START && rejectActionRateLimit("fs_upload", 1000UL, 4, 60000UL, "fs_upload_guard")) return;
    UploadContext& u = g_fsUpload;

    if (up.status == UPLOAD_FILE_START) {
      u = UploadContext();
      if (!g_fsMounted) { u.message = "littlefs_not_mounted"; return; }
      String requestedPath;
      if (g_srv.hasArg("path")) requestedPath = g_srv.arg("path");
      else if (g_srv.hasArg("targetPath")) requestedPath = g_srv.arg("targetPath");
      u.targetPath = pickUploadPath(requestedPath, up.filename);
      if (!u.targetPath.length()) { u.message = "bad_path"; return; }
      u.file = LittleFS.open(u.targetPath, "w");
      if (!u.file) { u.message = "open_failed"; return; }
      u.active = true;
      u.ok = true;
      u.bytesReceived = 0;
      u.expectedSize = (size_t)up.totalSize;
      u.message = "uploading";
      recordAdminAction("fs_upload", true, u.targetPath.c_str());
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (!u.ok || !u.file) return;
      if (u.file.write(up.buf, up.currentSize) != up.currentSize) {
        u.ok = false;
        u.message = "write_failed";
      }
    } else if (up.status == UPLOAD_FILE_END) {
      if (u.file) u.file.close();
      u.active = false;
      if (u.ok) u.message = "uploaded";
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      if (u.file) u.file.close();
      if (u.targetPath.length() && LittleFS.exists(u.targetPath)) LittleFS.remove(u.targetPath);
      u.active = false;
      u.ok = false;
      u.message = "aborted";
    }
  }

  static void handleFirmwareUpdate() {
    UploadContext& u = g_fwUpload;
    const bool ok = u.ok && (u.message == "uploaded");
    DynamicJsonDocument doc(512);
    doc["ok"] = ok;
    doc["msg"] = ok ? "firmware_uploaded_rebooting" : (u.message.length() ? u.message : "update_failed");
    doc["partitionBytes"] = (uint32_t)u.partitionSize;
    doc["receivedBytes"] = (uint32_t)u.bytesReceived;
    sendJsonDoc(ok ? 200 : 400, doc);
    const bool reboot = ok;
    u = UploadContext();
    if (reboot) { delay(250); ESP.restart(); }
  }

  static void handleFirmwareUpdateData() {
    HTTPUpload& up = g_srv.upload();
    if (up.status == UPLOAD_FILE_START && rejectActionRateLimit("fw_update", 10000UL, 2, 600000UL, "fw_update_guard")) return;
    UploadContext& u = g_fwUpload;
    if (up.status == UPLOAD_FILE_START) {
      u = UploadContext();
      u.partitionSize = getFirmwarePartitionLimit();
      u.expectedSize = (size_t)up.totalSize;
      if (u.partitionSize && u.expectedSize && u.expectedSize > u.partitionSize) {
        u.ok = false;
        u.message = "image_exceeds_firmware_partition";
        return;
      }
      u.active = true;
      u.ok = Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
      u.message = u.ok ? "uploading" : String("begin_failed");
      recordAdminAction("fw_update", u.ok, u.ok ? "started" : u.message.c_str());
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (!u.ok) return;
      u.bytesReceived += up.currentSize;
      if (u.partitionSize && u.bytesReceived > u.partitionSize) {
        Update.abort();
        u.ok = false;
        u.message = "image_exceeds_firmware_partition";
        return;
      }
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        u.ok = false;
        u.message = String("write_failed:") + Update.errorString();
      }
    } else if (up.status == UPLOAD_FILE_END) {
      u.active = false;
      if (u.ok && Update.end(true)) u.message = "uploaded";
      else { u.ok = false; u.message = String("end_failed:") + Update.errorString(); }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
      u.active = false;
      u.ok = false;
      u.message = "aborted";
    }
  }

  static void handleFilesystemUpdate() {
    UploadContext& u = g_fsImageUpload;
    const bool ok = u.ok && (u.message == "uploaded");
    DynamicJsonDocument doc(512);
    doc["ok"] = ok;
    doc["msg"] = ok ? "filesystem_uploaded_rebooting" : (u.message.length() ? u.message : "update_failed");
    doc["partitionBytes"] = (uint32_t)u.partitionSize;
    doc["receivedBytes"] = (uint32_t)u.bytesReceived;
    sendJsonDoc(ok ? 200 : 400, doc);
    const bool reboot = ok;
    u = UploadContext();
    if (reboot) { delay(250); ESP.restart(); }
  }

  static void handleFilesystemUpdateData() {
    HTTPUpload& up = g_srv.upload();
    if (up.status == UPLOAD_FILE_START && rejectActionRateLimit("fs_update", 10000UL, 2, 600000UL, "fs_update_guard")) return;
    UploadContext& u = g_fsImageUpload;
    if (up.status == UPLOAD_FILE_START) {
      u = UploadContext();
      u.partitionSize = getFilesystemPartitionLimit();
      u.expectedSize = (size_t)up.totalSize;
      if (u.partitionSize && u.expectedSize && u.expectedSize != u.partitionSize) {
        u.ok = false;
        u.message = "filesystem_image_size_mismatch";
        return;
      }
      u.active = true;
      u.ok = Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS);
      u.message = u.ok ? "uploading" : String("begin_failed");
      recordAdminAction("fs_update", u.ok, u.ok ? "started" : u.message.c_str());
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (!u.ok) return;
      u.bytesReceived += up.currentSize;
      if (u.partitionSize && u.bytesReceived > u.partitionSize) {
        Update.abort();
        u.ok = false;
        u.message = "filesystem_image_exceeds_partition";
        return;
      }
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        u.ok = false;
        u.message = String("write_failed:") + Update.errorString();
      }
    } else if (up.status == UPLOAD_FILE_END) {
      if (u.partitionSize && u.bytesReceived != u.partitionSize) {
        Update.abort();
        u.ok = false;
        u.message = "filesystem_image_size_mismatch";
        return;
      }
      if (u.ok && Update.end(true)) u.message = "uploaded";
      else { u.ok = false; u.message = String("end_failed:") + Update.errorString(); }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
      u.active = false;
      u.ok = false;
      u.message = "aborted";
    }
  }

  static void handleNotFound() {
    if (g_srv.method() == HTTP_GET) {
      if (tryServeFsFile(g_srv.uri())) return;
      handleRoot();
      return;
    }
    g_srv.send(404, "text/plain; charset=utf-8", "Not found");
  }


}

void webPortalInit() {
  if (g_started) return;
  g_started = true;

  g_fsMounted = LittleFS.begin(false);
  if (!g_fsMounted) {
    Serial.println("[WEB] LittleFS mount failed, trying recovery format");
    g_fsMounted = LittleFS.begin(true);
  }

  // NVS remains the fallback store, but valid LittleFS JSON can override it at boot.
  ConfigRuntime::loadAllFromStore();
  const size_t bootImportedSections = bootImportConfigFromLittleFS();
  ConfigRuntime::applyAllRuntime();
  if (bootImportedSections > 0) {
    Serial.printf("[WEB] Active config hydrated from LittleFS (%u sections)\n", (unsigned)bootImportedSections);
  } else {
    Serial.println("[WEB] Active config hydrated from NVS");
  }

  g_srv.on("/", HTTP_GET, handleRoot);
  g_srv.on("/index.html", HTTP_GET, handleRoot);
  g_srv.on("/filemanager", HTTP_GET, handleFileManager);
  g_srv.on("/filemanager/", HTTP_GET, handleFileManager);
  g_srv.on("/app.css", HTTP_GET, handleCss);
  g_srv.on("/app.js", HTTP_GET, handleJs);

  g_srv.on("/api/fast", HTTP_GET, handleFast);
  g_srv.on("/api/bootstrap", HTTP_GET, handleBootstrap);
  g_srv.on("/api/config", HTTP_GET, handleConfigGet);
  g_srv.on("/api/config/apply", HTTP_POST, handleConfigApply);
  g_srv.on("/api/config/export", HTTP_POST, handleConfigExport);
  g_srv.on("/api/config/import", HTTP_POST, handleConfigImport);
  g_srv.on("/api/config/inputs", HTTP_GET, handleInputsConfigGet);
  g_srv.on("/api/config/inputs", HTTP_POST, handleInputsConfigPost);
  g_srv.on("/api/config/equitherm", HTTP_GET, handleEquithermConfigGet);
  g_srv.on("/api/config/equitherm", HTTP_POST, handleEquithermConfigPost);
  g_srv.on("/api/config/opentherm", HTTP_GET, handleOpenThermConfigGet);
  g_srv.on("/api/config/opentherm", HTTP_POST, handleOpenThermConfigPost);
  g_srv.on("/api/config/ble", HTTP_GET, handleBleConfigGet);
  g_srv.on("/api/config/ble", HTTP_POST, handleBleConfigPost);
  g_srv.on("/api/config/ota", HTTP_GET, handleOtaConfigGet);
  g_srv.on("/api/config/ota", HTTP_POST, handleOtaConfigPost);
  g_srv.on("/api/config/dhw", HTTP_GET, handleDhwConfigGet);
  g_srv.on("/api/config/dhw", HTTP_POST, handleDhwConfigPost);
  g_srv.on("/api/config/dallas", HTTP_GET, handleDallasConfigGet);
  g_srv.on("/api/config/dallas", HTTP_POST, handleDallasConfigPost);
  g_srv.on("/api/config/alerts", HTTP_GET, handleAlertsConfigGet);
  g_srv.on("/api/config/alerts", HTTP_POST, handleAlertsConfigPost);
  g_srv.on("/api/config/mqtt", HTTP_GET, handleMqttConfigGet);
  g_srv.on("/api/config/mqtt", HTTP_POST, handleMqttConfigPost);
  g_srv.on("/api/config/time", HTTP_GET, handleTimeConfigGet);
  g_srv.on("/api/config/time", HTTP_POST, handleTimeConfigPost);
  g_srv.on("/api/mqtt/status", HTTP_GET, handleMqttStatus);
  g_srv.on("/api/events", HTTP_GET, [](){ sendJson(200, EventLog::toJson()); });
  g_srv.on("/api/events/clear", HTTP_POST, [](){ EventLog::clear(); DynamicJsonDocument d(64); d["ok"]=true; sendJsonDoc(200,d); });
  g_srv.on("/api/history", HTTP_GET, [](){ sendJson(200, HistoryBuffer::toJson()); });
  g_srv.on("/api/history/clear", HTTP_POST, [](){ HistoryBuffer::clear(); DynamicJsonDocument d(64); d["ok"]=true; sendJsonDoc(200,d); });
  g_srv.on("/api/service/io", HTTP_POST, [](){
    DynamicJsonDocument in(512), out(256);
    if (deserializeJson(in, g_srv.arg("plain"))) { out["ok"]=false; out["err"]="bad_json"; sendJsonDoc(400,out); return; }
    JsonObject o = in.as<JsonObject>();
    if (o.containsKey("relay")) { int r=(int)(o["relay"]|0); bool on=(bool)(o["on"]|false); if (r>=1 && r<=8) relaySet((RelayId)(r-1), on); }
    if (o.containsKey("pulseRelay")) { int r=(int)(o["pulseRelay"]|0); uint32_t ms=(uint32_t)(o["pulseMs"]|500); if (r>=1 && r<=8) { relaySet((RelayId)(r-1), true); delay(ms); relaySet((RelayId)(r-1), false);} }
    const char* bz = o["buzzer"] | nullptr; if (bz && !strcmp(bz, "startup")) buzzerPlayStartup(); else if (bz && !strcmp(bz, "warning")) buzzerPlayWarning(true); else if (bz && !strcmp(bz, "off")) buzzerPlayWarning(false);
    out["ok"]=true; sendJsonDoc(200,out); EventLog::record("service", "io_test", "manual");
  });
  g_srv.on("/api/relay", HTTP_POST, handleRelayPost);
  g_srv.on("/api/system/cmd", HTTP_POST, handleSystemCmd);
  g_srv.on("/api/reboot", HTTP_POST, handleReboot);

  g_srv.on("/api/opentherm/status", HTTP_GET, handleOpenThermStatus);
  g_srv.on("/api/dhw/status", HTTP_GET, handleDhwStatus);
  g_srv.on("/api/dhw/cmd", HTTP_POST, handleDhwCmd);
  g_srv.on("/api/opentherm/cmd", HTTP_POST, handleOpenThermCmd);

  g_srv.on("/api/equitherm/status", HTTP_GET, handleEquithermStatus);
  g_srv.on("/api/equitherm/cmd", HTTP_POST, handleEquithermCmd);
  g_srv.on("/api/opentherm/scan/status", HTTP_GET, handleOpenThermScanStatus);
  g_srv.on("/api/opentherm/scan/profile", HTTP_GET, handleOpenThermScanProfile);
  g_srv.on("/api/opentherm/scan/start", HTTP_POST, handleOpenThermScanStart);
  g_srv.on("/api/opentherm/scan/stop", HTTP_POST, handleOpenThermScanStop);

  g_srv.on("/api/opentherm/dataid/read", HTTP_POST, handleOpenThermDataIdRead);
  g_srv.on("/api/opentherm/dataid/write", HTTP_POST, handleOpenThermDataIdWrite);

  g_srv.on("/api/dallas/status", HTTP_GET, handleDallasStatus);
  g_srv.on("/api/ble/status", HTTP_GET, handleBleStatus);
  g_srv.on("/api/ota/status", HTTP_GET, handleOtaStatus);

  g_srv.on("/api/fs/list", HTTP_GET, handleFsList);
  g_srv.on("/api/fs/read", HTTP_GET, handleFsRead);
  g_srv.on("/api/fs/write", HTTP_POST, handleFsWrite);
  g_srv.on("/api/fs/mkdir", HTTP_POST, handleFsMkdir);
  g_srv.on("/api/fs/rename", HTTP_POST, handleFsRename);
  g_srv.on("/api/fs/delete", HTTP_POST, handleFsDelete);
  g_srv.on("/api/fs/upload", HTTP_POST, handleFsUpload, handleFsUploadData);
  g_srv.on("/api/update/firmware", HTTP_POST, handleFirmwareUpdate, handleFirmwareUpdateData);
  g_srv.on("/api/update/filesystem", HTTP_POST, handleFilesystemUpdate, handleFilesystemUpdateData);

  g_srv.onNotFound(handleNotFound);
  g_ws.begin();
  g_ws.onEvent(handleWsEvent);
  g_srv.collectHeaders(kCollectedHeaders, 1);
  g_srv.begin();

  Serial.println("[WEB] Portal started on http://" + getBestIpString() + "/");
}

void webPortalLoop() {
  if (!g_started) return;
  g_srv.handleClient();
  g_ws.loop();

  const unsigned long now = millis();
  if (g_wsClientCount > 0 && (now - g_wsLastPushMs) >= 1000UL) {
    g_wsLastPushMs = now;
    const String msg = buildFastWsFrame(false);
    if (msg.length()) g_ws.broadcastTXT(msg.c_str(), msg.length());
  }
}

#endif // FEATURE_WEBPORTAL
