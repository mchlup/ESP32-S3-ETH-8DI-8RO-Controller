#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Parse temperature from an arbitrary payload.
// Accepts plain numbers ("23.4"), JSON with common fields,
// and values with units ("23.4C").

inline bool parseTempC(const String& payload, float& outC) {
  String s = payload;
  s.trim();
  if (!s.length()) return false;

  // Fast path: plain number
  char* endp = nullptr;
  outC = strtof(s.c_str(), &endp);
  if (endp && endp != s.c_str()) {
    if (isfinite(outC)) return true;
  }

  // JSON-ish: try to find first number in string
  bool seen = false;
  String num;
  for (size_t i=0;i<s.length();i++) {
    char c = s[i];
    if ((c>='0'&&c<='9') || c=='-' || c=='+' || c=='.') {
      num += c;
      seen = true;
    } else {
      if (seen) break;
    }
  }
  if (!seen) return false;
  outC = strtof(num.c_str(), nullptr);
  return isfinite(outC);
}

// Extract temperature from payload with optional JSON key.
// - If jsonKey is empty: falls back to parseTempC()
// - If payload is JSON and jsonKey exists: uses that value (number or string)
inline bool tempParseFromPayload(const String& payload, const String& jsonKey, float& outC) {
  if (!jsonKey.length()) return parseTempC(payload, outC);

  // Try JSON parse only when it looks like JSON.
  String s = payload;
  s.trim();
  if (!s.length()) return false;
  if (!(s[0] == '{' || s[0] == '[')) {
    // Not JSON; fallback to generic
    return parseTempC(payload, outC);
  }

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, s);
  if (err) return parseTempC(payload, outC);

  JsonVariant v;
  if (doc.is<JsonObject>()) {
    v = doc.as<JsonObject>()[jsonKey];
  } else if (doc.is<JsonArray>()) {
    // Support simple array payloads; jsonKey may be numeric index
    int idx = jsonKey.toInt();
    JsonArray a = doc.as<JsonArray>();
    if (idx >= 0 && idx < (int)a.size()) v = a[idx];
  }
  if (v.isNull()) return false;
  if (v.is<float>() || v.is<int>() || v.is<long>() || v.is<double>()) {
    outC = v.as<float>();
    return isfinite(outC);
  }
  if (v.is<const char*>()) {
    return parseTempC(String((const char*)v.as<const char*>()), outC);
  }
  return false;
}
