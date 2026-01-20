#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

struct ValidationIssue {
    String code;
    String path;
    String message;
    bool fatal = false;
};

struct ValidationResult {
    bool ok = true;
    bool corrected = false;
    std::vector<ValidationIssue> issues;

    void addIssue(const String& code, const String& path, const String& message, bool fatal = false);
};

struct JsonDiagnostics {
    uint32_t parseErrors = 0;
    String lastError;
    size_t lastCapacity = 0;
    size_t lastUsage = 0;
};

const JsonDiagnostics& jsonGetDiagnostics();

bool loadJsonFromFile(const char* path, DynamicJsonDocument& doc, String& err);
bool saveJsonToFileAtomic(const char* path, const JsonDocument& doc, String& err);
bool parseJsonBody(Stream& client, DynamicJsonDocument& doc, size_t maxBytes, String& err);
bool parseJsonBody(const String& body, DynamicJsonDocument& doc, size_t maxBytes, String& err);
void applyDefaultsAndValidate(JsonDocument& doc, ValidationResult& out);
