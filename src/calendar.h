/**
 * calendar.h — Calendar app: a chronologically-sorted agenda of local events,
 * plus a simple editor. Entered from the launcher's Calendar tile.
 *
 * Storage: one file per event at /events/<id>.txt. Line 1 is the start time as
 * "YYYY-MM-DD HH:MM" (ISO-ish so it sorts chronologically as text); line 2+ is
 * the title. Edited via a two-line textarea, mirroring the Notes editor.
 */
#pragma once

// Open the Calendar app (builds the agenda screen and shows it).
void calendar_open();
