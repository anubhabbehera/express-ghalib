/**
 * journal.h — date-keyed journal (M9, PocketMage-inspired).
 *
 * One entry per day at /journal/YYYYMMDD.txt (body only — the date is the key).
 * List shows a "days written" total + current streak; T/Enter opens today,
 * typing "jan 1" / "20260101" in the jump box opens/creates that day; A
 * archives all entries to the SD card (/export/journal/).
 */
#pragma once

// Open the Journal app (entry list). Registers its launcher leave hook.
void journal_open();
