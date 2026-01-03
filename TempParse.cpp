#include "TempParse.h"

#include <ArduinoJson.h>

static bool parseFloatLoose(const String& sIn, float& out) {
    String s = sIn;
    s.trim();
    if (!s.length()) return false;

    // remove common unit suffixes
    // (keeps first numeric token)
    const char* cstr = s.c_str();
    char* endp = nullptr;
    float v = strtof(cstr, &endp);
    if (endp == cstr) return false;

    // allow trailing whitespace / unit chars
    while (endp && *endp) {
        if (!isspace((unsigned char)*endp) && *endp != 'C' && *endp != 'c' && *endp != '°') {
            // unknown trailing token -> still accept if numeric parsed ("23.1foo" is likely bad)
            // but keep strict-ish: reject
            return false;
        }
        endp++;
    }

    if (!isfinite(v)) return false;
    out = v;
    return true;
}

static JsonVariant getByPath(JsonVariant root, const String& path) {
    if (!root || !path.length()) return JsonVariant();

    JsonVariant cur = root;
    int start = 0;
    while (start < (int)path.length()) {
        int dot = path.indexOf('.', start);
        int slash = path.indexOf('/', start);
        int end = -1;
        if (dot < 0) end = slash;
        else if (slash < 0) end = dot;
        else end = (dot < slash) ? dot : slash;

        String seg = (end < 0) ? path.substring(start) : path.substring(start, end);
        seg.trim();
        if (!seg.length()) {
            start = (end < 0) ? (int)path.length() : end + 1;
            continue;
        }

        if (cur.is<JsonObject>()) {
            cur = cur.as<JsonObject>()[seg];
        } else if (cur.is<JsonArray>()) {
            // array index
            int idx = seg.toInt();
            cur = cur.as<JsonArray>()[idx];
        } else {
            return JsonVariant();
        }

        start = (end < 0) ? (int)path.length() : end + 1;
    }

    return cur;
}

static bool variantToFloat(JsonVariant v, float& out) {
    if (!v) return false;
    if (v.is<float>() || v.is<double>()) {
        float f = v.as<float>();
        if (!isfinite(f)) return false;
        out = f;
        return true;
    }
    if (v.is<long>() || v.is<int>() || v.is<short>()) {
        out = (float)v.as<long>();
        return isfinite(out);
    }
    if (v.is<const char*>() || v.is<String>()) {
        String s = v.as<String>();
        return parseFloatLoose(s, out);
    }
    return false;
}

bool tempParseFromPayload(const String& payload, const String& jsonKey, float& outTempC) {
    // 1) plain number
    if (parseFloatLoose(payload, outTempC)) return true;

    // 2) JSON
    String s = payload;
    s.trim();
    if (!s.length()) return false;
    const char first = s[0];
    if (first != '{' && first != '[' && first != '"') return false;

    DynamicJsonDocument doc(768);
    DeserializationError err = deserializeJson(doc, s);
    if (err) return false;

    JsonVariant root = doc.as<JsonVariant>();

    // If root is a value already
    if (variantToFloat(root, outTempC)) return true;

    // Try requested key/path
    if (jsonKey.length()) {
        JsonVariant v = getByPath(root, jsonKey);
        if (variantToFloat(v, outTempC)) return true;
    }

    // Auto-detect common keys
    static const char* keys[] = { "tempC", "temperature", "temp", "t", "value" };
    for (auto k : keys) {
        JsonVariant v = getByPath(root, String(k));
        if (variantToFloat(v, outTempC)) return true;
    }

    return false;
}
