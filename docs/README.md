# express-ghalib — technical documentation

Engineering reference for the firmware. If you're extending it — a new app, a new
peripheral, or anything that touches the screen or the power model — start here.

The project's guiding constraint is a **slow, 1‑bit, reflective panel driven by a
dual-core MCU that must also host BLE, decode audio, and never stutter while
typing.** Almost every non-obvious decision in the codebase traces back to that,
and these docs exist to make those decisions legible before you accidentally undo
one.

## Read in this order

1. **[architecture.md](architecture.md)** — the shape of the system. Boot
   sequence, the two-core task split (UI on core 1, audio on core 0), how apps
   and screens are created/torn down, and where data lives. Read this first;
   everything else assumes it.

2. **[display-pipeline.md](display-pipeline.md)** — how a pixel gets to the
   panel. The ST7305's non-linear packing, LVGL's `direct_mode` + `set_px_cb`,
   the single full-frame flush, and the *cost model* — why a redraw costs what it
   costs. This is the hot path.

3. **[performance.md](performance.md)** — the optimization playbook. The
   established techniques (core-split audio, diff-before-write, dirty-check
   autosave, I/O caching, deep sleep), what was measured, and the adversarial
   review method used to find waste without inventing it. Read before optimizing
   anything — several "obvious" optimizations here are wrong.

4. **[hardware.md](hardware.md)** — the board. Full pin map, every peripheral,
   the hardware facts you must design around, and the flash partition layout.

5. **[build-flash-debug.md](build-flash-debug.md)** — the mechanics. Toolchain,
   the USB‑OTG flashing dance, capturing serial without hanging your terminal,
   and pulling + decoding a crash coredump.

## The one-paragraph mental model

`main.cpp` sets up an LVGL display whose draw buffer **is** the ST7305's native
framebuffer; a per-pixel callback packs each drawn pixel into the panel's odd
2×4 layout, and one flush pushes the whole 15 KB frame over SPI (~12 ms). The UI
runs cooperatively in `loop()` on core 1 via `lv_timer_handler()`. Audio decode
runs in its own FreeRTOS task pinned to core 0, fed by a queue, so a full-screen
flush can never starve playback. Apps are self-contained modules that build LVGL
screens, register a keypad group, and install a teardown hook with the launcher.
Idle leads to a standby dashboard and then ESP32 deep sleep, from which "waking"
is a fast reboot. Notes/config persist on internal LittleFS; music, books, and
the dictionary live on the SD card.
