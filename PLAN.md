# express-ghalib — ESP32-S3 PDA

A pocket PDA in the spirit of [Orion PDA](https://orionpda.org/): a minimal,
keyboard-driven, paper-like personal device. Built on the
[Waveshare ESP32-S3-RLCD-4.2](https://docs.waveshare.com/ESP32-S3-RLCD-4.2).

## 1. Hardware baseline

| Part | Detail | Used for |
|---|---|---|
| SoC | ESP32-S3-WROOM-1-**N16R8**, dual-core LX7 @240MHz | Everything |
| Flash / PSRAM | 16 MB flash, 8 MB PSRAM, 512 KB SRAM | App code, LVGL framebuffer in PSRAM, file storage |
| Display | 4.2" reflective LCD (RLCD), **300×400**, no backlight, e-paper-like | UI (static, page-based) |
| Wireless | Wi-Fi 2.4GHz + **Bluetooth 5 LE only** | BLE keyboard (host), optional Wi-Fi time sync |
| Audio out | **ES8311** codec (I2S) → MX1.25 mono speaker header | Music player, reminder beeps |
| Audio in | ES7210 ADC + dual mic | (Out of scope for v1) |
| RTC | **PCF85063** + backup battery holder | Calendar clock, reminder alarms |
| Sensor | SHTC3 temp/humidity | (Optional status-bar widget) |
| Storage | microSD (TF) slot, FAT32 | Music files, note/journal archives |
| Buttons | KEY + BOOT (side), PWR | Wake / back / menu |
| Expansion | 2×8 2.54mm header, USB-C, UART, I2C, GPIO | Debug, future |

### Non-obvious hardware facts (design around these)
- **BT is LE-only.** A Classic-BT keyboard will never pair. Buy a **BLE HID** keyboard.
- **No touch panel.** Navigation = keyboard + 2 buttons. UI must be fully keyboard-drivable.
- **Reflective display** refreshes slower than TFT and is monochrome/limited-grayscale.
  Design static screens; avoid animation, partial-scroll, and color-dependent UI.
- **Mono audio** only (single speaker header). No headphone jack.

### Confirmed pin map (from Waveshare driver + board devicetree)
| Bus / signal | GPIO | Notes |
|---|---|---|
| LCD SPI SCLK | 11 | ST7305, 4-wire SPI |
| LCD SPI MOSI | 12 | |
| LCD CS | 40 | |
| LCD DC | 5 | |
| LCD RST | 41 | |
| LCD TE | 6 | tearing-effect (unused for now) |
| I2C SDA | 13 | shared: PCF85063 RTC + SHTC3 |
| I2C SCL | 14 | |

> Source: Waveshare official Arduino example `user_config.h` (`RLCD_*_PIN`).
> The ESPHome port's 39/38 for SCK/MOSI are WRONG for this board — verified on
> hardware (M0): 11/12 renders, 39/38 gives a blank panel.

- **Controller is ST7305 (Waveshare) / ST7306 (Zephyr naming)** — same family; ST7306
  adds 4-level grayscale. Driver runs 1-bit mono (works on both).
- **SPI clock kept low (~1–2 MHz):** Waveshare warns higher gives no benefit and can corrupt.
- **Panel is natively landscape 400×300** with a non-linear **2×4-block** pixel packing
  (Y inverted, MADCTL 0x48). Never send a plain horizontal bitmap. For a 300×400 portrait
  UI, use LVGL software rotation. Address window: col 0x12–0x2A, row 0x00–0xC7.
- I2S (ES8311) and SD (SDMMC) pins: resolve at M4 from the schematic/devicetree.

## 2. Recommended software stack

> **Framework decision: Arduino-ESP32 (core ≥3.0), not ESP-IDF.**
> Rationale from prior project history: ESP-IDF's Python toolchain repeatedly failed
> to install on this machine; the Arduino path reached working pixels reliably and
> Waveshare ships Arduino demos for this exact board. Revisit ESP-IDF only if we hit
> a wall Arduino can't clear (custom partitions, fine-grained BLE control, power tuning).

| Concern | Choice |
|---|---|
| Framework | Arduino-ESP32 core ≥ 3.0.x (S3 support) |
| UI toolkit | LVGL 8.3.x, configured for keypad/group navigation, monochrome/grayscale theme |
| Display driver | **ST7305** controller (1-bit mono, SPI). No drop-in Arduino/LVGL driver exists — port from Waveshare's demo + [`musicaJack/ST73xx_Reflective_Lcd`](https://github.com/musicaJack/ST73xx_Reflective_Lcd). LVGL runs in `LV_COLOR_DEPTH 1` mono mode (rounder + set_px bit-packing). |
| Audio | `ESP32-audioI2S` (Schreibfaul1) or ESP-ADF-style ES8311 driver for I2S MP3/WAV |
| BLE HID host | NimBLE-Arduino, HID-over-GATT (HOGP) **host** role |
| RTC | PCF85063 lib (I2C) — time + alarm IRQ |
| Filesystem | LittleFS (flash) for app data/config/templates; SD (FAT32) for music + archives |
| Time sync | `configTime()` NTP over Wi-Fi (optional; RTC is source of truth) |

### Key architectural decisions
1. **Keyboard is the primary input abstraction.** A single input layer maps BLE HID
   key reports → LVGL key events (`LV_KEY_NEXT/PREV/ENTER/ESC/...`) + printable chars.
   Physical KEY/BOOT buttons map to Back/Menu. Every app is driven through LVGL groups.
2. **App-launcher model.** A home screen lists apps (Notes, Calendar, Reminders, Music,
   Settings). Each app is a self-contained module with `open()/close()/handle_key()`.
3. **Storage split.** Small, frequently-written data (config, note index, templates,
   events) → LittleFS. Large/append data (music, journal exports) → SD card.
4. **Draw sparingly.** Batch UI updates; full-screen redraws only on screen change.
   Treat the RLCD like e-paper for redraw budgeting.

## 3. Data model (local, file-backed — no server)

```
/littlefs
  /config.json           # wifi creds, paired-keyboard MAC, prefs
  /templates/*.md        # journal/note templates (front-matter + body)
  /notes/index.json      # note metadata (id, title, template, mtime)
  /notes/<id>.md         # note bodies
  /events.json           # calendar events + reminder rules
/sd (FAT32)
  /music/*.mp3|*.wav     # playlist source
  /export/               # note/journal backups
```

Events schema (per entry): `{id, title, start, end, allDay, remindBeforeMin, repeat}`.
Reminders are derived from events with `remindBeforeMin`; the RTC alarm is set to the
*next* upcoming reminder, re-armed after each fire.

## 4. Milestones

### M0 — Toolchain + "hello pixels" (de-risk the display)
- Arduino-ESP32 core installed, board flashes over USB-C.
- Bring up Waveshare RLCD demo → confirm 300×400 output, refresh behavior, grayscale depth.
- Wrap the panel in an LVGL display driver; render a static test screen.
- **Exit:** text renders crisply; we know the real color depth + redraw cost.

### M0.5 — Sensor + Wi-Fi mock screen (de-risk I2C + Wi-Fi) ✅
- SHTC3 temp/humidity over I2C (SDA=13, SCL=14, addr 0x70); live on-panel.
- Async Wi-Fi SSID scan → on-screen list with RSSI, re-scans every ~15s.
- **Exit:** panel shows live temp/humidity that reacts to breath, plus nearby SSIDs.
- Lessons learned: (1) Arduino `Wire` needs explicit `INPUT_PULLUP` on SDA/SCL or
  the SHTC3 won't ACK; (2) LVGL's `lv_label_set_text_fmt` drops `%f` unless
  `LV_SPRINTF_USE_FLOAT` is set — format floats with newlib `snprintf` instead.
  I2C bus map: 0x18 ES8311, 0x40 ES7210, 0x51 PCF85063 RTC, 0x70 SHTC3.

### M1 — Input layer + launcher shell (de-risk BLE keyboard — highest risk)
- ✅ NimBLE HOGP host: scan, connect, bond (encrypted, persisted), auto-reconnect.
- ✅ HID report → LVGL key event bridge; on-screen "typing test" textarea works.
  - **8BitDo quirk:** ignores Boot Protocol; only sends a 16-byte **NKRO bitmap**
    report (byte0 = modifiers; byte b≥1, bit p → HID usage (b-1)*8 + p). Decoded
    in `ble_kbd.cpp` incl. shift. Verified: typed text + `!`(shift) + Enter.
- ✅ KEY/BOOT buttons → Back/Home. KEY (GPIO18) injects Esc (Back); BOOT (GPIO0)
  jumps to the launcher (Home). Active-low, debounced.
- ✅ Home-screen app launcher: icon-tile grid (Notes/Calendar/Reminders/Music/
  Settings), arrow-key nav with inverted focus, Enter opens stub app, Esc = back.
  Perf: ST7305 SPI 2→10 MHz (Waveshare-proven) fixed typing lag; font Montserrat 18.
- **✅ M1 COMPLETE** — pair/bond a BLE keyboard, type into a field, navigate the
  launcher with arrows/enter/esc, and Back/Home via physical buttons. All verified.
- **Exit:** can pair a real BLE keyboard and type into a field; navigate the launcher with arrows/enter/esc. *If BLE host proves unstable, fall back plan: USB-OTG wired keyboard or a matrix/UART keyboard on the header — decide here.*

### M2 — Notes + templates
- ✅ Note list, create/edit/delete, LittleFS persistence, autosave (3s + on exit).
  One file per note at `/notes/<id>.txt`; first line = title. Verified: survives reboot.
- ✅ Template picker (Blank / Journal / Daily Log) on "New note"; templates seed
  the editor. Daily Log titles by date via the PCF85063 RTC ({date} -> MM-DD-YY).
  Existing notes open with no template (blank failover).
- ✅ Text editor: LVGL textarea, steady (non-blinking) cursor for the reflective panel.
- ✅ **Exit met:** create a note from a template, reopen after reboot — verified.
- **✅ M2 COMPLETE.** (RTC read brought forward for dates; full RTC set/NTP is M3.)
- Mono-UI lesson: `lv_list` renders invisibly on the 1-bit panel — build rows by
  hand with explicit black text + inverted-focus highlight (see notes.cpp make_row).

### M3 — Calendar (+ Wi-Fi/NTP time)
- ✅ PCF85063 driver: read/set time (rtc.cpp). (Alarm + IRQ deferred to M3.5.)
- ⬜ Agenda view: chronologically-sorted event list; add/edit/delete local events.
  One file per event at `/events/<id>.txt` (line 1 = `YYYY-MM-DD HH:MM`, line 2 = title),
  edited via a two-line textarea (same pattern as the Notes editor).
- ✅ **Wi-Fi NTP sync + Settings screen** (user priority): Settings tile → Wi-Fi scan →
  pick SSID → password (Enter submits) → connect + NTP → set RTC (UTC); creds saved
  to NVS, auto-sync at boot; status-bar Wi-Fi icon. Wi-Fi connects only to sync then
  disconnects (RTC is timekeeper). Config via config.{h,cpp} (Preferences).
  - Gotchas fixed: one-line textarea swallows Tab/Enter → submit via LV_EVENT_READY;
    Enter key-release leaked a click to the home tile → defer go-home via one-shot timer.
- **Exit:** create an event from the keyboard, reopen after reboot — it persists.

### M3.5 — Reminders (deferred out of M3)
- ⬜ PCF85063 alarm + IRQ handling.
- ⬜ Reminder scheduler: arm the RTC alarm for the next event's reminder → wake +
  on-screen alert + ES8311 beep. (First use of the audio codec — a preview of M4.)
- **Exit:** an event set 2 min out fires a visible + audible reminder.

### M4 — Music player
- ES8311 I2S init; `ESP32-audioI2S` playback from SD.
- Browse `/music`, play/pause/next/prev/seek, volume; now-playing screen.
- Verify playback stays glitch-free while UI redraws (audio on core 1, UI on core 0).
- **Exit:** play an MP3 off the SD card, control it from the keyboard, no dropouts.

### M5 — Polish
- Status bar (clock, battery, optional temp/humidity from SHTC3).
- Power management: PWR-button sleep, screen-idle, battery gauge, deep-sleep wake on RTC/KEY.
- Settings app (Wi-Fi, keyboard re-pair, time, brightness N/A → contrast if supported).
- Persistence hardening, backups to SD.

## 5. Top risks & mitigations
| Risk | Likelihood | Mitigation |
|---|---|---|
| BLE HID *host* on ESP32-S3 flaky/complex | High | Prototype in M1 first; keep USB-OTG / header-keyboard fallback |
| Reflective display too slow / mono-only for desired UI | Medium | Confirm depth + redraw cost in M0 before building apps; design static UI |
| Audio dropouts during UI redraw | Medium | Pin audio to core 1, DMA I2S, throttle redraws |
| SD + I2S + BLE contention (CPU/bus) | Medium | PSRAM buffers, prioritize audio task, lazy UI |
| RTC drift without network | Low | Optional NTP sync; backup battery keeps time across power loss |

## 6. Open questions (answer before/at M1)
1. ~~Do you already own a specific **BLE** keyboard?~~ **Resolved: 8BitDo Retro Mechanical
   Keyboard — confirmed BLE/HOGP.** Use its Bluetooth mode switch ("B"); expect bonding.
2. ~~Confirm the RLCD's actual **color depth**~~ — **Resolved: ST7305 is 1-bit monochrome.** LVGL uses `LV_COLOR_DEPTH 1`; design a pure black/white theme. (Verify in M0 whether the panel exposes any grayscale via ST7305 4-level mode.)
3. Music format scope — MP3 only, or WAV/AAC too?
4. Is Wi-Fi in scope at all for v1 (NTP only), or fully offline?

## 7. Status + forward roadmap (M6+)

> Status as of 2026-07-27: **M0–M6 complete.** The milestone list above drifted:
> M5 became *perf + polish* (LVGL direct_mode 6.4× redraw, status bar w/ clock +
> battery + timezone), M6 was a UX pass (Notes editor, calendar month grid,
> reminders UX, music UX — branch `m6-ux`), and power management moved to M7.

The roadmap below is informed by [PocketMage PDA](https://github.com/TailsmanDesign/PocketMage_PDA)
(Apache-2.0 — code/ideas portable), a shipped ESP32-S3 E-Ink PDA with a very
similar philosophy. Its app roster: TXT, FileWiz, USB transfer, Settings,
Tasks, Calendar (month/week/day + repeat grammar), Journal (date-keyed),
Lexicon (SD dictionary), AppLoader (OTA .tar apps), Terminal (Wrench
scripting), COMM (ESP-NOW mesh chat).

### Compatibility triage vs our hardware (BLE kbd, 300×400 RLCD, SD, ES8311, PCF85063)

- **Port / adapt:** Tasks, Journal, Lexicon, file manager, USB-MSC file
  transfer, timers/alarms, e-book reader, home command bar, recurring events,
  sleep/wake UX (single-key wake-to-app, standby dashboard), 0–9 quick-select.
- **Skip:** AppLoader (OTA 3rd-party apps — we're a monolithic LVGL build),
  Terminal/Wrench scripting (big surface, little PDA value), OLED hybrid UI
  (no second screen), touch scroll bar (no touch).
- **Backlog:** COMM ESP-NOW chat (needs a second unit), Wi-Fi note/calendar
  sync, custom abbreviations / text expansion.

### M7 — Power / deep-sleep (+ PocketMage sleep UX) ✅ (branch m7-power)
- ✅ Idle timeout (Settings row: Off/1/2/5/10 m, default 2 m) → **standby
  dashboard** (48pt clock, date, battery, today's agenda) → deep sleep. Panel
  keeps the image (ST7305 LPM 0x39); a ~60 s timer wake redraws the clock via a
  minimal boot (display+FS+RTC, no BLE/audio) and re-sleeps — verified via the
  new `/pwr.log` boot journal (r8/w4 dash-tick entries).
- ✅ **On external power → awake desk clock** instead of deep sleep (any key
  returns; unplug drops to real sleep on the next 30 s tick). USB-host detect
  via `HWCDC::isPlugged()` (battery-voltage alone can't detect a charger with a
  part-full cell). This also keeps USB alive for development flashing.
- ✅ KEY (GPIO18, EXT1 any-low) wakes to a full boot; **BOOT is deliberately
  not a wake pin** (GPIO0 strap → download mode if still held at reset).
- ✅ Reminders across sleep: wake timer arms for min(60 s, next event); on a
  due wake the full boot replays the missed window through the scheduler
  (baseline seeded from the pre-sleep timestamp), so the alert + beep fire.
  Deep sleep is blocked while a snooze is pending (snoozes are RAM-only).
- ✅ Critical battery (≤3%): dashboard says "charge me", minute tick disabled.
- Findings: PCF85063 INT is **not routed** on this board (ESP32 timer wake
  stands in; RTC re-read every wake so drift never accumulates). RTC slow
  clock runs ~15% fast (60 s ticks arrive at ~49 s — harmless). Spurious EXT1
  wakes on GPIO18 happen occasionally (benign full boot). `setCpuFrequencyMhz`
  **hangs with BLE active** → CPU scaling dropped; awake = 240 MHz.
- `tools/capture.py` (serial capture w/ reset) + `tools/catch_wake.py` (catch
  the 1 s wake window of a sleeping device and hold it in the ROM bootloader
  for flashing).
- Deferred: single-key wake into a specific app; RTC slow-clock calibration.

### M8 — Tasks app + calendar power-ups ✅ (branch m8-apps; recur.cpp host-tested, on-device play-test pending)
- **Tasks** (`tasks.cpp`): title + optional due date, done/undone toggle,
  auto-sort by due date, 0–9 quick-toggle, `/littlefs/tasks.json`. Due-dated
  tasks surface in the calendar day agenda and (opt-in) arm reminders.
- **Recurring events**, PocketMage grammar: `daily` / `weekly mo,we` /
  `monthly 15` or `monthly 2tu` / `yearly apr22` — stored on the event file,
  expanded into month-grid dots + agenda + reminder scheduler.
- Type-to-jump in month view (digits → go to date).

### M9 — Journal + Notes upgrades
- **Journal** (`journal.cpp`): date-keyed entries — `T`/Enter = today,
  `YYYYMMDD` or `jan 1` opens/creates that day; seeded from the existing
  Journal template; streak / "days written" counter; archive to SD.
- **Notes**: search across titles+bodies, sort by modified, word count in the
  editor, export/import to SD.

### M10 — Files + USB transfer
- **FileWiz** (`files.cpp`): browse LittleFS + SD, open .txt in the editor,
  rename/delete, recents on 0–9.
- **USB app**: expose the SD card to a PC as TinyUSB **MSC** (dedicated app
  mode — unmount SD locally while exposed, eject to return). Loads music /
  dictionaries / books without pulling the card. Needs `USB Mode: USB-OTG`
  build flag; verify coexistence with CDC serial logging.

### M11 — Lexicon + Reader
- **Lexicon**: offline dictionary from SD (prefix-indexed flat file; `<`/`>`
  cycle definitions). Include a `tools/` script to build the data file from
  WordNet/GCIDE.
- **Reader**: paginated .txt reader from SD, saved position per book, reuses
  the S/M/L font bar. Ideal use of the reflective panel.

### M12 — Home command bar + consistency pass
- Command bar under the launcher grid: type to filter/launch apps, open a note
  by name, quick-add reminder (`rem 15m tea`), `roll d20`, `timeset`/`dateset`,
  inline calculator (`= 12*7`).
- Global keystroke consistency audit + `KEYS.md` manual + on-device `?` help
  overlay per app.
