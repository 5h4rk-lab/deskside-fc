# Simulator

Runs the shared `ui/` code in a 240x320 SDL window. Needs cmake, a C++17 compiler, SDL2 and libcurl dev packages.

- Build: `cmake -S sim -B build/sim && cmake --build build/sim -j`
- Run: `./build/sim/sim` (mouse acts as touch; `--scale N` enlarges the window, default 2, LVGL stays 240x320). Shows the demo data (a live Arsenal v Chelsea and the other mockup matches); `--fixtures` loads `contract/fixtures` instead, `--assets DIR` changes the image directory (default `sim/assets`).
- Screenshot: `./build/sim/sim --snapshot out.png` renders once without a window and writes a 240x320 PNG. `--screen matches|news` shows that tab and `--screen detail|summary|pitch|stats` opens the match detail first (default `live`), and `--match ID` picks the match (default: the live demo match; `demo-rma-bar` is a pre-match without lineups).
- Goal celebration: in the window, `g` makes the favourite score in its live demo match and `w` ends that match with the favourite ahead (full-time win); both run the whole path (source, poll, goal detector, celebration). `--demo-goal` scores automatically 6 s after start, `--reduced-motion` shows the static version. `--screen goal|goal-win` with `--snapshot` renders the celebration after `--at-ms N` ms of animation (default 900).
- Settings: the gear at the top right of the Live tab opens Settings. Choices are saved to `~/.config/soccer-display-sim/prefs.txt` (or `--prefs FILE`; `--prefs memory` keeps them for one run, `--reset-prefs` starts clean). Snapshots use memory prefs unless `--prefs` is given. `--tz ZONE` sets the time zone (otherwise the first run picks the zone matching the computer) and `--now EPOCH` fixes the clock. `--screen settings|zone|picker|picker-search` renders Settings, the zone list, the team picker, or the picker with "ars" typed and the keyboard up. Brightness and night dimming darken the window with an overlay.
- Test: `ctest --test-dir build/sim --output-on-failure` (headless, no window needed)
- Fonts: `ui/fonts/gen.sh` regenerates the committed font C files (needs node and python3).
