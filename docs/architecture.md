# Architecture

How the firmware is put together: the boot sequence, the two-core task model, the
app/screen lifecycle, and where data lives.

## Boot sequence

`setup()` in `main.cpp` runs in a deliberate order — the ordering matters because
a deep-sleep wake re-enters `setup()` from the top, and some wakes must re-sleep
*before* the panel is ever touched.

```
Serial.begin(115200)
[delay only if this is NOT a timer wake]   ← skip the USB-CDC settle on silent polls
storage_init()      mount LittleFS
rtc_init()          PCF85063 over I2C; also mirrors RTC → system clock
power_early_boot()  ← on a battery silent-poll wake, re-sleeps here and never returns
display_init()      ST7305 bring-up + LVGL display driver
input_init()        NimBLE HOGP host + LVGL keypad input device
buttons_init()      physical KEY / BOOT
audio_init()        ES8311 codec + I2S (shares the RTC's I2C bus)
launcher_build()    home-screen grid + status bar; starts the 1 s status timer
settings_boot_sync()if Wi-Fi creds saved: connect, NTP-sync the RTC, disconnect
```

**Why `power_early_boot()` sits before `display_init()`:** the device wakes from
deep sleep roughly once a minute to check for due reminders. If nothing is due
and it's on battery, it must go straight back to sleep without flashing the
panel. Putting the re-sleep decision before display bring-up keeps that path
fast and invisible. See [performance.md](performance.md#8-power--deep-sleep) and
`power.cpp`.

## The two-core task model

This is the single most important structural decision. The ESP32-S3 has two
cores; the firmware uses them as a hard split:

| Core | Runs | How |
|---|---|---|
| **Core 1** | The entire UI: LVGL, input, buttons, BLE state machine | Cooperative, in `loop()` |
| **Core 0** | Audio decode + I2S feed | A dedicated FreeRTOS task |

**Core 1 — the UI loop.** `loop()` is dead simple and cooperative:

```cpp
void loop() {
  ble_loop();          // drive the BLE connect state machine
  buttons_poll();      // physical KEY / BOOT
  lv_timer_handler();  // LVGL: input read, timers, render, flush
  delay(5);
}
```

Everything visual happens inside `lv_timer_handler()` — the input device is
polled, `lv_timer`s fire (the 1 s status bar, the 10 s reminder scheduler, app
timers), invalidated regions are re-rendered into the framebuffer, and the frame
is flushed. Because this is cooperative, **any long blocking call here freezes the
whole UI** — which is exactly why audio can't live here.

**Core 0 — the audio task.** Started by `music_init()`:

```cpp
xTaskCreatePinnedToCore(audio_task, "music", 10240, nullptr, 5, &g_task, 0);
```

The task owns the decode→I2S pipeline end to end. The UI sends it commands
(play/pause/next/volume) through a FreeRTOS **queue** and reads back player state;
it never touches the audio path directly. This is what makes MP3 playback
glitch-free: the panel's ~12 ms full-screen SPI flush and the reminder poll's
LittleFS reads run on core 1 and cannot starve the sample feed on core 0.

> **Consequence for contributors:** shared state between the UI and the audio task
> (track index, name, volume, play state) is guarded by a mutex (`g_mux`) and, for
> the fast path, read lock-free. Never hold `g_mux` across slow work — see the SD
> scan case study in [performance.md](performance.md#5-keep-mutexes-off-the-slow-path).

## App and screen lifecycle

Every app is a self-contained module (`notes.cpp`, `music.cpp`, …) exposing one
entry point (`notes_open()`), wired into the launcher's dispatch in
`launcher.cpp`. Apps follow a common contract built around three LVGL mechanisms:

- **Screens.** An app builds one or more `lv_obj` screens with
  `lv_obj_create(nullptr)` and shows one via `lv_scr_load()`. The launcher home is
  itself just another screen.
- **The keypad group.** There is one default input group. On entering a screen an
  app calls `lv_group_remove_all_objs()` then adds its focusable widgets, so arrow
  keys / Enter / Esc route to that screen. Getting this right is what makes the
  device fully keyboard-navigable with no touch.
- **The leave hook.** Because returning "home" can happen from anywhere (Esc, the
  physical Home path, idle→sleep), each app registers a teardown callback with
  `launcher_set_leave_hook()`. `launcher_go_home()` reactivates the home screen
  first, then calls the hook so the app can free its screens and timers safely.

**A recurring gotcha, documented once here:** on this LVGL build, an Enter/Esc
*release* leaks a synthetic click to whatever screen loads next. Screen switches
that happen on those keys are therefore deferred by one tick (via a zero-delay
`lv_timer`) so the click lands on nothing. You'll see this pattern in several
apps; it is intentional.

## Where data lives

Two filesystems, split by size and volatility:

| Store | Medium | Holds |
|---|---|---|
| **LittleFS** | Internal flash (~8 MB partition) | Notes, journal, calendar/reminder events, tasks, config, reader positions |
| **SD card** | microSD via SDMMC (1-bit) | Music, books (Reader), the WordNet dictionary, and SD backups/exports |
| **NVS** | Internal flash (small partition) | Paired-keyboard MAC, Wi-Fi credentials, preferences |
| **RTC RAM** | Survives deep sleep | Sleep timestamp + dashboard state across wake cycles |

`storage.cpp` provides an idempotent `storage_sd_mount()`; the Music, Reader,
Lexicon, and Files apps all share it. The Files app can additionally *unmount* the
SD and hand it raw to the host as a USB Mass Storage device — see the USB-MSC
section of [hardware.md](hardware.md) and the crash lesson in
[performance.md](performance.md).

System time and the RTC are kept in agreement: `rtc_init()` mirrors the PCF85063
into the system clock (`settimeofday`), so `time()` and LittleFS `getLastWrite()`
are real. The RTC stores UTC; a config-stored offset produces local time for the
clock, calendar, and reminder scheduler so they never disagree.

## Memory & flash budget

- **RAM:** ~48% of 512 KB internal SRAM in use. The 15 KB LVGL framebuffer is a
  static array in **internal** SRAM (not PSRAM — see the note in
  [display-pipeline.md](display-pipeline.md#why-the-framebuffer-stays-in-internal-sram)).
- **Flash:** ~42% of 16 MB. Dual OTA app slots (~4 MB each), a 64 KB coredump
  partition, and the ~8 MB LittleFS. Full layout in
  [hardware.md](hardware.md#flash-partition-layout).
- **PSRAM:** 8 MB, currently reserved for large media buffers via `ps_malloc`
  rather than the framebuffer.
