/**
 * storage.h — LittleFS mount for app data (notes, config, templates).
 * Uses the "littlefs" partition from partitions_16MB.csv.
 */
#pragma once

// Mount LittleFS (formatting on first boot) and ensure the app dirs exist.
// Returns false if the filesystem can't be mounted.
bool storage_init();

// Mount the microSD card at /sdcard (SDMMC 1-bit, CLK38/CMD21/D0 39).
// Idempotent — safe to call from any app that needs the card (music, backups).
bool storage_sd_mount();

// Unmount the SD VFS so the raw card can be handed to USB-MSC (Files app).
// storage_sd_mount() works again afterwards.
void storage_sd_unmount();
