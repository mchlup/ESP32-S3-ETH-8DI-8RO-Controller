#include "TempParse.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <stdlib.h>

namespace {

bool isAllowedTemperatureSuffix(const char* suffix) {
  if (suffix == nullptr) return true;

  while (*suffix != '\0' && isspace(static_cast<unsigned char>(*suffix))) {
    ++suffix;
  }

  // Optional UTF-8 degree sign: C2 B0.
  if (static_cast<unsigned char>(suffix[0]) == 0xC2U &&
      static_cast<unsigned char>(suffix[1]) == 0xB0U) {
    suffix += 2;
    while (*suffix != '\0' && isspace(static_cast<unsigned char>(*suffix))) {
      ++suffix;
    }
  }

  if (*suffix == 'C' || *suffix == 'c') {
    ++suffix;
  }

  while (*suffix != '\0' && isspace(static_cast<unsigned char>(*suffix))) {
    ++suffix;
  }

  return *suffix == '\0';
}

bool parseFloatStrict(const String& input, float& outValue) {
  String text = input;
  text.trim();
  if (text.isEmpty()) return false;

  const char* begin = text.c_str();
  char* end = nullptr;
  const float value = strtof(begin, &end);

  if (end == begin || !isfinite(value) || !isAllowedTemperatureSuffix(end)) {
    return false;
  }

  outValue = value;
  return true;
}

bool parseFirstNumericToken(const String& input, float& outValue) {
  const char* text = input.c_str();

  for (size_t i = 0; text[i] != '\0'; ++i) {
    const char c = text[i];
    const bool possibleStart = isdigit(static_cast<unsigned char>(c)) ||
                               c == '+' || c == '-' || c == '.';
    if (!possibleStart) continue;

    char* end = nullptr;
    const float value = strtof(text + i, &end);
    if (end != text + i && isfinite(value)) {
      outValue = value;
      return true;
    }
  }

  return false;
}

JsonVariant getByPath(JsonVariant root, const String& path) {
  if (root.isNull() || path.isEmpty()) return JsonVariant();

  JsonVariant current = root;
  int start = 0;

  while (start < static_cast<int>(path.length())) {
    const int dot = path.indexOf('.', start);
    const int slash = path.indexOf('/', start);

    int end = -1;
    if (dot < 0) {
      end = slash;
    } else if (slash < 0) {
      end = dot;
    } else {
      end = dot < slash ? dot : slash;
    }

    String segment = end < 0 ? path.substring(start) : path.substring(start, end);
    segment.trim();

    if (!segment.isEmpty()) {
      if (current.is<JsonObject>()) {
        current = current.as<JsonObject>()[segment];
      } else if (current.is<JsonArray>()) {
        char* indexEnd = nullptr;
        const long index = strtol(segment.c_str(), &indexEnd, 10);
        if (indexEnd == segment.c_str() || *indexEnd != '\0' || index < 0) {
          return JsonVariant();
        }

        JsonArray array = current.as<JsonArray>();
        if (static_cast<size_t>(index) >= array.size()) {
          return JsonVariant();
        }
        current = array[static_cast<size_t>(index)];
      } else {
        return JsonVariant();
      }

      if (current.isNull()) return JsonVariant();
    }

    start = end < 0 ? static_cast<int>(path.length()) : end + 1;
  }

  return current;
}

bool variantToTemperature(JsonVariant value, float& outValue) {
  if (value.isNull() || value.is<bool>()) return false;

  if (value.is<float>() || value.is<double>() ||
      value.is<int>() || value.is<unsigned int>() ||
      value.is<long>() || value.is<unsigned long>()) {
    const float converted = value.as<float>();
    if (!isfinite(converted)) return false;
    outValue = converted;
    return true;
  }

  if (value.is<const char*>() || value.is<String>()) {
    return parseTempC(value.as<String>(), outValue);
  }

  return false;
}

}  // namespace

bool parseTempC(const String& payload, float& outC) {
  String text = payload;
  text.trim();
  if (text.isEmpty()) return false;

  if (parseFloatStrict(text, outC)) {
    return true;
  }

  // JSON is handled by tempParseFromPayload(). Avoid accidentally reading a
  // number from a JSON key or unrelated JSON field here.
  const char first = text[0];
  if (first == '{' || first == '[' || first == '"') {
    return false;
  }

  return parseFirstNumericToken(text, outC);
}

bool tempParseFromPayload(const String& payload,
                          const String& jsonKey,
                          float& outTempC) {
  String text = payload;
  text.trim();
  if (text.isEmpty()) return false;

  const char first = text[0];
  const bool looksLikeJson = first == '{' || first == '[' || first == '"';

  if (!looksLikeJson) {
    return parseTempC(text, outTempC);
  }

  DynamicJsonDocument document(768);
  const DeserializationError error = deserializeJson(document, text);
  if (error) {
    return false;
  }

  JsonVariant root = document.as<JsonVariant>();

  // A JSON root can itself be a number or numeric string.
  if (variantToTemperature(root, outTempC)) {
    return true;
  }

  if (!jsonKey.isEmpty()) {
    JsonVariant requested = getByPath(root, jsonKey);
    return variantToTemperature(requested, outTempC);
  }

  static const char* const commonKeys[] = {
      "tempC", "temperature", "temp", "t", "value"};

  for (const char* key : commonKeys) {
    JsonVariant candidate = getByPath(root, String(key));
    if (variantToTemperature(candidate, outTempC)) {
      return true;
    }
  }

  return false;
}
