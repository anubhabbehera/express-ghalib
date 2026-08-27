# express-ghalib

A pocket, keyboard-driven PDA firmware for the **Waveshare ESP32-S3-RLCD-4.2** —
paper-like, fully local, and navigated entirely from a Bluetooth keyboard.

No cloud. No accounts. No touchscreen. A reflective monochrome panel, a BLE
keyboard, an SD card, and ten small apps that cover notes, planning, reading, and
music.

---

## 1. Overview

Modern pocket computers are bright, loud, and online. This is the opposite: a
calm device you keep on a desk or in a bag to *think* with — write a note, check
the week, read a chapter, look up a word, play an album — and nothing else.

It follows two projects directly:

- **[Orion PDA](https://orionpda.org/)** — the philosophy: a minimal, offline,
  keyboard-first personal device with a paper-like screen, built to do a few
  things well instead of everything badly.
- **[PocketMage PDA](https://github.com/TailsmanDesign/PocketMage_PDA)**
  (Apache-2.0) — a shipped ESP32-S3 E-Ink PDA with a very similar philosophy.
  Its app roster and interaction ideas (Tasks, Journal, Lexicon, file manager,
  USB transfer, recurring-event grammar, standby/wake UX, 0–9 quick-select)
  informed everything from M7 onward.

The hardware it is built for is a **$27-class off-the-shelf development board**:
the [Waveshare ESP32-S3-RLCD-4.2](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm)
— an ESP32-S3 (16 MB flash, 8 MB PSRAM) behind a 4.2", 300×400, 1-bit
**reflective LCD** that needs no backlight and holds its last image at
essentially zero power, plus an ES8311 audio codec, a PCF85063 RTC, a microSD
slot, and an 18650 battery holder. There is no touch panel and Bluetooth is
LE-only, so a **BLE HID keyboard is the only input device** apart from two side
buttons.

The design principles that fall out of that hardware:

- **Local-first.** Everything lives on the device — notes and journal on internal
  flash, music/books/dictionary on the SD card. Wi-Fi is used only to set the
  clock. Nothing leaves the device.
- **Keyboard-native.** Every screen is fully drivable from arrow keys, Enter,
  Esc, and a handful of letter shortcuts.
- **Paper-like, not app-like.** The panel refreshes slowly, so the UI is static
  and page-based: no animation, no scrolling where a page turn will do.
- **Quiet by default.** The device deep-sleeps behind a standby dashboard and
  wakes on a key; on USB power it becomes a desk clock. Battery life is a design
  constraint, not an afterthought.

---

## 2. Hardware

### 2.1 The board this firmware targets

**[Waveshare ESP32-S3-RLCD-4.2](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm)**
— [Waveshare wiki / docs](https://docs.waveshare.com/ESP32-S3-RLCD-4.2) ·
[vendor code + factory firmware](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2) ·
[Zephyr board docs](https://docs.zephyrproject.org/latest/boards/waveshare/esp32s3_rlcd_4_2/doc/index.html)
· [ESPHome device page](https://devices.esphome.io/devices/waveshare-esp32-s3-rlcd-42/)

| Part | Detail | Used for |
|---|---|---|
| **SoC** | ESP32-S3-WROOM-1-**N16R8**, dual-core LX7 @ 240 MHz | Everything |
| **Memory** | 16 MB QIO flash · 8 MB Octal PSRAM · 512 KB SRAM | Code, LVGL, LittleFS |
| **Display** | 4.2" reflective LCD (**ST7305**), **300×400**, 1-bit mono, no backlight | The whole UI |
| **Wireless** | Wi-Fi 2.4 GHz + **Bluetooth 5 LE only** | BLE keyboard host, NTP time sync |
| **Audio out** | **ES8311** codec over I2S → MX1.25 mono speaker header | Music, reminder beeps |
| **Audio in** | ES7210 ADC + dual mic | Unused by this firmware |
| **RTC** | **PCF85063** + backup cell | Clock, calendar, reminders |
| **Storage** | microSD (SDMMC 1-bit) + internal LittleFS | Music/books/dictionary + notes/config |
| **Input** | Side **KEY** + **BOOT** buttons; BLE keyboard | Wake / back / re-pair; all text |
| **Sensor** | SHTC3 temp/humidity (I2C) | Available, not surfaced in the UI |
| **Power** | USB-C + 18650 holder, battery sense on GPIO4 (÷3 divider) | Battery gauge, charge detect |

**Three hardware facts that shaped everything:**

1. **Bluetooth is LE-only.** A Classic-BT keyboard will never pair — you need a
   BLE HID keyboard. The firmware is a NimBLE HOGP *host* (central).
2. **No touch.** Navigation is keyboard + two buttons; every UI is keyboard-complete.
3. **The panel is reflective and 1-bit**, natively landscape 400×300 with a
   non-linear 2×4-block pixel packing. It refreshes slowly and holds its image
   with the power off. You design *around* the display — that is the source of
   most of the interesting engineering here (see
   [docs/display-pipeline.md](docs/display-pipeline.md)).

Full pin map, peripheral notes, and the flash layout:
**[docs/hardware.md](docs/hardware.md)**.

### 2.2 What a compatible board needs

Nothing here is board-agnostic by magic — the firmware assumes these five things,
and each one is isolated in a single file, so a port is a driver swap rather than
a rewrite:

| Assumption | Where it lives | Porting note |
|---|---|---|
| ST7305/ST7306 reflective panel, 300×400 portrait | `src/st7305.*` | Another size needs a new address window + the 2×4 packing math |
| ESP32-S3 with ≥ 8 MB flash (16 MB assumed) and PSRAM | `platformio.ini`, `partitions_16MB.csv` | Smaller flash = shrink the LittleFS partition; the framebuffer itself lives in SRAM |
| ES8311 codec on I2C + I2S | `src/audio.cpp` | An I2S DAC/amp (no I2C config) is a smaller driver, not a bigger one |
| PCF85063 RTC on I2C | `src/rtc.cpp` | Any I2C RTC works; ~150 lines |
| microSD over SDMMC 1-bit | `src/storage.cpp` | SPI cards work via `SD.h` with a pin change |

Everything above the drivers — LVGL setup, the BLE keyboard host, the launcher,
all ten apps, power management — is board-independent.

### 2.3 Other Waveshare / Pimoroni boards

No other board is a *drop-in* target today; this table is the honest effort
estimate if you want to try.

| Board | Fit | What it takes |
|---|---|---|
| **[Waveshare ESP32-S3-RLCD-4.2](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm)** | **Target** | Nothing — this is the board |
| [Waveshare ESP32-S3-ePaper-3.97](https://www.waveshare.com/esp32-s3-epaper-3.97.htm) (800×480 e-paper, [docs](https://docs.waveshare.com/ESP32-S3-ePaper-3.97)) | Close cousin | New display driver + a relayout for the bigger panel; RTC/SHTC3 already match. Same paper-like philosophy |
| [Waveshare ESP32-S3-ePaper-1.54](https://www.waveshare.com/esp32-s3-epaper-1.54.htm) (200×200, [wiki](https://www.waveshare.com/wiki/ESP32-S3-ePaper-1.54)) | Partial | Display driver plus a real UI rethink — 200×200 is too small for the current screens |
| [Waveshare ESP32-S3-AUDIO-Board](https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board) | Partial | Shares the ES8311 + PCF85063 stack; bring your own panel |
| [Waveshare ESP32-S3-Touch-LCD-1.69](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.69) / [AMOLED-1.8](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8) | Partial | PCF85063 matches; color display means a new driver and dropping the 1-bit theme |
| [Waveshare ESP32-S3-Zero](https://www.waveshare.com/wiki/ESP32-S3-Zero) | DIY base | 4 MB flash / 2 MB PSRAM — usable only with a shrunk partition table and no Lexicon |
| [Pimoroni ProS3](https://shop.pimoroni.com/en-us/products/pros3-esp32-s3) / [FeatherS3](https://shop.pimoroni.com/en-us/products/feathers3-esp32-s3) (Unexpected Maker) | DIY base | 16 MB flash + 8 MB PSRAM ESP32-S3 boards — the MCU half of a DIY build; wire the panel, codec, RTC and SD yourself |
| [Espressif ESP32-S3-DevKitC-1](https://www.adafruit.com/product/5364) | DIY base | Same as above, with more free GPIO |
| Pimoroni Badger 2040 W / Inky Frame | **Not compatible** | RP2040/RP2350, not ESP32-S3 — no BLE HOGP host, no IDF; this firmware will not run |

### 2.4 DIY build (roll your own board)

The device is five parts plus a battery. Everything here is a drop-in for a
Waveshare peripheral, with the driver file to touch listed above.

| Part | Suggested component | Notes |
|---|---|---|
| **MCU** | [Pimoroni ProS3](https://shop.pimoroni.com/en-us/products/pros3-esp32-s3), [ESP32-S3-DevKitC-1 (N16R8)](https://www.adafruit.com/product/5364), or [Waveshare ESP32-S3-Zero](https://www.waveshare.com/wiki/ESP32-S3-Zero) | ESP32-S3 required (BLE 5 LE + IDF 5). 16 MB flash keeps `partitions_16MB.csv` unchanged |
| **Display** | [Good Display GDTL042T71](https://www.good-display.com/product/455.html) — 4.2" reflective TFT, 400×300, SPI | Same panel class as the Waveshare board. Driver reference: [`musicaJack/ST73xx_Reflective_Lcd`](https://github.com/musicaJack/ST73xx_Reflective_Lcd) and [`kylehase/ESPHome-ST7305-RLCD`](https://github.com/kylehase/ESPHome-ST7305-RLCD). Smaller ST7305 panels (e.g. 2.13", 122×250) work with a new address window |
| **Audio** | [Adafruit MAX98357A I2S 3W amp](https://www.adafruit.com/product/3006) + a 4Ω/8Ω speaker | Replaces the ES8311 entirely: I2S in, speaker out, no I2C config. For reminder beeps only, a passive buzzer on a PWM pin is enough |
| **RTC** | A PCF85063 breakout (keeps `rtc.cpp` as-is) or [Adafruit DS3231](https://www.adafruit.com/product/3013) | The PCF85063's INT line is unused by this firmware — wake is an ESP32 timer, so an RTC without an IRQ pin is fine |
| **microSD** | [Adafruit MicroSD SPI/SDIO breakout](https://www.adafruit.com/product/4682) (SDIO-capable) or the [classic breakout](https://www.adafruit.com/product/254) (SPI) | SDIO matches the current 1-bit SDMMC path; SPI needs the `SD.h` swap |
| **Power** | 18650 cell + holder, any TP4056-class charger, and a ÷3 resistor divider into an ADC1 pin | The firmware reads battery volts on GPIO4 with 11 dB attenuation; change the pin in `power.cpp` |
| **Keyboard** | Any **BLE HID** keyboard | Verified: 8BitDo Retro Mechanical (Bluetooth mode switch "B"). Classic-BT keyboards will not pair |

---

## 3. Quick start

### 3.1 Prerequisites

- [PlatformIO Core](https://platformio.org/) (`pio`) or the VS Code extension.
  The platform (pioarduino fork of `espressif32`) and all libraries are pinned in
  `platformio.ini` and fetched on first build.
- Python 3 — only for the helper scripts in `tools/`.
- A USB-C cable, a **BLE HID keyboard**, and (optionally) a FAT32 microSD card.

### 3.2 Build and flash

```bash
git clone git@github.com:anubhabbehera/express-ghalib.git
cd express-ghalib

pio run                 # build
pio run -t upload       # build + flash over USB-C
```

> The build is **USB-OTG** (`ARDUINO_USB_MODE=0`) so the Files app can expose the
> SD card as a USB drive. Uploading uses a 1200 bps "touch" that reboots the
> running app into the ROM bootloader, so the app must be alive to be flashed —
> if it is crashed or asleep, hold **BOOT** while plugging in, or run
> `tools/catch_wake.py`. The first upload after a code change occasionally fails
> with "Error 2"; run it again.

### 3.3 Serial output

`pio device monitor` cannot be scripted (miniterm needs an interactive TTY), and
macOS has no `timeout`. Use the in-repo capture helper instead:

```bash
python3 tools/capture.py            # globs the app's TinyUSB CDC port
EG_PORT=/dev/tty.usbmodemXXXX python3 tools/capture.py
```

Flashing, port naming, and decoding crash coredumps:
**[docs/build-flash-debug.md](docs/build-flash-debug.md)**.

### 3.4 SD card layout (optional, but Music/Reader/Lexicon need it)

Format FAT32, then create what you want to use:

```
/music/         *.mp3, *.wav (16-bit PCM)   -> Music
/books/         *.txt, *.md                  -> Reader
/lexicon/       dict.txt + dict.idx          -> Lexicon
/export/        notes/ and journal/ backups  -> written by the device
```

Build the dictionary files from Princeton WordNet 3.1 (downloads ~10 MB, emits
~16 MB):

```bash
python3 tools/build_lexicon.py      # writes out/lexicon/{dict.txt,dict.idx}
```

Copy them to the card directly, or over USB with the Files app's **USB transfer**
row (no card reader needed).

### 3.5 First boot

1. Put a BLE keyboard into pairing mode. The device scans on boot; you can also
   long-press **KEY** (~1.5 s) to forget bonds and pick the nearest keyboard by
   RSSI, or pair from Settings.
2. Set the clock: **Settings → Wi-Fi**, pick an SSID, type the password, Enter.
   The device syncs NTP, writes the RTC, saves the credentials, and disconnects —
   Wi-Fi is never used for anything else.
3. Set your timezone and idle-sleep timeout in Settings.

### 3.6 Regenerating assets

```bash
python3 tools/make_icons.py         # src/img_icons.c  (pixel-art launcher icons)
python3 tools/make_wave.py          # src/img_wave.c   (standby dashboard artwork)
```

---

## 4. The plan

**[PLAN.md](PLAN.md)** is the full milestone plan and running engineering
history: hardware baseline, stack decisions, the data model, every milestone with
its status linked to the commits that delivered it, the risk table, and the
resolved open questions. Each milestone entry also records the gotchas found
while building it — it doubles as a project logbook.

Where it stands:

| Milestones | State |
|---|---|
| **M0 – M0.5** display bring-up, I2C + Wi-Fi de-risk | Done |
| **M1 – M2** BLE keyboard host, launcher, Notes | Done |
| **M3 – M3.5** Calendar, Wi-Fi/NTP + Settings, Reminders | Done |
| **M4 – M6** Music, perf + status bar, UX pass | Done |
| **M7 – M9** power/deep-sleep, Tasks + recurring events, Journal | Done |
| **M10 – M11** Files + USB-MSC, Lexicon + Reader | Done |
| Post-M11 | Pixel-art icons, Pixel Operator font, perf review (9 fixes), docs, BLE pairing fix — all done |
| **M12** home command bar, keystroke audit, `KEYS.md`, `?` help overlay | **Open** — the last roadmap item |

Everything marked done is hardware-verified on the real board.

---

## 5. Apps

Ten apps, all reachable from the launcher grid (arrow keys to move, Enter to
open, Esc to go back). **KEY** is Back, **BOOT** jumps Home, and a long-press of
**KEY** re-pairs the keyboard.

### Notes
Title-and-body notes stored one file per note on internal flash. Create from a
template (Blank / Journal / Daily Log — Daily Log titles itself by date from the
RTC), autosaves while you type and on exit, and sorts the list by last-modified.
`/` searches titles *and* bodies; the editor bar shows a live word count and
S/M/L text size; `X` backs every note up to SD `/export/notes/` and `I` restores
them.

### Journal
One dated entry per day at `/journal/YYYYMMDD.txt`. The Today row and `T` jump
straight into today's entry; the jump box takes `jan 1`, `20260101`, or `t`. New
entries are seeded from the Journal template and dropped again if you leave them
untouched, so an accidental open never pollutes your streak. Tracks a writing
streak and a days-written count; `A` archives everything to SD `/export/journal/`.

### Reader
Paginated `.txt`/`.md` reading from SD `/books`. Pages are byte windows snapped
to whitespace so a page turn is exactly one static redraw — the cheapest possible
operation on this panel. Prev is exact (page-start history, not re-estimation),
each book remembers its position across reboots, and `S` cycles the shared S/M/L
font size.

### Calendar
A 7×6 month grid — today boxed, selection inverted, event dots per day — with
arrow navigation that rolls across month boundaries and type-a-digit to jump to a
date. Enter opens that day's agenda; events are created and edited in a
Notes-style editor with a date/time strip. Supports PocketMage recurrence
grammar: `daily`, `weekly mo,we`, `monthly 15`, `monthly 2tu`, `yearly apr22`,
expanded into the grid, the agenda, and the reminder scheduler.

### Tasks
A checklist with optional due dates, auto-sorted by due date, with `0`–`9` to
toggle an item without arrowing to it. Due-dated tasks surface in the calendar's
day agenda and can arm reminders.

### Reminders
Scheduled alerts with a full-screen bell overlay plus an ES8311 beep, and
Dismiss / Snooze (+10 min) chips. The list groups by Today / Tomorrow / Later
with relative times, and quick-add presets skip the editor. Reminders survive
deep sleep: the wake timer arms for the next one, and a missed window is replayed
on the next full boot so nothing is silently lost.

### Music
WAV (16-bit PCM) and MP3 (Helix decoder) playback from SD `/music` through the
ES8311. Decode and I2S writes run on their own core so UI redraws never glitch
the audio. Now-playing shows play/pause state, elapsed/total time, and a volume
bar; the track list keeps a live ► marker; `s` toggles shuffle and `r` cycles
repeat all/one. Esc leaves the app with playback running.

### Lexicon
An offline dictionary — Princeton WordNet 3.1, 207k senses — read straight off
the SD card via a two-letter-prefix index, so lookups never load the 16 MB file.
Enter looks a word up, Up/Down cycle its senses, and a miss suggests nearby
words. Multi-word entries (`kick the bucket`) work.

### Files
A file manager for both volumes: internal flash and SD, with `0`–`9` recents on
the root screen, directories first, sizes, and hidden dotfiles. Opens
`.txt/.md/.log/.csv/.json` in a text editor (32 KB cap, saves on exit), `R`
renames, Del deletes. The root screen also carries **USB transfer**: the SD card
is unmounted from the firmware and re-exposed as a USB Mass Storage device, so
the card mounts on your computer over the same USB-C cable. Esc ejects and
remounts; idle-sleep is blocked while the card is exposed.

### Settings
Wi-Fi scan/connect and NTP sync, timezone, idle-sleep timeout (Off/1/2/5/10 min),
and BLE keyboard re-pairing.

### Standby
Not an app, but where the device spends most of its life: after the idle timeout
it shows a dashboard — a 48 pt clock, the date, battery, today's agenda, and a
pixelated *Great Wave off Kanagawa* — then deep-sleeps, waking about once a
minute to redraw the clock. On USB power it stays awake as a desk clock instead.
Any key wakes it; at ≤3% battery it says "charge me" and stops the minute tick.

---

## Software stack

- **Arduino-ESP32 core 3.x** (IDF 5.x under the hood), via the
  [pioarduino](https://github.com/pioarduino/platform-espressif32) platform fork
  — *not* the upstream `espressif32`, which lagged core 3.x.
- **[LVGL 8.3](https://lvgl.io/)** in 1-bit (`LV_COLOR_DEPTH 1`) with the mono
  theme, a keypad input device, and `direct_mode` rendering into a native
  framebuffer.
- **[NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)** as a BLE central
  running the HID-over-GATT profile — the keyboard host.
- **[arduino-libhelix](https://github.com/pschatzmann/arduino-libhelix)** for MP3
  decode to PCM; WAV parsed in-house. Both stream to the ES8311 over I2S.
- **Hand-written drivers** for the ST7305 display, ES8311 codec, and PCF85063 RTC
  (no off-the-shelf library fit the board).

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
site/                Documentation website generator (see below)
partitions_16MB.csv  Flash layout (dual OTA + coredump + LittleFS)
platformio.ini       Board, platform, build flags, libraries
PLAN.md              Milestone plan and running history
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
| **[docs/build-flash-debug.md](docs/build-flash-debug.md)** | Toolchain, USB-OTG flashing, non-interactive serial capture, and decoding crash coredumps |

### The website

The same markdown is published at
**[anubhabbehera.github.io/express-ghalib](https://anubhabbehera.github.io/express-ghalib/)**,
rendered to look like the device: PixelOperator at the size the firmware draws
it, the launcher icons decoded straight out of `src/img_icons.c`, and the panel's
two-colour palette. `site/build.mjs` generates it; `.github/workflows/pages.yml`
publishes `main`.

```sh
npm install
npm run site:serve -- --watch     # http://localhost:4200, rebuilds on save
npm run site:verify               # what CI runs: strict build + link check
```

The build fails on any character PixelOperator cannot draw, so nothing on the
site silently falls back to a system font.

---

## Credits

Inspired by the [Orion PDA](https://orionpda.org/) and
[PocketMage](https://github.com/TailsmanDesign/PocketMage_PDA). Built on the
Waveshare ESP32-S3-RLCD-4.2 reference driver, LVGL, NimBLE-Arduino, and the Helix
MP3 decoder. The standby artwork is Hokusai's *Great Wave off Kanagawa* (public
domain); the dictionary is derived from Princeton WordNet 3.1.
