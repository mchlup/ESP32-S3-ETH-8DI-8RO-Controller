#include "EventLog.h"

namespace {
  struct Entry {
    uint32_t ms = 0;
    char source[20] = {0};
    char event[28] = {0};
    char detail[72] = {0};
    char level[8] = {0};
  };
  constexpr size_t kCap = 96;
  Entry g_entries[kCap];
  size_t g_head = 0;
  size_t g_count = 0;
  void cpy(char* dst, size_t n, const char* src){ if(!dst||!n) return; snprintf(dst,n,"%s", src?src:""); }
}
namespace EventLog {
  void begin() { clear(); }
  void clear() { for (size_t i=0;i<kCap;i++) g_entries[i]=Entry{}; g_head=0; g_count=0; }
  void record(const char* source, const char* event, const char* detail, const char* level) {
    Entry &e = g_entries[g_head];
    e.ms = millis();
    cpy(e.source,sizeof(e.source),source?source:"system");
    cpy(e.event,sizeof(e.event),event?event:"event");
    cpy(e.detail,sizeof(e.detail),detail?detail:"");
    cpy(e.level,sizeof(e.level),level?level:"info");
    g_head = (g_head + 1) % kCap;
    if (g_count < kCap) ++g_count;
  }
  void fillJson(JsonArray out, size_t maxItems) {
    const size_t count = (g_count < maxItems) ? g_count : maxItems;
    const size_t start = (g_head + kCap - count) % kCap;
    for (size_t i=0;i<count;i++) {
      const Entry &e = g_entries[(start + i) % kCap];
      if (!e.ms) continue;
      JsonObject it = out.createNestedObject();
      it["ms"] = e.ms;
      it["source"] = e.source;
      it["event"] = e.event;
      it["detail"] = e.detail;
      it["level"] = e.level;
    }
  }
  String toJson(size_t maxItems) {
    DynamicJsonDocument doc(8192);
    doc["ok"] = true;
    JsonArray arr = doc.createNestedArray("items");
    fillJson(arr, maxItems);
    String out; serializeJson(doc, out); return out;
  }
}
