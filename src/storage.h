/**
 * storage.h — LittleFS mount for app data (notes, config, templates).
 * Uses the "littlefs" partition from partitions_16MB.csv.
 */
#pragma once

// Mount LittleFS (formatting on first boot) and ensure the app dirs exist.
// Returns false if the filesystem can't be mounted.
bool storage_init();
