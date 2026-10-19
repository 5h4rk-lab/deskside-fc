# Simulator

Runs the shared `ui/` code in a 240x320 SDL window. Needs cmake, a C++17 compiler, SDL2 and libcurl dev packages.

- Build: `cmake -S sim -B build/sim && cmake --build build/sim -j`
- Run: `./build/sim/sim` (mouse acts as touch; `--scale N` enlarges the window, default 2, LVGL stays 240x320). Shows the demo data (a live Arsenal v Chelsea and the other mockup matches); `--fixtures` loads `contract/fixtures` instead, `--assets DIR` changes the image directory (default `sim/assets`).
- Screenshot: `./build/sim/sim --snapshot out.png` renders once without a window and writes a 240x320 PNG.
- Test: `ctest --test-dir build/sim --output-on-failure` (headless, no window needed)
- Fonts: `ui/fonts/gen.sh` regenerates the committed font C files (needs node and python3).
