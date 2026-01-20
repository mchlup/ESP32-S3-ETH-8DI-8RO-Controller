#include "Log.h"

namespace {
    LogLevel s_logLevel = LogLevel::Info;
}

void logSetLevel(LogLevel level) {
    s_logLevel = level;
}

LogLevel logGetLevel() {
    return s_logLevel;
}

bool logShould(LogLevel level) {
    return static_cast<uint8_t>(level) <= static_cast<uint8_t>(s_logLevel);
}

void logPrint(LogLevel level, const char* tag, const char* fmt, ...) {
    if (!logShould(level)) return;
    if (!fmt) return;

    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Serial.printf("[%s] %s\n", tag ? tag : "?", buf);
}
