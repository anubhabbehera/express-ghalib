/**
 * storage.cpp — LittleFS mount + shared SD (SDMMC 1-bit) mount. See storage.h.
 */
#include "storage.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <SD_MMC.h>

bool storage_init() {
  // 4th arg is the partition label — ours is "littlefs" (see partitions CSV).
  if (!LittleFS.begin(true /*format on fail*/, "/littlefs", 8, "littlefs")) {
    Serial.println("[FS] mount FAILED");
    return false;
  }
  if (!LittleFS.exists("/notes")) LittleFS.mkdir("/notes");
  if (!LittleFS.exists("/events")) LittleFS.mkdir("/events");
  if (!LittleFS.exists("/journal")) LittleFS.mkdir("/journal");
  Serial.printf("[FS] mounted: %u / %u bytes used\n",
                (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  return true;
}

bool storage_sd_mount() {
  static bool mounted = false;
  if (mounted) return true;
  SD_MMC.setPins(38 /*CLK*/, 21 /*CMD*/, 39 /*D0*/);
  if (!SD_MMC.begin("/sdcard", true /*1-bit*/, false /*no format*/)) {
    Serial.println("[FS] SD mount failed (card inserted? FAT32?)");
    return false;
  }
  mounted = true;
  Serial.printf("[FS] SD mounted: type=%d size=%lluMB\n", SD_MMC.cardType(),
                SD_MMC.cardSize() / (1024ULL * 1024ULL));
  return true;
}
