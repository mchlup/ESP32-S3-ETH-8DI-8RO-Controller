#include "FsController.h"
#include <LittleFS.h>
#include <freertos/semphr.h>

static bool s_fsReady = false;
static SemaphoreHandle_t s_fsMutex = nullptr;

bool fsInit() {
    if (s_fsReady) return true;
    bool formatted = false;
    if (!LittleFS.begin()) {
        Serial.println(F("[FS] LittleFS mount failed, trying format..."));
        if (!LittleFS.format()) {
            s_fsReady = false;
            Serial.println(F("[FS] LittleFS format failed."));
            return false;
        }
        formatted = true;
        if (!LittleFS.begin()) {
            s_fsReady = false;
            Serial.println(F("[FS] LittleFS mount failed after format."));
            return false;
        }
    }
    if (!s_fsMutex) {
        s_fsMutex = xSemaphoreCreateMutex();
        if (!s_fsMutex) {
            Serial.println(F("[FS] Mutex create failed."));
        }
    }
    s_fsReady = true;
    if (formatted) {
        Serial.println(F("[FS] LittleFS formatted."));
    }
    Serial.println(F("[FS] LittleFS mounted."));
    return true;
}

bool fsIsReady() {
    return s_fsReady;
}

void fsLock() {
    if (s_fsMutex) {
        xSemaphoreTake(s_fsMutex, portMAX_DELAY);
    }
}

void fsUnlock() {
    if (s_fsMutex) {
        xSemaphoreGive(s_fsMutex);
    }
}

bool fsWriteAtomicKeepBak(const char* path, const String& data, const char* bakPath, bool keepBak) {
    if (!fsIsReady()) return false;

    fsLock();

    String tmpPath = String(path) + ".tmp";
    File f = LittleFS.open(tmpPath, "w");
    if (!f) {
        fsUnlock();
        return false;
    }

    const size_t written = f.print(data);
    f.flush();
    f.close();

    if (written != data.length()) {
        LittleFS.remove(tmpPath);
        fsUnlock();
        return false;
    }

    if (LittleFS.exists(bakPath)) LittleFS.remove(bakPath);
    if (LittleFS.exists(path)) {
        if (!LittleFS.rename(path, bakPath)) {
            LittleFS.remove(tmpPath);
            fsUnlock();
            return false;
        }
    }

    if (!LittleFS.rename(tmpPath, path)) {
        if (LittleFS.exists(bakPath)) LittleFS.rename(bakPath, path);
        LittleFS.remove(tmpPath);
        fsUnlock();
        return false;
    }

    if (!keepBak && LittleFS.exists(bakPath)) LittleFS.remove(bakPath);

    fsUnlock();
    return true;
}
