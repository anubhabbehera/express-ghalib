/**
 * lv_conf.h — LVGL 8.3 config for express-ghalib
 * ST7305 reflective LCD: 300x400, 1-BIT MONOCHROME.
 *
 * Only the settings that differ from LVGL defaults are set here; everything
 * else falls through to lv_conf_internal.h defaults (all #ifndef-guarded).
 * Found via `-D LV_CONF_INCLUDE_SIMPLE -I include` in platformio.ini.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*==========================
 *   COLOR / MONOCHROME
 *==========================*/
/* 1-bit panel. LVGL renders into a packed buffer via set_px_cb (see main.cpp). */
#define LV_COLOR_DEPTH 1

/*==========================
 *   MEMORY
 *==========================*/
/* Use LVGL's internal allocator. 48 KB is generous for a 1bpp, static UI.
 * (Large media buffers go to PSRAM via ps_malloc in app code, not here.) */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)

/*==========================
 *   HAL / TICK
 *==========================*/
/* Drive LVGL's tick from Arduino millis() so we don't need lv_tick_inc(). */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF 130 /* ~4.2" at 300x400 */

/*==========================
 *   FEATURES
 *==========================*/
#define LV_USE_LOG 1
#if LV_USE_LOG
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#endif

/* Keypad/encoder navigation is our only input model — no pointer/touch. */
#define LV_USE_GROUP 1

/*==========================
 *   FONTS  (black/white, so keep it lean)
 *==========================*/
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1   /* launcher icons + titles */
#define LV_FONT_MONTSERRAT_48 1   /* standby dashboard clock */
// UX pass 2026-07-29: default is Pixel Operator (CC0 proportional pixel font,
// 16px grid) converted at 1bpp — pixel-perfect on this panel, unlike the
// antialiased Montserrat thresholded to 1-bit. Sizes: 16 (+FontAwesome
// symbols), Bold 16 (+FA, titles), 32 and 48 (2x/3x grid: reading + clock).
// Sources in tools/fonts/, generated C in src/fonts/ (lv_font_conv --bpp 1).
#define LV_FONT_CUSTOM_DECLARE                                              \
  LV_FONT_DECLARE(pixel_operator_16)                                        \
  LV_FONT_DECLARE(pixel_operator_bold_16)                                   \
  LV_FONT_DECLARE(pixel_operator_32)                                        \
  LV_FONT_DECLARE(pixel_operator_48)
#define LV_FONT_DEFAULT &pixel_operator_16

/*==========================
 *   THEME  (mono)
 *==========================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 0
#define LV_USE_THEME_MONO 1 /* pure B/W theme suits a 1-bit reflective panel */

#endif /* LV_CONF_H */
