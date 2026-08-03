# Build, flash & debug

The mechanics of getting firmware onto the board and getting information back off
it. The USB-OTG mode (needed for the Files app's SD-over-USB) makes this less
standard than a typical ESP32 project, so read this before your first flash.

## Toolchain

[PlatformIO](https://platformio.org/). The platform is pinned to the
[pioarduino](https://github.com/pioarduino/platform-espressif32) fork in
`platformio.ini` — it ships Arduino-ESP32 core ≥ 3.0 (IDF 5.x), which upstream
`espressif32` historically lagged. Don't swap the platform without a known-good
core 3.x build.

```bash
pio run                 # build
pio run -t upload       # build + flash
```

Libraries (LVGL, NimBLE-Arduino, arduino-libhelix) are declared in
`platformio.ini` and fetched automatically. The ST7305, ES8311, and PCF85063
drivers are in-tree (no library fit the board).

## Flashing under USB-OTG

The build is `ARDUINO_USB_MODE=0` (TinyUSB), which changes the flashing flow:

- **Upload** uses a **1200 bps "touch"** (`use_1200bps_touch` +
  `wait_for_upload_port` in `platformio.ini`): `pio run -t upload` asks the
  running app to reboot into the ROM bootloader, which enumerates as a
  USB‑JTAG‑Serial port; esptool waits for that port, flashes, and the app returns
  on the TinyUSB CDC port.
- **The app must be alive** to honor the touch. If it's crashed or asleep, hold
  **BOOT** while plugging in, or use `tools/catch_wake.py` for automatic
  bootloader entry.
- **First-attempt flake:** the first `pio run -t upload` after a code change
  occasionally fails ("Error 2") with the device left in the bootloader — just run
  it again; the second attempt flashes.

### Serial ports (they differ by mode)

| Port | When it appears |
|---|---|
| **TinyUSB CDC** (`usbmodem<serial>…`, e.g. `…441BF6953EB42`) | Normal run — this is the app's serial |
| **USB‑JTAG‑Serial** (`usbmodem21201`) | Only in the ROM bootloader (during flashing) |

## Capturing serial output (non-interactively)

Two traps make the obvious approaches hang — avoid both:

- **`pio device monitor` cannot be scripted.** miniterm calls `termios.tcgetattr()`
  on stdin and dies whenever stdin isn't an interactive TTY (piped, redirected, or
  backgrounded). Read the port directly instead.
- **macOS has no `timeout` binary.** Don't bound a capture with `timeout`; use a
  line count or a backgrounded reader you stop explicitly.

Use the in-repo helper, which globs the app port (or takes `EG_PORT`) and survives
the port bouncing across a flash or a sleep cycle:

```bash
# Capture N seconds of serial to stdout
EG_PORT=/dev/cu.usbmodem441BF6953EB42 python3 tools/capture.py 60
```

To catch a **boot banner**: start `capture.py` in the background *first*, then run
`pio run -t upload` — the capture survives the port bounce and catches the fresh
boot. (The upload hard-resets the board before a plain reader can attach, so a
direct `head < port` only sees the post-boot heartbeat.)

## The power journal

Deep-sleep wakes live and die before USB-CDC enumerates (~2 s), so serial alone
can't show what happened overnight. `power.cpp` keeps a tiny boot journal on
LittleFS (`/pwr.log`) and dumps it to serial on every full boot. Reset-reason
codes are the tell:

| Code | Meaning |
|---|---|
| `r8` | deep-sleep timer wake |
| `r11` | USB / normal (expected after flashing) |
| `r6` | task watchdog reset |
| `r5` | interrupt watchdog reset |
| `r1` | power-on / cold boot |

## Decoding a crash coredump

The firmware saves a full ELF coredump to a dedicated flash partition on any panic
(the partition exists precisely for this — see
[hardware.md](hardware.md#flash-partition-layout)). To pull and symbolize it:

1. **Enter the bootloader** with the 1200 bps touch (a short pyserial script that
   opens the app port at 1200 bps and toggles DTR/RTS), so the ROM port appears.
2. **Read the partition** — at the **default baud only** (921600 fails with
   "serial data stream stopped"):
   ```bash
   python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodem21201 \
     --before no_reset --after hard_reset \
     read_flash 0x800000 0x10000 coredump.bin
   ```
3. **Decode** with `esp-coredump` (pip-install it into PlatformIO's penv):
   ```bash
   python3 -m esp_coredump --chip esp32s3 info_corefile \
     -c coredump.bin -t raw .pio/build/esp32s3-rlcd/firmware.elf
   ```
   It needs `xtensa-esp32s3-elf-gdb` on `PATH`; PlatformIO ships a generically
   named `xtensa-esp-elf-gdb`, so symlink it under the `s3` name into a `PATH`
   directory.

The decoded backtrace names the crashed task, the panic reason, and the full call
stack with source lines — this is how the USB-MSC eject watchdog crash was
diagnosed (see [performance.md](performance.md) and `files.cpp`).

## Handy tools (`tools/`)

| Tool | Purpose |
|---|---|
| `capture.py` | Timed, port-bounce-tolerant serial capture (`EG_PORT` or auto-glob) |
| `catch_wake.py` | Catch a sleeping device's brief wake and force it into the bootloader |
| `make_icons.py` | Generate the pixel-art launcher icons (`src/img_icons.c`) |
| `make_wave.py` | Generate the standby-dashboard *Great Wave* artwork |
| `build_lexicon.py` | Build the WordNet dictionary (`dict.txt` + `dict.idx`) for the SD card |
