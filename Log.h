#pragma once

#include <Arduino.h>

// Minimal logging helpers.
// Header-only for Arduino builds.

inline void _logPrintPrefix(const char* lvl) {
  Serial.printf("[%lu][%s] ", (unsigned long)millis(), lvl);
}

inline void _logV(const char* lvl, const char* fmt, va_list ap) {
  _logPrintPrefix(lvl);
  Serial.vprintf(fmt, ap);
  Serial.println();
}

inline void _log(const char* lvl, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  _logV(lvl, fmt, ap);
  va_end(ap);
}

#ifndef LOGI
#define LOGI(...) _log("I", __VA_ARGS__)
#endif
#ifndef LOGW
#define LOGW(...) _log("W", __VA_ARGS__)
#endif
#ifndef LOGE
#define LOGE(...) _log("E", __VA_ARGS__)
#endif
