/**
 * notes.h — Notes app: a list of notes backed by LittleFS, plus a full-screen
 * text editor. Entered from the launcher's Notes tile.
 *
 * Storage: one file per note at /notes/<id>.txt; the first line is the title.
 */
#pragma once

// Open the Notes app (builds the note-list screen and shows it).
void notes_open();
