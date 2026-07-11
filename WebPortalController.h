#pragma once

#include <Arduino.h>

#if defined(FEATURE_WEBPORTAL)

void webPortalInit();
void webPortalLoop();
void webPortalBackgroundService();

#else

inline void webPortalInit() {}
inline void webPortalLoop() {}
inline void webPortalBackgroundService() {}

#endif
