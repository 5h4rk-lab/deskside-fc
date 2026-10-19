# Simulator

Runs the shared `ui/` code in a 240x320 SDL window. Needs cmake, a C++17 compiler, SDL2 and libcurl dev packages.

- Build: `cmake -S sim -B build/sim && cmake --build build/sim -j`
- Run: `./build/sim/sim` (mouse acts as touch; `--scale N` enlarges the window, default 2, LVGL stays 240x320)
- Test: `ctest --test-dir build/sim --output-on-failure` (headless, no window needed)
- Fonts: `ui/fonts/gen.sh` regenerates the committed font C files (needs node and python3).
