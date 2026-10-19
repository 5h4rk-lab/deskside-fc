// Desktop simulator: runs the shared UI code in an SDL window.
// Usage: sim [--scale N]   (window is 240*N x 320*N, LVGL stays 240x320)
#include <SDL2/SDL.h>
#include <lvgl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app.h"

namespace {

constexpr int kWidth = 240;
constexpr int kHeight = 320;
constexpr int kDefaultScale = 2;

uint32_t tick_ms() { return SDL_GetTicks(); }

int parse_scale(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
      const int s = std::atoi(argv[i + 1]);
      if (s >= 1 && s <= 8) return s;
      std::fprintf(stderr, "invalid --scale '%s' (1-8)\n", argv[i + 1]);
      std::exit(2);
    }
    if (std::strcmp(argv[i], "--help") == 0) {
      std::printf("usage: sim [--scale N]\n");
      std::exit(0);
    }
  }
  return kDefaultScale;
}

}  // namespace

int main(int argc, char** argv) {
  const int scale = parse_scale(argc, argv);

  lv_init();
  lv_tick_set_cb(tick_ms);

  lv_display_t* disp = lv_sdl_window_create(kWidth, kHeight);
  if (!disp) {
    std::fprintf(stderr, "could not create SDL window\n");
    return 1;
  }
  lv_sdl_window_set_title(disp, "Soccer display simulator");
  lv_sdl_window_set_zoom(disp, static_cast<float>(scale));
  lv_sdl_mouse_create();

  app_create(lv_screen_active());

  for (;;) {
    lv_timer_handler();
    SDL_Delay(5);
  }
}
