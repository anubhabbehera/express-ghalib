/**
 * storage.cpp — LittleFS mount. See storage.h.
 */
#include "storage.h"
#include <Arduino.h>
#include <LittleFS.h>

bool storage_init() {
  // 4th arg is the partition label — ours is "littlefs" (see partitions CSV).
  if (!LittleFS.begin(true /*format on fail*/, "/littlefs", 8, "littlefs")) {
    Serial.println("[FS] mount FAILED");
    return false;
  }
  if (!LittleFS.exists("/notes")) LittleFS.mkdir("/notes");
  Serial.printf("[FS] mounted: %u / %u bytes used\n",
                (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  return true;
}
