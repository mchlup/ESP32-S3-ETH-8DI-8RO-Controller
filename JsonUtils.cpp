#include "JsonUtils.h"

#include <FS.h>
#include <LittleFS.h>
#include "FsController.h"
#include "Log.h"

namespace {
    JsonDiagnostics s_diag;

    size_t calcCapacityForSize(size_t size) {
        size_t cap = (size * 13) / 10 + 1024;
        if (cap < 2048) cap = 2048;
        if (cap > 65536) cap = 65536;
        return cap;
    }

    void updateDiagnostics(const DynamicJsonDocument& doc) {
        s_diag.lastCapacity = doc.capacity();
        s_diag.lastUsage = doc.memoryUsage();
    }

    void recordJsonError(const String& err) {
        s_diag.parseErrors++;
        s_diag.lastError = err;
        LOGW("JSON error: %s", err.c_str());
    }
}

const JsonDiagnostics& jsonGetDiagnostics() {
    return s_diag;
}

void ValidationResult::addIssue(const String& code, const String& path, const String& message, bool fatal) {
    ValidationIssue issue;
    issue.code = code;
    issue.path = path;
    issue.message = message;
    issue.fatal = fatal;
    issues.push_back(issue);
    if (fatal) {
        ok = false;
    } else {
        corrected = true;
    }
}

bool loadJsonFromFile(const char* path, DynamicJsonDocument& doc, String& err) {
    err = "";
    if (!path || !path[0]) {
        err = "missing_path";
        recordJsonError(err);
        return false;
    }

    fsLock();
    File file = LittleFS.open(path, "r");
    fsUnlock();
    if (!file) {
        err = "open_failed";
        recordJsonError(err);
        return false;
    }

    const size_t size = (size_t)file.size();
    const size_t cap = calcCapacityForSize(size);
    doc = DynamicJsonDocument(cap);
    DeserializationError de = deserializeJson(doc, file);
    file.close();

    updateDiagnostics(doc);

    if (de) {
        err = String("deserialize_failed:") + de.c_str();
        recordJsonError(err);
        return false;
    }

    LOGD("JSON loaded: size=%u cap=%u used=%u", (unsigned)size, (unsigned)doc.capacity(), (unsigned)doc.memoryUsage());
    return true;
}

bool saveJsonToFileAtomic(const char* path, const JsonDocument& doc, String& err) {
    err = "";
    if (!path || !path[0]) {
        err = "missing_path";
        recordJsonError(err);
        return false;
    }

    const String tmpPath = String(path) + ".tmp";
    const String bakPath = String(path) + ".bak";

    fsLock();
    File wf = LittleFS.open(tmpPath, "w");
    if (!wf) {
        fsUnlock();
        err = "open_tmp_failed";
        recordJsonError(err);
        return false;
    }

    if (serializeJson(doc, wf) == 0) {
        wf.close();
        LittleFS.remove(tmpPath);
        fsUnlock();
        err = "serialize_failed";
        recordJsonError(err);
        return false;
    }

    wf.flush();
    wf.close();

    if (LittleFS.exists(path)) {
        LittleFS.remove(bakPath);
        if (!LittleFS.rename(path, bakPath)) {
            LittleFS.remove(tmpPath);
            fsUnlock();
            err = "backup_failed";
            recordJsonError(err);
            return false;
        }
    }

    if (!LittleFS.rename(tmpPath, path)) {
        LittleFS.remove(tmpPath);
        fsUnlock();
        err = "rename_failed";
        recordJsonError(err);
        return false;
    }

    fsUnlock();
    return true;
}

bool parseJsonBody(Stream& client, DynamicJsonDocument& doc, size_t maxBytes, String& err) {
    err = "";
    String body;
    body.reserve(maxBytes + 1);

    while (client.available()) {
        if (body.length() >= maxBytes) {
            err = "payload_too_large";
            recordJsonError(err);
            return false;
        }
        char c = (char)client.read();
        body += c;
    }

    if (!body.length()) {
        err = "empty_body";
        recordJsonError(err);
        return false;
    }

    return parseJsonBody(body, doc, maxBytes, err);
}

bool parseJsonBody(const String& body, DynamicJsonDocument& doc, size_t maxBytes, String& err) {
    err = "";
    if (!body.length()) {
        err = "empty_body";
        recordJsonError(err);
        return false;
    }
    if (body.length() > maxBytes) {
        err = "payload_too_large";
        recordJsonError(err);
        return false;
    }

    const size_t cap = calcCapacityForSize(body.length());
    doc = DynamicJsonDocument(cap);
    DeserializationError de = deserializeJson(doc, body);
    updateDiagnostics(doc);
    if (de) {
        err = String("bad_json:") + de.c_str();
        recordJsonError(err);
        return false;
    }
    LOGD("JSON parsed: len=%u cap=%u used=%u", (unsigned)body.length(), (unsigned)doc.capacity(), (unsigned)doc.memoryUsage());
    return true;
}

void applyDefaultsAndValidate(JsonDocument& doc, ValidationResult& out) {
    out.ok = true;
    out.corrected = false;

    if (!doc.is<JsonObject>()) {
        out.addIssue("wrong_type", "$", "Kořen JSON musí být objekt", true);
        return;
    }

    JsonObject root = doc.as<JsonObject>();
    const char* objectKeys[] = {
        "iofunc", "equitherm", "tuv", "dhwRecirc", "akuHeater", "system",
        "sensors", "schedules", "mqtt", "time", "thermometers", "tempRoles",
        "opentherm"
    };
    const char* arrayKeys[] = {
        "relayNames", "inputNames", "inputActiveLevels", "inputs", "relayMap",
        "modes", "modeNames", "modeDescriptions", "mode_names", "mode_descriptions",
        "dallasGpios", "dallasAddrs", "dallasNames"
    };

    for (const char* key : objectKeys) {
        if (!root.containsKey(key)) {
            root.createNestedObject(key);
            out.addIssue("missing_key", String("$.") + key, "Doplněn výchozí objekt");
            continue;
        }
        if (!root[key].is<JsonObject>()) {
            root.remove(key);
            root.createNestedObject(key);
            out.addIssue("wrong_type", String("$.") + key, "Přepsáno na objekt");
        }
    }

    for (const char* key : arrayKeys) {
        if (!root.containsKey(key)) {
            root.createNestedArray(key);
            out.addIssue("missing_key", String("$.") + key, "Doplněno prázdné pole");
            continue;
        }
        if (!root[key].is<JsonArray>()) {
            root.remove(key);
            root.createNestedArray(key);
            out.addIssue("wrong_type", String("$.") + key, "Přepsáno na pole");
        }
    }

    if (root.containsKey("autoDefaultOffUnmapped") && !root["autoDefaultOffUnmapped"].is<bool>()) {
        root["autoDefaultOffUnmapped"] = false;
        out.addIssue("wrong_type", "$.autoDefaultOffUnmapped", "Přepsáno na bool");
    }
    if (root.containsKey("auto_default_off_unmapped") && !root["auto_default_off_unmapped"].is<bool>()) {
        root["auto_default_off_unmapped"] = false;
        out.addIssue("wrong_type", "$.auto_default_off_unmapped", "Přepsáno na bool");
    }
}
