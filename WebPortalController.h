#pragma once

#include <Arduino.h>

#if defined(FEATURE_WEBPORTAL)

void webPortalInit();
void webPortalLoop();

#else

inline void webPortalInit() {}
inline void webPortalLoop() {}

#endif
