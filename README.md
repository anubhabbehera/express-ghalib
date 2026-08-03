# express-ghalib

A pocket, keyboard-driven PDA firmware for the **Waveshare ESP32-S3-RLCD-4.2** —
paper-like, fully local, and navigated entirely from a Bluetooth keyboard. In the
spirit of the [Orion PDA](https://orionpda.org/): a minimal personal device that
does a few things well, offline, on a reflective screen that sips power and reads
like paper.

No cloud. No accounts. No touchscreen. A reflective monochrome panel, a BLE
keyboard, an SD card, and ten small apps that cover notes, planning, reading, and
music.

---

## Vision

Modern pocket computers are bright, loud, and online. This is the opposite: a
calm device you'd keep on a desk or in a bag to *think* with. The design
principles that fall out of that:

- **Local-first.** Everything lives on the device — notes and journal on internal
  flash, music/books/dictionary on the SD card. Wi‑Fi is used only to set the
  clock. Nothing leaves the device.
- **Keyboard-native.** There is no touch panel, so every screen is fully
  drivable from arrow keys, Enter, Esc, and a handful of letter shortcuts. A
  paired BLE keyboard is the only pointing device.
- **Paper-like, not app-like.** The reflective LCD has no backlight and refreshes
  slowly. The UI is built for that: static, page-based screens, no animation, no
  scrolling where a page turn will do. It holds its last image at essentially
  zero power.
- **Quiet by default.** The device deep-sleeps behind a standby dashboard and
  wakes on a key; on USB power it becomes a desk clock. Battery life is a
  first-class design constraint, not an afterthought.

---

## The board — Waveshare ESP32-S3-RLCD-4.2

| Part | Detail | Used for |
|---|---|---|
| **SoC** | ESP32-S3-WROOM-1-**N16R8**, dual-core LX7 @ 240 MHz | Everything |
| **Memory** | 16 MB flash · 8 MB Octal PSRAM · 512 KB SRAM | Code, LVGL, file storage |
| **Display** | 4.2" reflective LCD (ST7305), **300×400**, 1‑bit mono, no backlight | The whole UI |
| **Wireless** | Wi‑Fi 2.4 GHz + **Bluetooth 5 LE only** | BLE keyboard host, Wi‑Fi time sync |
| **Audio** | **ES8311** codec over I2S → mono speaker header | Music, reminder beeps |
| **RTC** | **PCF85063** + backup cell | Clock, calendar, reminder alarms |
| **Storage** | microSD (SDMMC) + internal LittleFS | Music/books/dictionary + notes/config |
| **Input** | Side **KEY** + **BOOT** buttons; BLE keyboard | Wake / back / pairing; all text |
| **Sensor** | SHTC3 temp/humidity | Optional status widget |

**Three hardware facts that shaped everything:**

1. **Bluetooth is LE-only.** A Classic-BT keyboard will never pair — you need a
   BLE HID keyboard. The firmware is a NimBLE HOGP *host* (central).
2. **No touch.** Navigation is keyboard + two buttons. Every UI is keyboard-complete.
3. **The panel is reflective and 1‑bit**, natively landscape 400×300 with a
   non-linear 2×4-block pixel packing. It refreshes slowly and holds its image
   with the power off. You design *around* the display, not against it — this is
   the source of most of the interesting engineering here (see
   [docs/display-pipeline.md](docs/display-pipeline.md)).

Full pin map and peripheral details: **[docs/hardware.md](docs/hardware.md)**.

---

## Software stack

- **Arduino-ESP32 core 3.x** (IDF 5.x under the hood), via the
  [pioarduino](https://github.com/pioarduino/platform-espressif32) platform fork
  — *not* the upstream `espressif32`, which lagged core 3.x.
- **[LVGL 8.3](https://lvgl.io/)** in 1‑bit (`LV_COLOR_DEPTH 1`) with the mono
  theme, keypad input device, and `direct_mode` rendering into a native
  framebuffer.
- **[NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)** as a BLE central
  running the HID-over-GATT profile — the keyboard host.
- **[arduino-libhelix](https://github.com/pschatzmann/arduino-libhelix)** for MP3
  decode to PCM; WAV parsed in-house. Both stream to the ES8311 over I2S.
- **Hand-written drivers** for the ST7305 display, ES8311 codec, and PCF85063 RTC
  (no off-the-shelf library fit the board).

---

## Apps shipped

Ten apps, all reachable from the launcher grid:

| App | What it does |
|---|---|
| **Notes** | Title + body notes on LittleFS, templates, live word count, full-text search, SD backup/restore |
| **Journal** | One dated entry per day, streak + days-written counts, jump-to-date, SD archive |
| **Reader** | Paginated `.txt`/`.md` reader from SD `/books`, exact Prev via history, per-book resume, S/M/L font |
| **Calendar** | 7×6 month grid + per-day agenda, add/edit/delete events, recurring-event grammar |
| **Tasks** | Checklist with optional due dates; due items surface in Calendar and the standby dashboard |
| **Reminders** | Scheduled alerts with a visual bell overlay + ES8311 beep, snooze/dismiss, recurring occurrences |
| **Music** | WAV + MP3 player from SD `/music`, glitch-free via a dedicated audio task, shuffle/repeat, now-playing |
| **Lexicon** | Offline WordNet 3.1 dictionary (207k senses) from SD, sense cycling, multi-word lookups |
| **Files** | Flash + SD browser and text editor; **USB‑MSC** to mount the SD on a computer over USB‑C |
| **Settings** | Wi‑Fi + NTP sync, timezone, idle-sleep timeout, BLE re-pairing |

A physical **KEY** long-press re-pairs the keyboard (forgets bonds, RSSI-picks the
nearest). The device idles into a standby dashboard (clock + agenda + a pixelated
*Great Wave off Kanagawa*) and then deep-sleeps, waking on KEY.

---

## Quick start

Requires [PlatformIO](https://platformio.org/). The board flashes over USB‑C.

```bash
# Build
pio run

# Flash (USB-OTG: a 1200bps "touch" reboots the app into the ROM bootloader)
pio run -t upload
```

First boot needs a **BLE HID keyboard** in pairing mode — the launcher is
keyboard-driven. Pair via Settings, or long-press **KEY** (~1.5 s) to scan and
bond the nearest keyboard.

> The build is **USB‑OTG** (`ARDUINO_USB_MODE=0`) so the Files app can expose the
> SD card as a USB drive. This changes the serial port and flashing dance — the
> first `pio run -t upload` after a code change occasionally needs a second
> attempt. Full flashing, serial-capture, and crash-coredump workflow:
> **[docs/build-flash-debug.md](docs/build-flash-debug.md)**.

---

## Repository layout

```
src/                 Firmware (one .cpp/.h pair per subsystem/app)
  main.cpp             boot, LVGL display driver, main loop, BLE input device
  st7305.*             reflective-LCD driver (1-bit, 2×4 packing)
  launcher.*           home-screen app grid + status bar
  power.*              idle → standby → deep sleep, battery, wake
  ble_kbd.*            NimBLE HOGP keyboard host
  audio.* / music.*    ES8311 codec + I2S; the core-0 playback task
  notes/journal/reader/calendar/tasks/reminders/lexicon/files/settings.*
  img_icons.c          generated pixel-art launcher icons
  fonts/               generated Pixel Operator + Montserrat fonts
include/lv_conf.h    LVGL configuration (1-bit, mono theme, tuned read period)
tools/               make_icons.py, make_wave.py, build_lexicon.py, capture.py …
docs/                Technical documentation (see below)
partitions_16MB.csv  Flash layout (dual OTA + coredump + LittleFS)
platformio.ini       Board, platform, build flags, libraries
PLAN.md              Original milestone plan and running history
```

---

## Documentation

The `docs/` folder is the engineering reference — read it before extending the
firmware, and especially before touching the render path or the power model.

| Doc | Covers |
|---|---|
| **[docs/architecture.md](docs/architecture.md)** | Boot flow, the two-core task model, app/screen lifecycle, storage and memory map |
| **[docs/display-pipeline.md](docs/display-pipeline.md)** | The ST7305 driver, 1-bit 2×4 packing, LVGL `direct_mode` + `set_px_cb`, the flush, and the render cost model |
| **[docs/performance.md](docs/performance.md)** | The optimization playbook: what's fast, what's slow, and every technique used to keep a 240 MHz dual-core responsive on a slow panel — plus the adversarial perf-review method |
| **[docs/hardware.md](docs/hardware.md)** | Full pin map, peripherals, non-obvious hardware facts, and the flash partition layout |
| **[docs/build-flash-debug.md](docs/build-flash-debug.md)** | Toolchain, USB‑OTG flashing, non-interactive serial capture, and decoding crash coredumps |

---

## Status

Milestones **M0–M11 plus a UX pass are complete and hardware-verified**: display
bring-up, BLE keyboard host, all ten apps, USB‑MSC SD transfer, the offline
dictionary + reader, deep-sleep power management, custom pixel-art icons, and a
full performance-optimization pass. The last planned milestone is **M12** (a home
command bar, keystroke audit, and help overlay). See [PLAN.md](PLAN.md) for the
milestone history.

---

## Credits

Inspired by the [Orion PDA](https://orionpda.org/) and
[PocketMage](https://github.com/TailsmanDesign/PocketMage_PDA). Built on the
Waveshare ESP32-S3-RLCD-4.2 reference driver, LVGL, NimBLE-Arduino, and the Helix
MP3 decoder. The standby artwork is Hokusai's *Great Wave off Kanagawa* (public
domain); the dictionary is derived from Princeton WordNet 3.1.
