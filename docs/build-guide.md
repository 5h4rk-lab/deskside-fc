# Build guide

From an empty machine to a working display. You can stop after step 2 and have a working simulator. Everything that needs the board is marked.

Contents: [1. Tools](#1-install-the-tools) · [2. Clone and test](#2-clone-and-run-the-simulator-tests) · [3. Helper](#3-deploy-the-helper) · [4. Configure](#4-configure-the-firmware) · [5. Flash](#5-first-flash-over-usb) · [6. Provision](#6-provision-the-helper-url-and-token) · [7. Wi-Fi and team](#7-pick-wi-fi-and-your-team) · [8. Updates](#8-publish-an-over-the-air-update) · [Serial console](#serial-console-words) · [Troubleshooting](#troubleshooting)

## 1. Install the tools

| Tool | Needed for | Notes |
|---|---|---|
| Git | cloning | |
| cmake, a C++17 compiler, SDL2 and libcurl development packages | simulator and tests | e.g. `cmake`, `g++`, `sdl2`, `curl` dev packages on your distribution |
| Node.js 22 or newer and npm | helper | the helper declares `node >= 22` |
| PlatformIO Core (`pio`) | firmware | `pipx install platformio` or the VS Code extension. The first build downloads the platform `espressif32@8.0.0`, Arduino-ESP32 3.3.12 and the libraries (internet needed). |
| Vercel CLI and a Vercel account | deploying the helper | the free tier was enough for this project |
| `python3` | `tools/publish-firmware.sh` | |

## 2. Clone and run the simulator tests

```sh
git clone https://github.com/YOUR-NAME/YOUR-REPO.git soccer-display
cd soccer-display
cmake -S sim -B build/sim && cmake --build build/sim -j
ctest --test-dir build/sim --output-on-failure     # 976 doctest cases, headless
./build/sim/sim                                    # open the simulator (demo data)
```

The first configure downloads LVGL 9.2.2, ArduinoJson 7.4.2 and doctest 2.4.12. The helper's tests:

```sh
cd helper && npm ci && npx vitest run && cd ..
```

Options for the simulator (headless screenshots, fake Wi-Fi, battery, sound, update) are in [sim/README.md](../sim/README.md).

## 3. Deploy the helper

The helper is a Vercel project in `helper/` (TypeScript, Node 22). Every endpoint requires the `x-token` header, whose value is a secret you choose.

**Make a strong token without showing it.** Create a private file and put a random token in it. Nothing below prints the token:

```sh
umask 077
mkdir -p ~/.config/soccer-display
printf 'HELPER_TOKEN=%s\n' "$(openssl rand -hex 32)" > ~/.config/soccer-display/helper.env
chmod 600 ~/.config/soccer-display/helper.env
```

This is also the file that `provision-helper.sh` reads in step 6, and the script refuses it unless its mode is 600 or stricter. Never commit it or paste it into chats, issues or screenshots.

Deploy:

```sh
cd helper
vercel link --yes --project YOUR-HELPER-NAME
. ~/.config/soccer-display/helper.env
printf %s "$HELPER_TOKEN" | vercel env add HELPER_TOKEN production --sensitive
unset HELPER_TOKEN
vercel deploy --prod --yes
```

`printf` is a shell builtin, so the token never appears in a process list. If your CLI version does not accept the value from standard input, run `vercel env add HELPER_TOKEN production --sensitive` and paste the value at its prompt (it is hidden).

Your helper's address will look like `https://YOUR-HELPER.vercel.app`. Run the smoke test, which reads the token from the environment:

```sh
(. ~/.config/soccer-display/helper.env && HELPER_TOKEN="$HELPER_TOKEN" scripts/smoke.sh https://YOUR-HELPER.vercel.app)
```

It also checks that a request without the token is refused with 401. More detail about endpoints, caching and limits: [helper/README.md](../helper/README.md).

## 4. Configure the firmware

Normally nothing needs to be configured in files. The helper URL and token go into the device's NVS flash in step 6.

**Optional:** to bake build-time defaults into an image, copy `firmware/app/secrets.ini.example` to `firmware/app/secrets.ini` and fill it in. Values saved in NVS win over these defaults. That file is git-ignored; **never commit it**, and never publish or give away an image built with it, because the image then contains the token.

### Check the defaults when you fork

The author's own helper host appears as a default in two places: `firmware/app/tools/provision-helper.sh` (used when you pass no `--url` and the env file has no `HELPER_URL`) and `kDefaultHelperHost` in `ui/src/update_check.h` (a default for the update rules; the firmware builds its real rules from the helper address stored on the device). Always pass `--url https://YOUR-HELPER.vercel.app` to the provisioning script, and consider replacing both defaults in your fork. The tests and docs also use that host as sample data.

## 5. First flash over USB

Connect the board with a data-capable USB-C cable. Leave the battery disconnected for the first flash (a precaution). If you have a battery, [check its polarity first](hardware.md#battery-wiring-check-the-polarity-first).

```sh
cd firmware/app
pio run -e app               # build only
pio run -e app -t upload     # build and flash
```

The environments:

| Env | Use |
|---|---|
| `app` | release; the one you give away |
| `app_diag` | release plus measurement hooks. **Never flash it onto a device you hand over**: any serial byte can inject a touch. |
| `app_debug` | `-Og` with debug symbols and the measurement hooks |
| `app_demo` | built-in demo data and a fixed demo clock; no helper requests |

On Linux you may need serial permissions: add yourself to `dialout` (Debian, Ubuntu) or `uucp` (Arch, Manjaro), then log out and back in.

If the board does not appear as a serial port, force download mode: hold the BOOT button, power cycle the board, flash, then power cycle again.

Open the console with `pio device monitor -b 115200`. You should see a build line within a few seconds. **Opening the serial port resets this board**, so you will see a fresh boot each time.

A plain `upload` leaves NVS alone, so saved Wi-Fi networks, settings and the helper config survive reflashing.

## 6. Provision the helper URL and token

Close any serial monitor first (the script refuses if something holds the port), then:

```sh
cd firmware/app
./tools/provision-helper.sh --url https://YOUR-HELPER.vercel.app --restart
```

What it does:

- reads `HELPER_TOKEN` from `~/.config/soccer-display/helper.env` (change with `--env-file`), and refuses the file unless its mode is 600 or stricter;
- sends `:set-url` and `:set-token` to the board over the serial port with shell builtins, so the token is **never echoed, never in a command line, never in a log**;
- sends `:show-config` and checks that the board shows only a masked token (first 4 characters and a length), then prints `OK` lines;
- with `--restart`, restarts the board so the new values are used.

If the board ever prints the token back, the script aborts and shows nothing of the output. By hand, in a serial terminal, the same words are `:set-url <url>`, `:set-token <token>`, `:show-config`, `:restart`.

The URL must be https, have no user info and be at most 200 characters; the token must be 16 to 128 printable characters without spaces. A value that fails these checks is never used, and then the device makes no requests.

## 7. Pick Wi-Fi and your team

1. On first start with no saved network, the guided Wi-Fi setup opens. Pick your network and type the password on the on-screen keyboard. Open and WPA2-Personal networks only. You can save several networks; the strongest known one is joined automatically.
2. Within about 10 to 14 seconds of joining you should see real data (the clock must sync first).
3. Settings is behind the gear at the top right of the Live tab. Open it to pick your favourite team (top five European leagues), the time zone, brightness, night dimming and the sound options.
4. Until a team is picked, the default favourite is Arsenal.

## 8. Publish an over-the-air update

Updates come from your helper. Devices check about 2 minutes after start, then every 6 hours, and when Settings is opened; they install only when the user taps "Version X available - Install".

1. Raise `APP_VERSION_TEXT` in `ui/src/version.h` and commit all firmware and UI changes (the script refuses a dirty tree).
2. Make sure `firmware/app/secrets.ini` is absent or does not set `HELPER_URL` or `HELPER_TOKEN`, and that `PLATFORMIO_BUILD_FLAGS` is unset.
3. From the repository root: `tools/publish-firmware.sh --notes "One line"`. It builds the release `app`, checks the image (size, project name `soccer-display`, version), copies it to `helper/public/firmware/<version>.bin` and rewrites `helper/firmware-manifest.json`.
4. It **deploys nothing**. Run `cd helper && npx vitest run` (it checks the manifest against the file), commit both files and deploy the helper as in step 3. Afterwards `curl -sI https://YOUR-HELPER.vercel.app/firmware/<version>.bin` should answer 200 with the right Content-Length.

A new image has to confirm itself. If the board restarts before it gets first data (or 60 seconds of running), it reverts to the old image. Wait for the Settings row "Updated to X" before restarting a freshly updated device. Keep older binaries in `helper/public/firmware/`.

A real update over HTTPS from a deployed helper, a power cut during an update, and a Wi-Fi drop mid-download have not been tested on hardware.

## Serial console words

Connect at 115200 baud. One-letter commands act at once; word commands start with `:` and run when you press Enter. While you type a word its letters are swallowed, so typing cannot trigger a letter command.

Release builds (`app`):

| Word | What it does |
|---|---|
| `g` | goal celebration test for the favourite (live data is not touched) |
| `w` | full-time win celebration test |
| `d` | fetch everything now |
| `s0` `s1` `s2` | show the Live, Matches or News tab |
| `t` | make the next team in the list the favourite |
| `p`, `k`, `f`, `m` | print settings, NVS self-test, toggle one-second perf lines, memory report |
| `:status` | uptime, reset reason, crash counter, safe mode, version, memory, Wi-Fi, battery, update state (nothing secret) |
| `:heap` | memory report |
| `:wifi` | connection, signal, address, saved networks |
| `:wipe-wifi` | forget every saved network and restart |
| `:wipe-settings` or `:erase-settings` | erase settings (not Wi-Fi, not the helper) and restart |
| `:reboot` or `:restart` | software restart |
| `:safe-mode`, `:safe-mode off` | show the crash counter; leave safe mode |
| `:set-url <url>`, `:set-token <t>`, `:show-config` | helper config (token never echoed; first 4 characters shown) |
| `:net` | data path and picture counters |
| `:sound <name> [v]` | play goal, win, kickoff, fulltime, low, critical or wifi; `:sound tone l`, `r` or `both`; `:sound stop` |
| `:power` | battery ADC, voltage, model, power key, power hold |
| `:cal-battery <mV>`, `clear` | calibrate the battery reading to a multimeter's value (3000 to 4400) |
| `:ota` | update state: slots, trial record, manifest address |
| `:help` | list the commands |

Diagnostics builds (`app_diag`, `app_debug`) add `h j n b0..b2 =xxxyyy` (injected swipes and taps), `y`, `x0..x5`, `S`, `W`, `Z`, and `:diag-` words (clock break and restore, burst and churn tests, offline, power-off dry run, key, battery, watchdog stall, panic, backlight sweep, CPU shares, screen shot, picker, image failure) plus `:ota-url`, `:ota-check`, `:ota-install`. See the header comment of `firmware/app/src/main.cpp` for each.

## Troubleshooting

| Symptom | Likely cause and what to do |
|---|---|
| Board resets when you open the serial monitor | Expected: opening the USB serial port resets this board. It is not counted as a crash. |
| No serial port appears | Use a data cable. Force download mode (hold BOOT while power cycling). Check group membership (`dialout` or `uucp`). |
| `provision-helper.sh` says the port is in use | Close the serial monitor or any `pio` process holding it. |
| `provision-helper.sh` complains about the file mode | `chmod 600 ~/.config/soccer-display/helper.env`. |
| Screen says "Connecting..." for a while after power-up | Normal for the first seconds while Wi-Fi joins and the clock syncs. The clock needs about 9 to 10 s after boot in the measurements. |
| "Waiting for the clock" | Wi-Fi is up but SNTP has not given a valid time (year 2024 or later). No requests are made until it does. A refused bad sync is retried 30 s later. |
| "No Wi-Fi connection" | The link is down. Check the router, or add a network in Settings. |
| "Cannot reach the helper" | DNS, TLS or network failure. Check the URL with `:show-config` and that the helper deploy is up. Old data stays visible with an "Updated N min ago" stale marker. |
| "Helper rejected the token" | The device's token differs from the `HELPER_TOKEN` env var on Vercel (401). Re-run provisioning, then restart. |
| "Helper returned an error" or "Helper sent unreadable data" | The helper answered with an error status or something that is not valid JSON. Check the helper's logs. ESPN may have changed. |
| "Waiting for the first update..." | Connected and configured but no successful matches answer yet. |
| Team picker stays on "loading" | No helper is configured on the device yet. Run step 6. |
| Banner "Safe mode: updates paused" | Three abnormal resets (panic, watchdog, brownout) within 10 minutes of running. Data, pictures and updates are paused; Settings, Wi-Fi and the power key work. It ends after 10 minutes of running, or with `:safe-mode off`. Use `:status` to see the crash list and the last core dump summary. |
| An update "did not start" and the old version came back | The new image restarted before confirming (a rollback). The window is until first data or 60 s of running. Do not restart during it. |
| Wrong colours, mirrored or swapped touch | Check the board revision and the `[touch]` and `[display]` lines. V1 was never run. |
| Battery percentage looks wrong | Calibrate with `:cal-battery <mV>` against a multimeter. Charging is inferred from voltage and can be wrong. |
| Sound is silent | Check the volume knob on the board, the Sound setting (default "Goals only"), the night mute, and try `:sound goal 100`. Sound quality was never checked by ear. |
| After a crash in the release build there is no core dump | By design: the release build erases it after printing a summary because it could contain the token. Use `app_diag` to keep dumps. |
