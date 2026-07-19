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
- NimBLE HOGP host: scan, pair, bond a BLE keyboard; persist MAC; auto-reconnect.
- HID report → LVGL key event bridge; on-screen "typing test" buffer.
- KEY/BOOT buttons → Back/Menu.
- Home-screen app launcher with LVGL group navigation.
- **Exit:** can pair a real BLE keyboard and type into a field; navigate the launcher with arrows/enter/esc. *If BLE host proves unstable, fall back plan: USB-OTG wired keyboard or a matrix/UART keyboard on the header — decide here.*

### M2 — Notes + templates
- Note list, create/edit/delete, LittleFS persistence, autosave.
- Template picker (journal, daily log, blank); templates seed title + body scaffold.
- Text editor: cursor, backspace, word-wrap, scroll — tuned for slow refresh.
- **Exit:** create a journal entry from a template, reopen it after reboot.

### M3 — Calendar + reminders
- PCF85063 driver: read/set time, set alarm, handle IRQ.
- Month/agenda view; add/edit local events.
- Reminder scheduler: arm RTC alarm for next reminder → wake + on-screen alert + beep.
- Optional NTP sync on Wi-Fi connect to correct RTC drift.
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
