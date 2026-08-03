# Performance & optimization playbook

Everything the firmware does to stay responsive on a 240 MHz dual-core driving a
slow 1‑bit reflective panel while also hosting BLE and decoding audio. This is the
document to read **before** optimizing anything — several intuitive optimizations
here are actively wrong, and this records which and why.

The governing cost model lives in
[display-pipeline.md](display-pipeline.md#the-cost-model-memorize-this):

- A **render** costs ~91% of a typical update and scales with the invalidated area.
- A **flush** is a fixed **~12 ms blocking SPI** push of the whole 15 KB frame,
  fired on *any* invalidation (partial flush was tried and reverted).
- **`lv_label_set_text` always invalidates**, even for byte-identical text.

Almost every technique below falls out of those three facts.

---

## The established techniques

### 1. Diff before write (the core render optimization)

**Problem.** A timer that unconditionally re-sets a label invalidates it every
tick, and any invalidation triggers a full ~12 ms flush — even when the displayed
value hasn't changed. A 1 s status clock whose text is identical 59 seconds out of
60 was flushing the whole panel every second.

**Technique.** Cache the last value written to each label and only call the setter
on a real change:

```cpp
static char last_clock[6] = "";
if (strcmp(last_clock, hhmm) != 0) {
  strncpy(last_clock, hhmm, sizeof(last_clock) - 1);
  lv_label_set_text(g_clock, hhmm);
}
```

**Applied in:** the launcher status bar (`launcher.cpp`, clock/battery/BLE/Wi‑Fi)
and the music now-playing screen (`music.cpp`, name/state/elapsed/total/volume/
mode), which also drops the poll from 250 ms → 1000 ms since the only sub-second
field is the mm:ss clock. Idle home-screen flushes fall from 1/s to ~1/min.

**Rules of thumb:**
- Seed caches to a force-first-paint sentinel so the first draw isn't suppressed.
- For values built from state, compose into a fixed `char` buffer with `snprintf`
  and compare — this also removes per-tick Arduino `String` heap churn.
- `lv_bar_set_value` already no-ops internally when unchanged; labels do not.

### 2. Split audio onto core 0

Audio decode and the I2S feed run in a dedicated FreeRTOS task pinned to core 0,
fed by a command queue from the UI (`music_init` in `music.cpp`). The UI's ~12 ms
flushes and LittleFS reads happen on core 1 and therefore **cannot** starve the
sample feed. This is the difference between glitch-free MP3 and periodic dropouts.
See [architecture.md](architecture.md#the-two-core-task-model).

### 3. `direct_mode` rendering

LVGL renders only invalidated regions into a persistent full-screen framebuffer,
instead of re-rendering all ~120 k pixels every refresh. Measured **134 ms →
21 ms (6.4×)** on common updates. Details in
[display-pipeline.md](display-pipeline.md#direct_mode-is-on-deliberately). This is
already on; do not turn it off.

### 4. Dirty-check before writing to flash

**Problem.** The Notes and Journal editors autosave every 3 s by rewriting the
whole file — a LittleFS truncate + reprogram — regardless of whether anything
changed. An editor left open while the device idles into its desk clock (which
does not tear the editor down) would rewrite the same file every 3 s
*indefinitely*: real flash wear, ~28,800 writes/day.

**Technique.** Cache the last-saved content and skip the write when unchanged:

```cpp
if (full != g_last_saved) { write_note(id, full.c_str()); g_last_saved = full; }
```

**Why content-comparison and not a dirty flag:** the notes *title* textarea and
the journal *body* textarea have no `VALUE_CHANGED` handler to hang a flag on, so a
flag would miss those edit sources and silently drop edits. Content comparison
can't miss a source. Seed the cache with the loaded content on open so an
untouched editor writes nothing.

### 5. Keep mutexes off the slow path

**Problem.** The Music browser held `g_mux` across a full SD `/music` directory
scan (walk + sort + unique). The core-0 audio task takes the same mutex on every
track boundary. If a track auto-advanced while the browser was reloading, the
audio task blocked for the entire SD scan → DMA underrun → audible dropout, on the
one feature whose whole point is glitch-free playback.

**Technique.** Do the slow work **outside** the lock; publish with an O(1) swap
under a brief lock:

```cpp
std::vector<String> scanned;
scan_tracks(scanned);                 // slow SD walk, no lock held
xSemaphoreTake(g_mux, portMAX_DELAY);
g_tracks.swap(scanned);               // O(1); the audio task waits microseconds
xSemaphoreGive(g_mux);
```

**General rule:** every `g_mux` critical section must be O(microseconds). Never
hold it across SD/LittleFS I/O, sorting, or allocation.

### 6. Cache repeated I/O; hold handles open

Small wins that add up on interactive paths:

- **Reader** holds the book's `File` handle open for the whole reading session
  (opened once in `open_book`, `seek`+`read` per page) instead of re-opening —
  and re-walking the FAT directory — on every page-turn keystroke. Close it on
  *every* exit path.
- **Reader** reads `/reader_pos.txt` **once** into a map when building the book
  list, instead of re-opening and scanning it once per row.
- **Notes** search folds the body match into the single directory pass in
  `list_notes(filter)` — reading each file once for title + match — instead of
  opening every file a second time.

### 7. Input latency

On a keyboard-only device, key-to-response latency *is* the responsiveness metric.
Two levers:

- `LV_INDEV_DEF_READ_PERIOD` is overridden to **12 ms** in `include/lv_conf.h`
  (LVGL's default is 30 ms). A keystroke needs two poll cycles (PRESS then
  RELEASE), so the default cost ~60 ms; the read callback is a cheap ring-buffer
  pop, so tightening it is nearly free and roughly halves felt latency.
- The main loop runs `lv_timer_handler()` every ~5 ms, so display refresh is not
  the bottleneck — the read period was.

### 8. Power & deep sleep

The device is battery-first. The model (`power.cpp`):

- Idle → a **standby dashboard** (clock + agenda + artwork) → **ESP32 deep
  sleep**. Deep sleep resets the chip, so "waking" is a fast, minimal reboot.
- The panel is put in **low-power self-refresh** first, so the dashboard stays
  visible at ~zero power while the MCU is off.
- A ~60 s **timer wake** checks for due reminders; if none and on battery, it
  re-sleeps **before** the panel or heavy init runs (see the boot ordering in
  [architecture.md](architecture.md#boot-sequence)).
- **KEY (GPIO18)** is the EXT1 wake source. GPIO0 is deliberately *not* a wake
  source — it's a strapping pin.
- The silent-poll wake skips the 200 ms USB-CDC settle delay (gated on wake cause
  ≠ TIMER); on a ~1.4 s active window every 60 s, 200 ms of pure spin was ~14% of
  the standby power budget.

**Hard constraint:** `setCpuFrequencyMhz()` **hangs** when BLE is active, so CPU
frequency scaling is off the table — the chip runs 240 MHz fixed while awake.
Don't reach for DVFS as a power lever here.

---

## Things that look like optimizations but aren't (rejected)

An adversarial review surfaced these and correctly rejected them. Recorded so they
aren't "rediscovered":

- **Move the framebuffer to PSRAM.** Wrong. `g_fb` is hot (touched per-pixel by
  `set_px_cb`, streamed by SPI DMA) and belongs in low-latency internal SRAM.
  PSRAM is for cold media buffers. It is correctly in SRAM today.
- **De-duplicate the inverted launcher-icon bitmaps.** Not worth it — the inverted
  twins total ~272 bytes and buy instant focus highlighting with zero redraw.
- **Cache the reminder `/events` scan / reduce battery ADC sampling.** These paths
  are cold or cheap enough that the added state isn't justified.
- **CPU frequency scaling for power.** Impossible while BLE is up (it hangs).

And two "obvious" fixes that would have introduced **bugs**, kept only in their
safe form:

- **Music: update volume/index/mode labels in the key handlers instead of
  polling.** Unsound — `g_index`/`g_name` change via auto-advance on the *core-0*
  audio task, and `g_volume`/`g_shuffle`/`g_repeat` in `handle_cmd` on core 0
  after the UI queues the command. Reading them in the UI key handler races and
  freezes the track name after auto-advance. The poll is kept; only diffing was
  added.
- **Autosave: a dirty flag set in the body-changed handler.** Incomplete — it
  misses the title/body sources with no `VALUE_CHANGED` handler. Content
  comparison is used instead (technique #4).

---

## How the review was done (method, for next time)

The optimization pass was run as a multi-agent **adversarial review**, and the
method is worth reusing because it produced signal without noise:

1. **Map** the perf-critical surfaces (render, tasks, storage, memory) into a
   shared brief so finders don't re-derive the architecture.
2. **Find** — one agent per dimension (render, task/concurrency, storage I/O,
   RAM/heap, PSRAM/flash, CPU/algorithmic, power, BLE/audio), each producing
   candidate findings with an exact `file:line`, evidence, a proposed fix, and an
   honest impact estimate.
3. **Verify adversarially** — two skeptics per finding: a *reality* check ("is
   this actually hot on this hardware, or premature?") and a *soundness* check
   ("would the fix be correct given the 1‑bit panel / core split / deep sleep /
   BLE?"). Default to rejection.

Result: **28 candidates → 13 survived → 9 distinct fixes**, none rated "high"
after scrutiny — a signal that the codebase was already well-built, not that the
review was weak. Crucially, the verify step caught the two unsound "fixes" above
before they shipped.

The finding-level detail (evidence, verdicts, and the full rejection log) is kept
in the repo-local, git-ignored `perf-review/` folder rather than committed —
useful as a record, not as source.

### The meta-lesson

The reflective panel has no backlight and the CPU is pinned at 240 MHz under BLE,
so **absolute power headroom is small and the biggest wins are about doing less
work, not doing work faster**: don't invalidate what didn't change, don't write
flash that didn't change, don't hold a lock across I/O, don't burn CPU on a path
that's about to sleep. Optimize frequency and avoidance, not micro-throughput.
