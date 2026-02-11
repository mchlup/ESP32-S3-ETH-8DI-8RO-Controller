#include "FsController.h"

#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "Log.h"

namespace {
  SemaphoreHandle_t g_fsMtx = nullptr;
  bool g_inited = false;
}

bool fsInit() {
  if (!g_fsMtx) g_fsMtx = xSemaphoreCreateMutex();
  if (g_inited) return LittleFS.begin(); // idempotent

  g_inited = true;
  bool ok = LittleFS.begin(true);
  LOGI("LittleFS begin: %s", ok ? "ok" : "fail");
  return ok;
}

void fsLock() {
  if (!g_fsMtx) g_fsMtx = xSemaphoreCreateMutex();
  xSemaphoreTake(g_fsMtx, portMAX_DELAY);
}

void fsUnlock() {
  if (!g_fsMtx) return;
  xSemaphoreGive(g_fsMtx);
}

bool fsReadTextFile(const char* path, String& out) {
  out = "";
  if (!path) return false;
  fsLock();
  File f = LittleFS.open(path, "r");
  if (!f) {
    fsUnlock();
    return false;
  }
  out = f.readString();
  f.close();
  fsUnlock();
  return true;
}

bool fsWriteTextFile(const char* path, const String& data) {
  if (!path) return false;
  fsLock();
  File f = LittleFS.open(path, "w");
  if (!f) {
    fsUnlock();
    return false;
  }
  f.print(data);
  f.close();
  fsUnlock();
  return true;
}
