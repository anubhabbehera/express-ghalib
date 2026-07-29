/**
 * reader.h — paginated .txt book reader from SD (M11, PocketMage-inspired).
 *
 * Books live in /sdcard/books (load them via the Files app's USB transfer).
 * Ideal use of the reflective panel: a static page per redraw. Reading
 * position is saved per book (LittleFS) and restored on reopen; the S/M/L
 * font bar is shared with the Notes editor (config text-size pref).
 */
#pragma once

// Open the Reader app (book list). Registers its launcher leave hook.
void reader_open();
