/**
 * recur.cpp — recurring-event grammar + occurrence math. See recur.h.
 * TZ=UTC0 is set globally (rtc_init), so mktime() is a plain calendar
 * normalizer here — no DST surprises.
 */
#include "recur.h"

#include <time.h>

namespace {

const char* kD2[7] = {"su", "mo", "tu", "we", "th", "fr", "sa"};  // tm_wday
const char* kM3[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                       "jul", "aug", "sep", "oct", "nov", "dec"};

struct Spec {
  enum Type { NONE, DAILY, WEEKLY, MON_DOM, MON_NTH, YEARLY };
  Type type = NONE;
  uint8_t wmask = 0;      // WEEKLY: bit per tm_wday (0 = use anchor weekday)
  int dom = 0;            // MON_DOM
  int nth = 0, wday = 0;  // MON_NTH
  int mon = 0, day = 0;   // YEARLY
};

int wday_of(const String& s) {
  for (int i = 0; i < 7; i++)
    if (s == kD2[i]) return i;
  return -1;
}

Spec parse(const String& raw) {
  Spec sp;
  String s = raw;
  s.trim();
  s.toLowerCase();
  if (s.isEmpty() || s == "no" || s == "none") return sp;
  if (s == "daily") {
    sp.type = Spec::DAILY;
  } else if (s.startsWith("weekly")) {
    sp.type = Spec::WEEKLY;
    String rest = s.substring(6);
    rest.trim();
    while (rest.length() >= 2) {
      const int w = wday_of(rest.substring(0, 2));
      if (w >= 0) sp.wmask |= 1 << w;
      const int c = rest.indexOf(',');
      if (c < 0) break;
      rest = rest.substring(c + 1);
      rest.trim();
    }
  } else if (s.startsWith("monthly")) {
    String rest = s.substring(7);
    rest.trim();
    if (rest.length() >= 3 && isDigit(rest[0]) && !isDigit(rest[1])) {
      const int n = rest[0] - '0';
      const int w = wday_of(rest.substring(1, 3));
      if (n >= 1 && n <= 5 && w >= 0) {
        sp.type = Spec::MON_NTH;
        sp.nth = n;
        sp.wday = w;
      }
    } else {
      const int d = rest.toInt();
      if (d >= 1 && d <= 31) {
        sp.type = Spec::MON_DOM;
        sp.dom = d;
      }
    }
  } else if (s.startsWith("yearly")) {
    String rest = s.substring(6);
    rest.trim();
    if (rest.length() >= 4) {
      const String m3 = rest.substring(0, 3);
      for (int i = 0; i < 12; i++) {
        if (m3 == kM3[i]) {
          const int d = rest.substring(3).toInt();
          if (d >= 1 && d <= 31) {
            sp.type = Spec::YEARLY;
            sp.mon = i + 1;
            sp.day = d;
          }
        }
      }
    }
  }
  return sp;
}

// "YYYY-MM-DD" -> normalized tm (tm_wday filled). False on parse failure.
bool date_tm(const String& d, struct tm& t) {
  int y, m, dd;
  if (sscanf(d.c_str(), "%d-%d-%d", &y, &m, &dd) != 3) return false;
  t = {};
  t.tm_year = y - 1900;
  t.tm_mon = m - 1;
  t.tm_mday = dd;
  t.tm_hour = 12;
  return mktime(&t) != (time_t)-1;
}

bool spec_on(const Spec& sp, const String& anchor_date, const struct tm& t) {
  switch (sp.type) {
    case Spec::DAILY:
      return true;
    case Spec::WEEKLY: {
      uint8_t mask = sp.wmask;
      if (!mask) {
        struct tm a;
        if (!date_tm(anchor_date, a)) return false;
        mask = 1 << a.tm_wday;
      }
      return mask & (1 << t.tm_wday);
    }
    case Spec::MON_DOM:
      return t.tm_mday == sp.dom;
    case Spec::MON_NTH:
      return t.tm_wday == sp.wday && (t.tm_mday - 1) / 7 + 1 == sp.nth;
    case Spec::YEARLY:
      return t.tm_mon + 1 == sp.mon && t.tm_mday == sp.day;
    default:
      return false;
  }
}

}  // namespace

String recur_normalize(const String& raw) {
  const Spec sp = parse(raw);
  switch (sp.type) {
    case Spec::DAILY:
      return "daily";
    case Spec::WEEKLY: {
      if (!sp.wmask) return "weekly";
      String out = "weekly ";
      bool first = true;
      for (int i = 0; i < 7; i++)
        if (sp.wmask & (1 << i)) {
          if (!first) out += ",";
          out += kD2[i];
          first = false;
        }
      return out;
    }
    case Spec::MON_DOM:
      return "monthly " + String(sp.dom);
    case Spec::MON_NTH:
      return "monthly " + String(sp.nth) + kD2[sp.wday];
    case Spec::YEARLY:
      return "yearly " + String(kM3[sp.mon - 1]) + String(sp.day);
    default:
      return "";
  }
}

bool recur_on(const String& spec, const String& anchor_date,
              const String& date) {
  if (date < anchor_date) return false;
  const Spec sp = parse(spec);
  if (sp.type == Spec::NONE) return false;
  struct tm t;
  if (!date_tm(date, t)) return false;
  return spec_on(sp, anchor_date, t);
}

String recur_next(const String& spec, const String& anchor_dt,
                  const String& after) {
  if (anchor_dt.length() < 16) return "";
  const Spec sp = parse(spec);
  if (sp.type == Spec::NONE) return "";
  const String anchor_date = anchor_dt.substring(0, 10);
  const String hm = anchor_dt.substring(11, 16);

  String start = after.length() >= 10 ? after.substring(0, 10) : anchor_date;
  if (start < anchor_date) start = anchor_date;
  struct tm t;
  if (!date_tm(start, t)) return "";
  // ~2 years covers everything except "yearly feb29", which needs a leap cycle.
  const int horizon = sp.type == Spec::YEARLY ? 1500 : 750;
  for (int i = 0; i < horizon; i++) {
    char d[11];
    snprintf(d, sizeof d, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday);
    const String cand = String(d) + " " + hm;
    if (cand > after && String(d) >= anchor_date && spec_on(sp, anchor_date, t))
      return cand;
    t.tm_mday++;
    mktime(&t);
  }
  return "";
}
