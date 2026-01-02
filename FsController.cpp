#include "FsController.h"
#include <LittleFS.h>

static bool s_fsReady = false;

bool fsInit() {
    if (s_fsReady) return true;
    if (!LittleFS.begin()) {
        s_fsReady = false;
        Serial.println(F("[FS] LittleFS mount failed."));
        return false;
    }
    s_fsReady = true;
    Serial.println(F("[FS] LittleFS mounted."));
    return true;
}

bool fsIsReady() {
    return s_fsReady;
}