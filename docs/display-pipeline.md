# The display pipeline

The reflective ST7305 panel is the heart of the device and the source of most of
the firmware's non-obvious engineering. This document explains how a pixel gets
from LVGL to the glass, and — just as important — **what it costs**, because that
cost model drives every rendering decision in [performance.md](performance.md).

## The panel

- **Controller:** ST7305 (Waveshare's naming; Zephyr calls the same family
  ST7306, which adds 4-level grayscale). Driven here in **1-bit monochrome**.
- **Geometry:** natively **landscape 400×300**. The UI runs landscape to match;
  a portrait 300×400 layout would use LVGL software rotation.
- **Interface:** 4-wire SPI at **10 MHz** (`ST7305_SPI_HZ`). Waveshare's own LVGL
  demo drives it at 10 MHz, so that's proven safe. (An older note in `PLAN.md`
  says 1–2 MHz — the code and hardware say 10 MHz; trust the code.)
- **Reflective:** no backlight, e-paper-like. It refreshes slowly but **retains
  its image with the controller in low-power mode at ~zero power** — the basis of
  the standby dashboard.

Driver: `src/st7305.{h,cpp}`, ported from Waveshare's reference driver. Pins in
[hardware.md](hardware.md).

## The non-linear packing (why you can't just memcpy a bitmap)

The ST7305's framebuffer is **not** a linear row-major bitmap. Pixels are packed
in **2×4 blocks with the Y axis inverted** (MADCTL `0x48`). A byte does not
correspond to 8 horizontal pixels of one row. This is the single most important
fact about the panel:

> **Never hand this panel a plain horizontal bitmap.** Always set pixels through
> `st7305_buf_set(buf, x, y, on)`, which knows the packing. `on == true` is ink
> (black); `false` is background (white).

The full native framebuffer is `ST7305_BUF_BYTES = 400 × 300 / 8 = 15000` bytes.

## How LVGL is wired to it

Rather than let LVGL render into a generic buffer and then translate to the
panel's packing (a second full-frame pass), the driver makes **LVGL's draw buffer
be the ST7305 native framebuffer directly**, and does the packing per-pixel as
LVGL draws. Three pieces in `main.cpp`:

1. **The framebuffer is the draw buffer.**
   ```cpp
   static uint8_t g_fb[ST7305_BUF_BYTES];         // 15 KB, internal RAM
   lv_disp_draw_buf_init(&g_lv_draw_buf, g_fb, nullptr, ST7305_W * ST7305_H);
   ```

2. **`set_px_cb` packs each pixel as it's drawn.** LVGL calls this for every pixel
   it renders; it converts the mono theme's brightness to ink/background and
   writes it into `g_fb` in native packing:
   ```cpp
   static void mono_set_px_cb(..., lv_coord_t x, lv_coord_t y, lv_color_t color, ...) {
     st7305_buf_set(buf, x, y, lv_color_brightness(color) < 128);
   }
   ```

3. **`flush_cb` pushes the whole frame once.** Under `direct_mode`, `set_px_cb`
   has already written the changed pixels into the persistent `g_fb`; the flush
   just sends the entire frame on the last dirty area:
   ```cpp
   static void mono_flush_cb(lv_disp_drv_t* drv, ...) {
     if (lv_disp_flush_is_last(drv)) st7305_flush_full(g_fb);
     lv_disp_flush_ready(drv);
   }
   ```

### `direct_mode` is on, deliberately

```cpp
g_lv_disp_drv.direct_mode = 1;
```

`direct_mode` tells LVGL to render **only invalidated areas** into the full-screen
framebuffer, which persists between refreshes. Without it, LVGL would re-render
all ~120 k pixels every refresh through the (relatively expensive) per-pixel
`set_px_cb`. With it, only the pixels inside a dirty rectangle are re-packed. A
prior optimization pass measured this taking common UI updates **134 ms → 21 ms
(6.4×)**, and identified the per-pixel `set_px_cb` render — *not* the SPI flush —
as the dominant cost (~91%) of a typical update. That measurement is the reason
the render-frequency work in [performance.md](performance.md) matters more than
shaving the flush.

### Why the framebuffer stays in internal SRAM

`g_fb` is a static array in **internal** SRAM, even though the board has 8 MB of
PSRAM. This is correct and intentional: the framebuffer is touched constantly by
`set_px_cb` and streamed by the SPI DMA, and internal SRAM has far lower latency
and cleaner DMA behavior than PSRAM. An adversarial performance review explicitly
flagged and **rejected** "move the framebuffer to PSRAM" as a regression. PSRAM is
for large, cold media buffers, not the hot render target.

## The cost model (memorize this)

Two costs, and they are asymmetric:

| Operation | Cost | Scales with |
|---|---|---|
| **Render** (`set_px_cb` over a dirty area) | ~91% of a typical small update | the **area** invalidated |
| **Flush** (`st7305_flush_full`, full 15 KB over 10 MHz SPI) | **~12 ms**, blocking, on core 1 | fixed — always the whole frame |

Two consequences that drive the whole optimization strategy:

1. **The flush is all-or-nothing.** `mono_flush_cb` always pushes the *entire*
   framebuffer on any dirty area — a sub-window/partial flush was implemented and
   **reverted** because the panel's partial-refresh mode interacted badly with it
   (the byte math was correct; the panel wasn't). So *any* invalidation, however
   tiny, costs a full ~12 ms blocking SPI flush.

2. **Therefore, redraw *frequency* is the lever, not redraw *size*.** The cheapest
   pixel is the one you never invalidate. Since LVGL 8.3's `lv_label_set_text`
   **always** invalidates — even when the new text is byte-identical — any timer
   that unconditionally re-sets a label triggers a full flush every tick for no
   visual change. The fix used throughout the codebase is **diff before write**:
   cache the last value and only call the setter when it actually changed. See
   [performance.md](performance.md#1-diff-before-write-the-core-render-optimization).

## Standby / low power

Before deep sleep, `st7305_low_power()` switches the panel to Low Power Mode
(~1 Hz self-refresh); the last image — the standby dashboard — stays visible at
essentially zero power. The next `st7305_flush_full()` re-asserts High Power Mode
automatically. This is why "sleep" can show a clock: the MCU is off, but the panel
is still holding the frame the MCU drew before sleeping.
