#pragma once

#include <Arduino.h>
#include <stdarg.h>

enum class LogLevel : uint8_t {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3
};

void logSetLevel(LogLevel level);
LogLevel logGetLevel();
bool logShould(LogLevel level);
void logPrint(LogLevel level, const char* tag, const char* fmt, ...);

#define LOGE(...) logPrint(LogLevel::Error, "E", __VA_ARGS__)
#define LOGW(...) logPrint(LogLevel::Warn, "W", __VA_ARGS__)
#define LOGI(...) logPrint(LogLevel::Info, "I", __VA_ARGS__)
#define LOGD(...) logPrint(LogLevel::Debug, "D", __VA_ARGS__)
