/**
 * tasks.h — Tasks (to-do) app, M8. PocketMage-inspired.
 *
 * A flat checklist with optional due dates: undone tasks sorted by due date
 * (undated last), done tasks at the bottom. Enter toggles done, E edits,
 * Del deletes, C clears all done. Storage: /tasks.txt on LittleFS, one task
 * per line: "<0|1>|<YYYY-MM-DD or empty>|<title>".
 */
#pragma once
#include <Arduino.h>

// Open the Tasks app (list + editor).
void tasks_open();

// Fill titles[] with up to `max` OPEN tasks due on `date` ("YYYY-MM-DD") or
// overdue relative to it; returns the count. Used by the calendar day agenda
// and the standby dashboard.
int tasks_due_on(const String& date, String* titles, int max);
