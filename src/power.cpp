/**
 * power.cpp — M7 power management. See power.h for the sleep model.
 *
 * Design note: the PCF85063's INT line is not routed to any ESP32 GPIO on this
 * board (verified against the whole waveshareteam/ESP32-S3-RLCD-4.2 demo repo:
 * no example, the factory program, nor the ESPHome configs ever touch it), so
 * the ESP32's own deep-sleep timer stands in for the RTC hardware alarm. The
 * PCF85063 stays the wall-clock source of truth: every wake re-reads it, so
 * ESP32 timer drift never accumulates.
 *
 * Deep sleep resets the chip, so "waking" is a reboot. Two boot paths:
 *  - dashboard refresh (timer wake, no reminder due): display + FS + RTC are
 *    already up when power_early_boot() runs -> redraw clock, sleep again.
 *    Nothing else (BLE, audio, apps) is initialised — this path is ~0.5 s.
 *  - everything else: full boot; power_init() seeds the reminder scheduler
 *    with the pre-sleep timestamp so alerts missed while asleep fire normally.
 */
#include "power.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <lvgl.h>
#include <time.h>

#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_system.h"

#include "config.h"
#include "files.h"
#include "launcher.h"
#include "music.h"
#include "reminders.h"
#include "rtc.h"
#include "st7305.h"
#include "tasks.h"

namespace {

constexpr int PIN_KEY  = 18;  // side buttons, active-low (EXT1 wake sources)
constexpr int PIN_BOOT = 0;

constexpr int      CRIT_BATT_PCT   = 3;      // below this: sleep, no minute tick
constexpr uint32_t DASH_TICK_S     = 60;     // dashboard clock refresh period
constexpr uint32_t REMIND_MARGIN_S = 3;      // wake this much AFTER an event is due

// Survives deep sleep. g_sleep_at = local "YYYY-MM-DD HH:MM" when we went to
// sleep ("" = cold boot); g_dash = we are in dashboard sleep cycles.
RTC_DATA_ATTR char    g_sleep_at[17];
RTC_DATA_ATTR uint8_t g_dash;

// Tiny boot journal on LittleFS: dashboard-refresh boots live and die before
// USB-CDC enumerates (~2 s), so serial alone can't show what sleep/wake did.
// One line per event; the log is printed (and visible) on every full boot.
void pwr_log(const char* msg) {
  File f = LittleFS.open("/pwr.log", "a");
  if (!f) return;
  if (f.size() > 2048) {  // bounded: start over rather than grow forever
    f.close();
    LittleFS.remove("/pwr.log");
    f = LittleFS.open("/pwr.log", "a");
    if (!f) return;
  }
  char now[17];
  rtc_local_datetime(now);
  f.printf("%s r%d w%d %s\n", now, (int)esp_reset_reason(),
           (int)esp_sleep_get_wakeup_cause(), msg);
  f.close();
}

time_t parse_local(const char* s) {
  struct tm t = {};
  if (sscanf(s, "%d-%d-%d %d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday,
             &t.tm_hour, &t.tm_min) < 5)
    return 0;
  t.tm_year -= 1900;
  t.tm_mon -= 1;
  return mktime(&t);  // TZ=UTC0 (rtc_init): consistent for differences
}

[[noreturn]] void sleep_now(int batt_pct);  // fwd (defined below)

// External power? A USB host attached is the reliable signal: SOF-based
// HWCDC::isPlugged() on USB-Serial-JTAG builds, TinyUSB's tud_mounted() on
// USB-OTG builds (M10 MSC). The battery-sense fallback (>= 4.10 V ->
// pct == -1) additionally covers dumb wall chargers, which enumerate no host
// but do pin the charge rail high.
#if !ARDUINO_USB_MODE
extern "C" bool tud_mounted(void);
#endif

bool usb_host_attached() {
#if ARDUINO_USB_MODE
  return HWCDC::isPlugged();
#else
  return tud_mounted();
#endif
}

bool on_external_power(int batt_pct) {
  return usb_host_attached() || batt_pct < 0;
}

// ---------------------------------------------------------------------------
// Standby dashboard. Two modes:
//  - on battery: drawn once, then the ESP32 deep-sleeps behind it (reflective
//    LCD + ST7305 low-power mode = the image stays at ~zero power);
//  - on external power (battery sense pinned >= 4.10 V): the device STAYS
//    AWAKE as a live desk clock (PocketMage's "Now-Later" idea) — refreshed
//    every 30 s, any key returns to the launcher, and pulling the plug drops
//    it into the real deep-sleep path on the next tick. This also keeps the
//    USB port alive, which is what makes reflashing during development sane.
// ---------------------------------------------------------------------------
lv_obj_t*   g_dash_scr   = nullptr;
lv_obj_t*   g_dash_clk   = nullptr;
lv_obj_t*   g_dash_date  = nullptr;
lv_obj_t*   g_dash_bat   = nullptr;
lv_obj_t*   g_dash_ag    = nullptr;
lv_timer_t* g_dash_timer = nullptr;
bool        g_dash_awake = false;   // awake desk-clock mode is active

String agenda_text(const char* now) {
  String dts[5], titles[5];
  const int n = reminders_upcoming(dts, titles, 5);
  String txt;
  int shown = 0;
  const String today = String(now).substring(0, 10);
  for (int i = 0; i < n && shown < 4; i++) {
    if (dts[i].substring(0, 10) == today) {
      txt += dts[i].substring(11) + "   " + titles[i] + "\n";
      shown++;
    }
  }
  if (!shown && n > 0)  // nothing left today -> show what's next
    txt = "next: " + dts[0].substring(5, 10) + " " + dts[0].substring(11) +
          "   " + titles[0] + "\n";
  String due[3];
  const int nd = tasks_due_on(today, due, 3);
  for (int i = 0; i < nd; i++) txt += "task: " + due[i] + "\n";
  if (txt.isEmpty()) txt = "no upcoming events";
  return txt;
}

void dashboard_show(int batt_pct) {
  lv_obj_t* scr = lv_obj_create(nullptr);
  g_dash_scr = scr;
  lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  char now[17];
  rtc_local_datetime(now);  // "YYYY-MM-DD HH:MM"
  struct tm t = {};
  const time_t tt = parse_local(now);
  localtime_r(&tt, &t);

  // Big clock + date.
  g_dash_clk = lv_label_create(scr);
  lv_obj_set_style_text_font(g_dash_clk, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(g_dash_clk, lv_color_black(), 0);
  lv_label_set_text(g_dash_clk, now + 11);  // "HH:MM"
  lv_obj_align(g_dash_clk, LV_ALIGN_TOP_MID, 0, 34);

  static const char* kWday[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday"};
  static const char* kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char date[40];
  snprintf(date, sizeof date, "%s, %s %d", kWday[t.tm_wday], kMon[t.tm_mon],
           t.tm_mday);
  g_dash_date = lv_label_create(scr);
  lv_obj_set_style_text_font(g_dash_date, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(g_dash_date, lv_color_black(), 0);
  lv_label_set_text(g_dash_date, date);
  lv_obj_align(g_dash_date, LV_ALIGN_TOP_MID, 0, 96);

  // Battery, top-right.
  g_dash_bat = lv_label_create(scr);
  lv_obj_set_style_text_color(g_dash_bat, lv_color_black(), 0);
  if (batt_pct < 0) {
    lv_label_set_text(g_dash_bat, LV_SYMBOL_CHARGE);
  } else {
    char b[8];
    snprintf(b, sizeof b, "%d%%", batt_pct);
    lv_label_set_text(g_dash_bat, b);
  }
  lv_obj_align(g_dash_bat, LV_ALIGN_TOP_RIGHT, -10, 8);

  // Divider + agenda: today's remaining events, else the next upcoming one.
  static lv_point_t seg[2] = {{60, 0}, {(lv_coord_t)(ST7305_W - 60), 0}};
  lv_obj_t* ln = lv_line_create(scr);
  lv_line_set_points(ln, seg, 2);
  lv_obj_set_style_line_width(ln, 1, 0);
  lv_obj_set_style_line_color(ln, lv_color_black(), 0);
  lv_obj_set_pos(ln, 0, 134);

  g_dash_ag = lv_label_create(scr);
  lv_obj_set_style_text_font(g_dash_ag, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(g_dash_ag, lv_color_black(), 0);
  lv_obj_set_style_text_align(g_dash_ag, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(g_dash_ag, agenda_text(now).c_str());
  lv_obj_align(g_dash_ag, LV_ALIGN_TOP_MID, 0, 150);

  lv_obj_t* foot = lv_label_create(scr);
  lv_obj_set_style_text_color(foot, lv_color_black(), 0);
  lv_label_set_text(foot, batt_pct >= 0 && batt_pct <= CRIT_BATT_PCT
                              ? "battery empty - charge me"
                          : batt_pct < 0 ? "on power - any key to return"
                                         : "press KEY to wake");
  lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);

  lv_scr_load(scr);
  lv_refr_now(nullptr);  // paint + flush before we power down / settle
}

// --- awake desk-clock mode (external power) --------------------------------
void dash_leave() {  // launcher teardown hook (any key / BOOT went home)
  if (g_dash_timer) { lv_timer_del(g_dash_timer); g_dash_timer = nullptr; }
  if (g_dash_scr) { lv_obj_del_async(g_dash_scr); g_dash_scr = nullptr; }
  g_dash_clk = g_dash_date = g_dash_bat = g_dash_ag = nullptr;
  g_dash_awake = false;
}

void dash_key_cb(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_KEY ||
      lv_event_get_code(e) == LV_EVENT_CLICKED)
    launcher_go_home();
}

void dash_refresh_cb(lv_timer_t*) {
  const int b = power_battery_pct();
  char now[17];
  rtc_local_datetime(now);
  if (g_dash_clk) lv_label_set_text(g_dash_clk, now + 11);
  if (g_dash_ag) lv_label_set_text(g_dash_ag, agenda_text(now).c_str());
  if (!on_external_power(b)) {
    // Plug pulled: drop into the real battery standby (deep sleep).
    lv_timer_del(g_dash_timer);
    g_dash_timer = nullptr;
    g_dash_awake = false;
    char pb[8];
    snprintf(pb, sizeof pb, "%d%%", b);
    if (g_dash_bat) lv_label_set_text(g_dash_bat, pb);
    lv_refr_now(nullptr);
    sleep_now(b);  // never returns
  }
}

void awake_dashboard() {
  g_dash_awake = true;
  dashboard_show(-1);
  // A focused catcher object so any keyboard key returns to the launcher.
  lv_group_remove_all_objs(lv_group_get_default());
  lv_obj_t* c = lv_btn_create(g_dash_scr);
  lv_obj_set_size(c, 1, 1);
  lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_shadow_width(c, 0, 0);
  lv_obj_add_event_cb(c, dash_key_cb, LV_EVENT_KEY, nullptr);
  lv_obj_add_event_cb(c, dash_key_cb, LV_EVENT_CLICKED, nullptr);
  lv_group_add_obj(lv_group_get_default(), c);
  lv_group_focus_obj(c);
  launcher_set_leave_hook(dash_leave);
  g_dash_timer = lv_timer_create(dash_refresh_cb, 30000, nullptr);
  Serial.println("[PWR] awake dashboard (external power)");
}

// Arm wake sources and enter deep sleep. Never returns.
[[noreturn]] void sleep_now(int batt_pct) {
  char now[17];
  rtc_local_datetime(now);
  memcpy(g_sleep_at, now, sizeof g_sleep_at);
  g_dash = 1;

  // Timer wake: the minute tick for the dashboard clock, or the next reminder
  // if that lands sooner. On critical battery skip the minute tick entirely
  // (the clock freezes; reminders still wake us — they're rare and audible).
  uint64_t wake_s = DASH_TICK_S;
  const bool crit = batt_pct >= 0 && batt_pct <= CRIT_BATT_PCT;
  if (crit) wake_s = 0;
  const String next = reminders_next_dt();
  if (!next.isEmpty()) {
    const long due = (long)(parse_local(next.c_str()) - parse_local(now)) +
                     REMIND_MARGIN_S;
    const uint64_t due_s = due < (long)REMIND_MARGIN_S ? REMIND_MARGIN_S
                                                       : (uint64_t)due;
    if (wake_s == 0 || due_s < wake_s) wake_s = due_s;
  }
  if (wake_s) esp_sleep_enable_timer_wakeup(wake_s * 1000000ULL);

  // KEY (active-low) as EXT1 wake; hold its pull-up through sleep. BOOT is
  // deliberately NOT a wake source: GPIO0 is a strapping pin, and a press that
  // is still low when the chip resets out of deep sleep would strap the ROM
  // into download mode instead of booting.
  rtc_gpio_pullup_en((gpio_num_t)PIN_KEY);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_KEY);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_KEY, ESP_EXT1_WAKEUP_ANY_LOW);

  st7305_low_power();  // panel keeps the image at its ~1 Hz self-refresh
  char sl[32];
  snprintf(sl, sizeof sl, "sleep wake_s=%llu", (unsigned long long)wake_s);
  pwr_log(sl);
  Serial.printf("[PWR] deep sleep (wake in %llus or on KEY)\n",
                (unsigned long long)wake_s);
  Serial.flush();
  esp_deep_sleep_start();
  abort();  // not reached
}

// The idle watchdog (1 s lv_timer on the full-boot path).
// NOTE: runtime CPU-frequency scaling was tried here (160 MHz base / 240 MHz
// for MP3) and HANGS the calling task: setCpuFrequencyMhz() is not safe while
// the BLE controller is active (verified on HW — the call never returns). The
// chip stays at 240 MHz while awake; deep sleep is the power story.
void idle_cb(lv_timer_t*) {
  if (g_dash_awake) return;  // desk-clock mode owns the screen already

  const int cfg = config_get_sleep_secs();
  if (cfg <= 0) return;  // sleep disabled

  // While something is genuinely running, keep resetting the idle clock so a
  // finished song / dismissed alert starts a fresh timeout instead of an
  // instant sleep.
  if (music_playing() || reminders_alert_active() ||
      reminders_snooze_pending() || files_usb_active()) {
    lv_disp_trig_activity(nullptr);
    return;
  }

  if (lv_disp_get_inactive_time(nullptr) > (uint32_t)cfg * 1000) {
    const int b = power_battery_pct();
    Serial.printf("[PWR] idle timeout -> standby dashboard (batt=%d usb=%d)\n",
                  b, (int)usb_host_attached());
    if (on_external_power(b)) {
      awake_dashboard();  // external power: stay awake as a desk clock
    } else {
      dashboard_show(b);
      sleep_now(b);
    }
  }
}

}  // namespace

int power_battery_pct() {
  // 18650 sense on GPIO4 (ADC1_CH3, /3 divider — Waveshare factory demo).
  // No VBUS/battery-detect pin exists, so >= 4.10 V means external power /
  // charging / full — callers show an icon for -1. 3.0 V = 0% .. 4.2 V = 100%.
  constexpr int   PIN_BATT    = 4;
  constexpr float BATT_ICON_V = 4.10f;
  static bool attenuated = false;
  if (!attenuated) {  // ~0..3.1 V range for the /3 sense
    analogSetPinAttenuation(PIN_BATT, ADC_11db);
    attenuated = true;
  }
  uint32_t sum = 0;
  constexpr int N = 16;
  for (int i = 0; i < N; i++) sum += analogReadMilliVolts(PIN_BATT);
  const float vbat = (sum / (float)N) * 3.0f / 1000.0f;
  if (vbat >= BATT_ICON_V) return -1;
  if (vbat < 3.0f) return 0;
  return (int)((vbat - 3.0f) / 1.2f * 100.0f);
}

void power_early_boot() {
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_TIMER && g_dash) {
    if (reminders_due_since(g_sleep_at)) {
      pwr_log("wake: reminder due -> full boot");
      return;  // full boot; power_init() replays the missed window
    }
    const int b = power_battery_pct();
    if (on_external_power(b)) {
      // Plugged in while sleeping: leave the sleep cycle, full boot (the idle
      // watchdog then brings up the awake desk clock instead).
      pwr_log("wake: on power -> full boot");
      return;
    }
    // Pure clock tick: redraw and go back down. ~0.5 s awake.
    pwr_log("wake: dash tick");
    dashboard_show(b);
    sleep_now(b);
  }
  pwr_log(cause == ESP_SLEEP_WAKEUP_EXT1 ? "wake: KEY -> full boot"
                                         : "boot (cold/other)");
}

void power_init() {
  if (g_sleep_at[0]) {
    // Replay reminders that came due while we slept (edge window rule fires
    // everything in (sleep_at, now] exactly once).
    reminders_seed_baseline(g_sleep_at);
    g_sleep_at[0] = 0;
    reminders_check_now();
  }
  g_dash = 0;
  lv_timer_create(idle_cb, 1000, nullptr);
  Serial.printf("[PWR] idle watchdog on (sleep after %d s; 0 = never)\n",
                config_get_sleep_secs());
  // Dump the boot journal — the only window into dashboard-refresh boots.
  File f = LittleFS.open("/pwr.log", "r");
  if (f) {
    Serial.println("[PWR] journal:");
    while (f.available()) {
      String line = f.readStringUntil('\n');
      if (line.length()) Serial.println("  " + line);
    }
    f.close();
  }
}
