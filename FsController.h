#pragma once

#include <Arduino.h>

bool fsInit();

// LittleFS mutex guards (WebServer + MQTT + logic config may access FS).
void fsLock();
void fsUnlock();

bool fsReadTextFile(const char* path, String& out);
bool fsWriteTextFile(const char* path, const String& data);
