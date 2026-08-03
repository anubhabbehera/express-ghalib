# Hardware reference

The Waveshare ESP32-S3-RLCD-4.2, as this firmware uses it: the pin map, every
peripheral, the facts you must design around, and the flash layout.

## Module

**ESP32-S3-WROOM-1-N16R8** — dual-core Xtensa LX7 @ 240 MHz, **16 MB QIO flash**,
**8 MB Octal (OPI) PSRAM @ 1.8 V**, 512 KB internal SRAM. Configured in
`platformio.ini` as `memory_type = qio_opi` (QIO flash + OPI PSRAM).

## Pin map

Verified on hardware. Source: Waveshare's official Arduino example `user_config.h`
(the `RLCD_*_PIN` values) plus the board schematic.

### Display — ST7305 reflective LCD (4-wire SPI)
| Signal | GPIO |
|---|---|
| SCLK | 11 |
| MOSI | 12 |
| CS | 40 |
| DC | 5 |
| RST | 41 |
| TE (tearing-effect, unused) | 6 |

> ⚠️ The ESPHome port lists 39/38 for SCK/MOSI. Those are **wrong** for this board
> — verified in M0: 11/12 renders, 39/38 gives a blank panel.

### I2C (shared bus) — PCF85063 RTC + SHTC3 sensor
| Signal | GPIO |
|---|---|
| SDA | 13 |
| SCL | 14 |

The ES8311 audio codec (address `0x18`) also sits on this I2C bus for its control
registers.

### Audio — ES8311 codec (I2S)
| Signal | GPIO | Notes |
|---|---|---|
| MCLK | 16 | ESP32 is I2S master; MCLK = 256 × fs |
| BCLK | 9 | |
| WS / LRCK | 45 | |
| DOUT (to codec) | 8 | |
| PA enable | 46 | Left **LOW** after init; raised only around playback/beeps |

The codec is an **I2S slave**; the ESP32 provides the clocks. Base sample rate is
16 kHz (MCLK 4.096 MHz). The power amp on GPIO46 **must be driven HIGH to hear
anything** — it's kept low except during audio to save power and avoid hiss.

### microSD — SDMMC (1-bit mode)
| Signal | GPIO |
|---|---|
| CLK | 38 |
| CMD | 21 |
| D0 | 39 |

`SD_MMC.setPins(38, 21, 39)`; mounted 1-bit. No bus conflict with the display SPI.

### Buttons (side, active-low)
| Button | GPIO | Role |
|---|---|---|
| KEY | 18 | Back / Home; long-press (~1.5 s) = BLE re-pair; **EXT1 deep-sleep wake** |
| BOOT | 0 | Menu; **strapping pin — never a deep-sleep wake source** |

### Battery sense
18650 voltage on **GPIO4** (ADC1_CH3) through a ÷3 divider, 11 dB attenuation
(Waveshare factory-demo wiring). There is no dedicated VBUS/charge-detect pin, so
"on external power" is inferred (≥ ~4.10 V, or the USB-CDC plugged state).

## Peripherals & non-obvious facts

Design *around* these — most caused a real bug or dead end at least once:

- **Bluetooth is LE-only.** No Classic BT. The keyboard must be a BLE HID device;
  the firmware is a NimBLE HOGP host (central). Expect bonding.
- **No touch panel.** Every screen must be fully keyboard-navigable.
- **Reflective 1-bit display**, natively landscape 400×300, **non-linear 2×4-block
  packing** (Y inverted, MADCTL `0x48`). Never send a plain bitmap; always use the
  driver's pixel setter. Details in [display-pipeline.md](display-pipeline.md).
- **The PCF85063 STOP bit** can leave the RTC frozen; boot code must clear it. The
  RTC's **INT pin is not routed** on this board — hardware alarm-driven wake is
  impossible, so deep-sleep timing uses the ESP32's own timer. The RTC slow clock
  runs ~15% fast; account for it if you rely on wake precision.
- **Audio codec shares the RTC's I2C bus** — bring-up order matters.
- **`setCpuFrequencyMhz()` hangs while BLE is active.** No DVFS while connected.

## Flash partition layout

`partitions_16MB.csv` — 16 MB total:

| Partition | Type | Offset | Size | Purpose |
|---|---|---|---|---|
| `nvs` | data/nvs | 0x9000 | 20 KB | Paired-keyboard MAC, Wi-Fi creds, prefs |
| `otadata` | data/ota | 0xe000 | 8 KB | OTA slot selector |
| `app0` | app/ota_0 | 0x10000 | 4 MB | Firmware slot A |
| `app1` | app/ota_1 | 0x410000 | ~3.9 MB | Firmware slot B (OTA-ready) |
| `coredump` | data/coredump | 0x800000 | 64 KB | Crash coredumps (ELF) |
| `littlefs` | data/spiffs | 0x810000 | ~8 MB | Notes, journal, events, config, reader positions |

The `coredump` partition was carved from `app1`'s tail so the LittleFS offset and
size are unchanged — **user data survives** the partition-table update. The
Arduino libs ship with `ESP_COREDUMP_ENABLE_TO_FLASH` on, so every panic leaves a
symbolizable dump; see the decode workflow in
[build-flash-debug.md](build-flash-debug.md#decoding-a-crash-coredump).

## USB-MSC (SD-over-USB)

The build runs in **USB-OTG / TinyUSB mode** (`ARDUINO_USB_MODE=0`) so the Files
app can expose the SD card to a host computer as a USB Mass Storage device. This
has firmware-wide consequences:

- Serial becomes the TinyUSB CDC (a different port; see build docs).
- `power.cpp` uses `tud_mounted()` instead of `HWCDC::isPlugged()` to detect USB
  power under this mode.
- **Ejecting the MSC volume must detach at the USB bus level** (`tud_disconnect()`)
  before tearing down, then re-enumerate. A polite "media not ready" reply is
  *not* enough — the host retries forever and the busy-loop trips the task
  watchdog (this was a real, coredump-diagnosed crash). See the case study in
  [performance.md](performance.md) and `files.cpp`.
