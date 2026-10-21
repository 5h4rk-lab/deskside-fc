/**
 * LVGL 9.2 configuration for the desktop simulator.
 *
 * Deliberately minimal: anything not set here takes the LVGL default from
 * lv_conf_internal.h. The ESP32 firmware config must match the items marked
 * [FW] so the UI renders and behaves identically on the device:
 *   - LV_COLOR_DEPTH 16 (RGB565, same as the ILI9341-class panel)
 *   - LV_FONT_FMT_TXT_LARGE 0 (the generated .c files in ui/fonts assume the default)
 *   - LV_USE_TABVIEW, LV_USE_SNAPSHOT, LV_USE_LODEPNG, LV_USE_TJPGD enabled
 *   - LV_USE_KEYBOARD (needs LV_USE_BUTTONMATRIX), LV_USE_TEXTAREA,
 *     LV_USE_SLIDER, LV_USE_SWITCH enabled (Settings and the team picker).
 *     LV_USE_SPINNER (needs LV_USE_ARC) enabled (Wi-Fi scanning and joining).
 *     They default to 1 here but to 0 under ESP-IDF Kconfig: set them there.
 *   - LV_FONT_MONTSERRAT_14 enabled (default theme font and LV_SYMBOL_* glyphs)
 *   - LV_USE_DRAW_SW_COMPLEX_GRADIENTS 1 (radial shading on the Live screen)
 *   - LV_USE_FS_* is NOT needed by the UI; the helper serves images as bytes
 * Everything else (SDL, stdlib allocator, log) is simulator-only.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* [FW] 16-bit colour: matches the RGB565 panel on the device. */
#define LV_COLOR_DEPTH 16

/* Desktop: LVGL allocates from the system heap (malloc/free), not from a fixed
 * pool. Decoded images (ui/src/lvgl_image_decoder.cpp takes their pixels from
 * lv_malloc) are limited by the cache budget, not by a pool size, and malloc is
 * thread safe and visible to ASan. The firmware has its own heap hooks
 * (firmware/app/src/board/lvgl_alloc.cpp: PSRAM for big blocks). */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CUSTOM  /* sim/sim_alloc.cpp: counted, can fail on demand */
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

/* The UI runs on a single thread; ticks come from SDL_GetTicks in main.cpp. */
#define LV_USE_OS LV_OS_NONE

/* Simulator logging to stdout, warnings and above. */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* [FW] Widgets used by the UI. */
#define LV_USE_TABVIEW 1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_KEYBOARD 1
#define LV_USE_TEXTAREA 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_ARC 1
#define LV_USE_SPINNER 1

/* [FW] Image decoders: PNG (lodepng) and JPEG (tjpgd) for crests and photos. */
#define LV_USE_LODEPNG 1
#define LV_USE_TJPGD 1

/* [FW] lv_snapshot_take(): used by tests and the screenshot helper. */
#define LV_USE_SNAPSHOT 1

/* [FW] Radial gradients: the Live screen's vignette over the team colours.
 * Without it the UI falls back to a flat shade. */
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 1

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
