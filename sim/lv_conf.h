/**
 * LVGL 9.2 configuration for the desktop simulator.
 *
 * Deliberately minimal: anything not set here takes the LVGL default from
 * lv_conf_internal.h. The ESP32 firmware config must match the items marked
 * [FW] so the UI renders and behaves identically on the device:
 *   - LV_COLOR_DEPTH 16 (RGB565, same as the ILI9341-class panel)
 *   - LV_FONT_FMT_TXT_LARGE 0 (the generated ui/fonts/*.c assume the default)
 *   - LV_USE_TABVIEW, LV_USE_SNAPSHOT, LV_USE_LODEPNG, LV_USE_TJPGD enabled
 *   - LV_FONT_MONTSERRAT_14 enabled (default theme font and LV_SYMBOL_* glyphs)
 *   - LV_USE_FS_* is NOT needed by the UI; the helper serves images as bytes
 * Everything else (SDL, stdlib allocator, log) is simulator-only.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* [FW] 16-bit colour: matches the RGB565 panel on the device. */
#define LV_COLOR_DEPTH 16

/* Desktop: 4 MB built-in pool (decoded images, snapshots). The firmware
 * will size its own pool from PSRAM. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN
#define LV_MEM_SIZE             (4 * 1024U * 1024U)

/* The UI runs on a single thread; ticks come from SDL_GetTicks in main.cpp. */
#define LV_USE_OS LV_OS_NONE

/* Simulator logging to stdout, warnings and above. */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* [FW] Widgets used by the UI. */
#define LV_USE_TABVIEW 1

/* [FW] Image decoders: PNG (lodepng) and JPEG (tjpgd) for crests and photos. */
#define LV_USE_LODEPNG 1
#define LV_USE_TJPGD 1

/* [FW] lv_snapshot_take(): used by tests and the screenshot helper. */
#define LV_USE_SNAPSHOT 1

/* [FW] Default theme font and the LV_SYMBOL_* icon glyphs. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Simulator only: SDL2 window and mouse. */
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_ACCELERATED 1
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 1

#endif /* LV_CONF_H */
