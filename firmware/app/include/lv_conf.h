/**
 * LVGL 9.2 configuration for the ESP32-S3 firmware.
 *
 * Derived from sim/lv_conf.h: the items marked [FW] there are the ones the
 * firmware must match so the UI renders and behaves identically on the
 * device. Anything not set here takes the LVGL default from
 * lv_conf_internal.h. Built through PlatformIO with -DLV_CONF_INCLUDE_SIMPLE
 * and -I include (see platformio.ini).
 *
 * Differences from the simulator: no SDL window, no snapshot, no file system
 * drivers, no logging (nothing LVGL prints can leak a secret), the heap lives
 * in PSRAM/internal RAM through hooks in src/board/lvgl_alloc.cpp, and ticks
 * come from lv_tick_set_cb().
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* [FW] 16-bit colour: matches the RGB565 panel on the device. */
#define LV_COLOR_DEPTH 16

/* LVGL's heap: our own allocator hooks (lvgl_alloc.cpp). Requests of 2 KB and
 * above (decoded images, draw buffers) go to PSRAM, smaller ones to internal
 * RAM, which the Wi-Fi and LwIP stacks also need. LV_MEM_SIZE is unused with
 * a custom allocator. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
/* Same formatter as the simulator, so numbers render identically. */
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

/* One LVGL task, no OS abstraction. Ticks: lv_tick_set_cb() in main. */
#define LV_USE_OS LV_OS_NONE

/* No LVGL logging on the device. LV_LOG_* compile to nothing. */
#define LV_USE_LOG 0

/* A failed LVGL assertion (null pointer, failed allocation inside LVGL)
 * restarts the board with a backtrace instead of hanging in while(1) until
 * the watchdog bites. */
#define LV_ASSERT_HANDLER_INCLUDE <stdlib.h>
#define LV_ASSERT_HANDLER abort();

/* [FW] Widgets used by the UI. They default to 0 under ESP-IDF Kconfig,
 * so they are set explicitly. */
#define LV_USE_TABVIEW 1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_KEYBOARD 1
#define LV_USE_TEXTAREA 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_ARC 1
#define LV_USE_SPINNER 1

/* [FW] Image decoders: PNG (lodepng) and JPEG (tjpgd) for crests and photos.
 * They decode from memory; no file system is involved. */
#define LV_USE_LODEPNG 1
#define LV_USE_TJPGD 1

/* The simulator's snapshot helper is not needed on the device. */
#define LV_USE_SNAPSHOT 0

/* [FW] Radial gradients: the Live screen's vignette over the team colours. */
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 1

/* [FW] Default theme font and the LV_SYMBOL_* icon glyphs. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* No SDL, no file system drivers: they default to 0 and are left out. */

#endif /* LV_CONF_H */
