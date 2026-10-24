# Performance

These are measurements taken on one board, in one place. They describe what this firmware did on that board and nothing else. They are not benchmarks and they are not compared with any other product, device or website; nothing comparable was measured.

## Methodology and caveats

- **Hardware:** one Waveshare ESP32-S3-Touch-LCD-2.8, V2 revision (CST3530 touch), on USB power from a PC, battery connected, on a desk.
- **Network:** one home Wi-Fi network, signal strength (rssi) of about -61 to -73 dBm. Results that depend on the network (Wi-Fi join, first data) will differ at another location. The SNTP clock sync, which gates the first request, varies from boot to boot.
- **Data:** the real helper, with 62 matches and 96 teams in its answers (the figure for the Matches answer is 31 KB of JSON).
- **Builds:** measured over the serial console with the diagnostics build (`app_diag`, `-O2` like the release) unless noted; the demo build (`app_demo` with `-DAPP_DIAG=1`) for the live-match screens, which needs a live match to exist.
- **Tools:** the one-second and five-second `[perf]` and `[status]` lines (frames, time inside `lv_timer_handler`), `[trace]` frame lists of scripted gestures (each entry is the time and the frame duration in ms), `:diag-cpu` (FreeRTOS run-time counters per task), `:diag-picker` (timed team picker build), `[http]` lines and `:diag-shot`. Scripted gestures stand in for a finger; the diagnostics build is needed for them.
- **"Before" and "after"** in the Task 11 table are the firmware before and after the performance work of that task (commit `34816bb` and the end of the task). Where "after" is blank, the number was measured once.
- **Not measured:** battery life, anything over several days, power draw, and how any sound actually sounds. A soak test (long unattended run) is not in these results.
- **Sources** are given per table. They are the project's hardware notes (`docs/superpowers/specs/hardware-waveshare-esp32-s3-touch-lcd-2.8.md`, section "Measured on the board") and the task reports kept by the maintainer.

## Summary table (end of Task 11)

Source: hardware notes, "Measured on the board: performance (2026-10-23, Task 11)".

| What | Before | After | Notes |
|---|---|---|---|
| Boot: `setup()` to first frame on the panel | 1375 ms (release) | 1426 ms (diag) | About 0.45 s of ROM and bootloader time comes before `setup()`. The backlight lights with the first frame. |
| Wi-Fi: radio ready / scan done / joined | 1.9 s / 4.5 s / 7.9 s after start | same | Saved network, rssi -61; one 2.6 s scan before the join. |
| Clock valid (SNTP) / first matches on screen | 11.6 s / 12.5 s after start | 9.7 to 13.9 s (first matches) | Depends on SNTP. |
| Goal celebration (live build, test key `g`, about 4 s) | 19.6 to 24.4 fps; `lv_timer_handler` is 91 to 98 % of the LVGL core; frames 35 to 50 ms on average; first frame 81 to 95 ms | same (not changed) | Demo build: 18 to 26 fps, first frame 102 ms, LVGL task 71.5 % of one core over a 5 s window. `kIdleFrameMs` stays 40 ms. |
| Live pulse (demo live match) | | 28 to 31 frames/s of the dot, 1.6 ms each; LVGL task 9.3 % of one core (both cores busy 11.4 % of 200 %) | |
| Idle, Live tab, no live match | 8.2 % of the LVGL core, one 20 to 23 ms call every second (Matches recomputed 62 rows: 17 ms) | 6.7 %, longest call 3.2 ms | Fix: Matches caches its rows. |
| Team picker, 96 rows | 1006 ms to build (one frame held 1.07 s); LVGL +149 KB, PSRAM -205 KB | Opens in 104 ms (first frame 168 ms) with 12 rows; the rest 3 rows per 30 ms (21 to 27 ms steps), complete after about 0.9 s; same memory when complete | Fixes: no layout per crest (560 ms), then progressive build. |
| First data on screen (62 matches arrive: Live, Matches, News built) | One LVGL call of 1070 ms (release; the loop task lagged 991 ms) | First call 327 ms (Live 78 ms, 8 Matches items, News 80 ms), then the Matches list fills 3 items per step (44 to 91 ms steps) for about 1.5 s | A Matches row costs about 15 to 18 ms to create on the board. |
| Matches list scroll | 39 to 47 ms per frame (21 to 26 fps) | same | Render bound. |
| News list scroll | 42 to 55 ms per frame | 38 to 54 ms | Lead picture 240 x 158 drawn 1:1. |
| Swipe Live to Matches (and back) | 45 to 51 ms frames, first 50 to 60 ms | 47 to 62 ms | Render bound. |
| Swipe Matches to News (and back) | 58 to 86 ms frames | 55 to 70 ms | Render bound. |
| Full repaint per tab | Live 52 ms, Matches 48 to 50 ms, News 63 ms; SPI 34 ms per full frame | | The SPI bus alone allows about 29 fps; rendering is the limit. |
| Matches answer (31 KB JSON) | Parsed on the LVGL task: 48.9 ms (teams 19 KB: 28.5 ms) | Parsed on the HTTP worker: 50 to 51 ms (60 to 107 ms under stress); LVGL task under 10 ms | |
| Picture decode (on the LVGL task) | 2 to 40 ms, average 3.2 ms | same | One per tick; left on that task because LVGL's decoder is not thread safe. |
| Internal RAM free, idle | 110.8 KB (largest block 55 KB) | 110.8 KB | PSRAM free 7.15 MB. |
| Internal RAM lowest, normal start (two TLS sessions) | 65.6 KB | 65.6 KB | |
| Internal RAM lowest, stress: 60 new pictures, 30 matches requests and a celebration | 56.3 KB | 93.4 KB | Fix: JSON documents in PSRAM. PSRAM lowest 5.5 MB. |
| CPU, everything active (same stress, 8 s) | | Both cores 134 to 138 % of 200 %: http_data 63 %, lvgl 51 to 54 %, wifi 8 %, http_images 7 to 8 %, tiT 3.5 % | |
| CPU, idle | Both cores 8.3 % of 200 % (lvgl 6.5 %) | | |
| Stack headroom (lowest) | lvgl 6.1 KB of 16 KB, loop 8 KB, wifi worker 3.6 KB, http 4.9 to 5.0 KB, sound 3.2 KB | same | The LVGL low point comes from swipes and the picker. |

What these say, in plain terms:

- **Swipes and scrolls are render bound.** Frames take 45 to 70 ms against 34 ms of SPI per full frame. 80 MHz SPI would not help render-bound frames. Options recorded for later: LVGL's parallel software draw units (costs about 16 KB of internal RAM, which would eat into the TLS memory floor) or sliding snapshots of the two pages. A profile of a full repaint put the crest images on the Matches tab at about 7 ms and the labels at about 3 ms, with the card backgrounds and rounded corners about 1.5 to 2 ms each; on the News tab the backgrounds are the largest share (about 13 ms) followed by the images (about 10 ms).
- **Celebrations** run at roughly 20 to 24 fps in the live build, which keeps the LVGL core about 90 percent busy. `kIdleFrameMs` was left at 40 ms because a faster cap would push that core to 100 percent and starve polling and pictures.
- **The first screen of data** costs one 327 ms call once per boot, then fills in over about 1.5 s.

## Wi-Fi memory (Task 5)

Release build. Source: Task 5 report.

| State | Internal RAM free |
|---|---|
| Idle, before Wi-Fi | about 155 to 163 KB |
| Right after boot, radio starting | 99 to 105 KB (largest block 57 to 61 KB) |
| Connected, idle | 102.9 to 104.8 KB; PSRAM free 8.275 MB |

Wi-Fi and lwIP therefore cost about 55 to 60 KB of internal RAM. The Wi-Fi worker's 6 KB stack showed 3.6 KB of headroom.

## HTTPS and TLS memory (Task 6)

`app_diag`, Wi-Fi joined, real helper. Source: Task 6 report.

| Configuration | During TLS (free / largest block) | Result |
|---|---|---|
| mbedTLS in internal RAM (IDF default) | 19 KB / 8 KB; internal minimum 936 B | first request worked, the second failed (`mbedtls_ssl_setup` out of memory) |
| mbedTLS allocations in PSRAM | 116 KB / 75 KB on the first request | worked |
| mbedTLS and C++ `new` in PSRAM (final) | internal free during TLS 122 to 124 KB | worked; 100 requests left the internal heap unchanged and PSRAM 332 B higher |

Real fixtures arrived 1 to 4.5 s after the Wi-Fi join. TLS handshake time was 410 to 535 ms.

## Pictures (Task 7)

`app_diag`, real helper. Source: Task 7 report.

| Measurement | Result |
|---|---|
| Internal RAM lowest during 30 requests plus 100 real picture downloads | 71.9 KB (largest block 65.5 KB), against a 60 KB floor |
| Internal RAM lowest during a normal boot (first data and first 104 pictures) | 79.5 KB |
| TLS handshake, new connection per picture (the original design) | about 430 ms each, which made the first screen's roughly 100 crests need about 35 s |
| Picture over a kept connection | 30 to 130 ms; 104 pictures in about 10 s after data arrived |
| 10-minute scripted browse | PSRAM free changed by -284.5 KB while the cache grew by +285 KB (no leak); internal free unchanged |
| 1,005 downloads in churn runs | no PSRAM growth beyond the 3 MB cache; internal RAM flat |
| Celebration with a cutout picture | 22 to 24 fps with a 190 px cutout; 10 to 12 fps when it was stretched into the box (fixed by requesting the shown size) |

Decode times on the LVGL task:

| Picture | Bytes in | Decode |
|---|---|---|
| PNG crest 24 x 24 | 0.5 to 1.9 KB | 2.0 to 4.2 ms |
| PNG crest 32 x 32 or 40 x 40 | about 2 KB | 2.9 to 3.5 ms |
| PNG crest 84 x 84 (Live) | 8 to 11 KB | 10.8 to 15.6 ms |
| JPEG 48 x 48 news thumbnail | 1.2 to 1.4 KB | 2.4 to 2.8 ms |
| JPEG 240 x 135 news lead | 10 KB | 35 to 36 ms |
| PNG 40 x 40 pitch photo or event cutout | about 4 KB | 3 to 5 ms |
| PNG 190 x 190 celebration cutout | 62.5 KB | 62.3 ms |

## Audio driver (Task 7a)

Measured over the serial console, no listening involved. Source: hardware notes, "Measured on the board: audio".

- Every clip ran for its length plus 30 ms pre-roll and 105 ms tail (goal 2385 ms, fanfare 1545, kick-off 645, full time 1185, low 465, critical 585, Wi-Fi 345).
- No write errors and 0 DMA underruns in all runs, including during a Wi-Fi scan, a 20-request HTTPS burst and a 30-picture download. No watchdog or panic.
- The speaker takes 7.6 KB of internal RAM for good. Under the diagnostics-only double-TLS stress the internal minimum was 61.9 KB.
- Clip synthesis costs 1.6 s of CPU on core 1 after the first frame.
- **Not measured:** how it sounds (clicks, hiss, distortion, left/right).

## Software updates (Task 9)

Source: hardware notes, "Measured on the board: software updates" and the Task 9 report.

- Rollback window: until the first successful data, or 60 s of running. Measured times to first data were 13.8 s and 14.4 s.
- The prebuilt bootloader reverted an unconfirmed image as designed; the firmware's own NVS trial record never had to act.
- During a 10 to 20 s download over plain http (a local test server), the UI hitched by about 0.1 s many times a second because flash erase and write stall PSRAM and flash access. Internal RAM was 90.8 KB during that download. A download over HTTPS from a deployed helper has not been measured.
- Release images are about 2.5 MB of the 5 MB slot. Release flash use 47.8 %, RAM use 21.7 % (Task 10 report).

## Watchdog, crash counter and safe mode (Task 10)

Source: hardware notes, "Measured on the board: watchdog, crash counter, safe mode".

- A deliberate stall of a watched task reset the board exactly 15.0 s later with reset reason `ESP_RST_TASK_WDT`, listed the stalled task and counted as a crash.
- In safe mode: a gear tap opened Settings (152 ms frame), Wi-Fi joined, internal RAM free about 131 KB.
- A forced allocation failure (every LVGL allocation of 1 KB or more failing) over 40 picture decodes ended in placeholders with no crash; the internal heap changed by -32 B.

## Battery readings (Task 8)

Source: hardware notes, "Measured on the board: battery, power key, power hold".

- 1360 to 1365 mV at the ADC pin, 4119 to 4134 mV by the factory formula, smoothed 4122 to 4131 mV; model 90 to 93 percent, not charging. On USB the reading stayed flat within 6 mV over the session. **Not checked against a multimeter; accuracy unknown.**
- The injected 3.5 s key press produced the full power-off sequence as a dry run (GPIO7 write skipped). The real power-off was not run.

## Backlight curve (Task 11)

The backlight uses a gamma 2.2 curve at 10 bit: 1 to 4 percent all give duty 1 of 1024, 10 percent duty 6, 20 percent (the default night level) duty 30 and 50 percent duty 223. The steps have not been judged by eye.

## What was not measured

Multi-day uptime, battery runtime and power draw, a real HTTPS over-the-air update, behaviour with the internet down but the router up (DNS), and the visual quality of gradients and sound on the real hardware beyond what the notes say. See "Known limitations" in the [README](../README.md).
