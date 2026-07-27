/**
 * recur.h — recurring-event grammar + occurrence math (M8).
 *
 * Spec grammar (PocketMage-compatible, case-insensitive):
 *   no / (empty)      one-off event (canonical form: "")
 *   daily             every day
 *   weekly mo,we      weekly on the listed days (mo tu we th fr sa su);
 *                     "weekly" with no days = the anchor date's weekday
 *   monthly 15        monthly on day-of-month 15
 *   monthly 2tu       monthly on the 2nd Tuesday (1-5 + two-letter day)
 *   yearly apr22      yearly on April 22
 *
 * Events store the spec on line 1 after the datetime: "YYYY-MM-DD HH:MM|spec".
 * The datetime is the ANCHOR (first occurrence + the recurring time of day);
 * nothing recurs before it.
 */
#pragma once
#include <Arduino.h>

// Parse + re-serialize a user-typed spec to canonical form ("" = none/invalid).
String recur_normalize(const String& raw);

// Does an event anchored on anchor_date ("YYYY-MM-DD") with `spec` occur on
// `date`? False for dates before the anchor and for empty/invalid specs.
bool recur_on(const String& spec, const String& anchor_date, const String& date);

// First occurrence strictly after `after` ("YYYY-MM-DD HH:MM"), carrying the
// anchor's HH:MM. Searches ~2 years ahead; "" if no match / invalid spec.
String recur_next(const String& spec, const String& anchor_dt,
                  const String& after);
