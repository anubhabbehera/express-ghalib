/**
 * files.h — FileWiz file manager (M10, PocketMage-inspired).
 *
 * Browse LittleFS (flash) + the SD card, open text files in a simple editor,
 * rename/delete, and reopen recent files with the 0-9 digit keys. The USB
 * transfer mode (SD card exposed to a PC as USB-MSC) lives here too, as a row
 * on the root screen (the launcher grid is full at 8 tiles).
 */
#pragma once

// Open the Files app (volume picker + recents). Registers its leave hook.
void files_open();
