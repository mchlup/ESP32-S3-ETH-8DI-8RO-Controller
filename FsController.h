#pragma once
#include <Arduino.h>

bool fsInit();
bool fsIsReady();
bool fsWriteAtomicKeepBak(const char* path, const String& data, const char* bakPath, bool keepBak);
void fsLock();
void fsUnlock();
