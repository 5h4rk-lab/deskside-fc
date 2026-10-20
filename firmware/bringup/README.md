# Bring-up test firmware: Waveshare ESP32-S3-Touch-LCD-2.8

A small test program that answers three questions on the first day with the
board: does the display work, does touch work, does Wi-Fi work. It prints
everything it does to the USB serial port and ends with one summary line:

```
BRINGUP RESULT display=? touch=CST3530 wifi=9 networks
```

`display=?` means "the program has no way to check the screen; look at it".
Press the BOOT button once the test card looks right and the line is printed
again with `display=ok`.

The same firmware works on both board versions (V1 has a CST328 touch chip,
V2 a CST3530; it detects which one is there).

## What you need

- The board and a USB-C cable that carries **data** (many charging cables do
  not; if no serial port appears, try another cable first).
- PlatformIO at `~/.local/bin/pio` (already installed). The commands below
  write `pio`; type `~/.local/bin/pio` if `pio` is not on your PATH.
- Your user in the `uucp` group (already done) so you can open the serial port.
- Leave the battery unplugged for the first test.

## Flash it

```sh
cd firmware/bringup
pio run -e bringup -t upload
pio device monitor -b 115200
```

Or both in one go (the monitor starts right after flashing):

```sh
pio run -e bringup -t upload -t monitor
```

Quit the monitor with `Ctrl+C`.

### Where is the serial port?

The board uses the ESP32-S3's built-in USB, so it shows up as
`/dev/ttyACM0` (not `ttyUSB0`). Check with:

```sh
ls /dev/ttyACM*
dmesg | tail -20      # look for "303a" (Espressif) and "ttyACM0"
```

If there is more than one port, add `--upload-port /dev/ttyACM0` to the
upload command and `-p /dev/ttyACM0` to the monitor command.

### If uploading fails ("Failed to connect", "No serial data received")

Put the board into download mode by hand, then run the upload command again:

1. Unplug the USB cable (the board must be fully off).
2. Hold the **BOOT** button.
3. Plug the cable back in while holding BOOT, then release BOOT.

Alternative with the cable plugged in: hold **BOOT**, press and release
**RESET**, then release **BOOT**. (Waveshare documents the first method; the
second is the standard ESP32-S3 method.)

After a manual download-mode flash the board may stay in download mode:
press **RESET** (or unplug and replug) to start the new firmware.

## What you should see

The program runs these steps once, then stays on the test card. Every step is
also described on the serial monitor in lines starting with
`[display] expect:`.

| Step | Screen | Serial |
|---|---|---|
| 1. Backlight | backlight glows dim and gets brighter over 1 s (content may be noise) | `[display] backlight: ...` |
| 2. Colours | solid RED, GREEN, BLUE, WHITE, BLACK, 1.5 s each | `[display] expect: solid RED` ... |
| 3. Test card | see below | `[display] expect: TEST CARD ...` |
| 4. Touch | nothing changes | `[touch] ...` lines, I2C scans |
| 5. Wi-Fi | nothing changes | `[wifi] ...` list of networks |
| 6. Result | test card | `BRINGUP RESULT ...` |
| then | touch the screen: a small 12x12 square follows your finger | `[touch] raw x=.. y=.. n=..` |

Test card (hold the board upright, USB-C at the bottom):

- **Top third**: eight vertical colour stripes, left to right: red, green,
  blue, yellow, cyan, magenta, white, black.
- **Middle third**: a smooth gray ramp, black on the left, white on the right.
- **Bottom third**: black panel with a thin white frame, a white diagonal
  line from its top-left to its bottom-right corner, and a white 40x40 square
  in its top-left corner.

Touching the white square re-runs the colour sequence. Every 5 seconds the
monitor prints `[sys] alive ...`; the result line repeats every minute.

Keys in the serial monitor: `r` prints the whole boot log again (useful if the
monitor started late), `p` re-runs the colour sequence, `s` prints the result
line.

## The four versions (environments)

The program can be built in four variants. They differ only in two display
settings. Start with `bringup`; switch only if the colours are wrong.

| Environment | What it changes | Command |
|---|---|---|
| `bringup` | nothing: same settings as the Waveshare demo | `pio run -e bringup -t upload` |
| `bringup_swap` | sends each pixel's two bytes in the other order | `pio run -e bringup_swap -t upload` |
| `bringup_noinv` | leaves colour inversion off | `pio run -e bringup_noinv -t upload` |
| `bringup_swap_noinv` | both of the above | `pio run -e bringup_swap_noinv -t upload` |

The start of the log says which one is running (`[sys] env=...`).

### Display SPI speed

The display link runs at 40 MHz (safe). The Waveshare demo uses 80 MHz. To try
80 MHz (faster screen updates), no file needs editing:

```sh
PLATFORMIO_BUILD_FLAGS="-DBRINGUP_SPI_HZ=80000000" pio run -e bringup -t upload
```

To go slower if the picture has noise or wrong pixels, use
`-DBRINGUP_SPI_HZ=20000000`. The log shows the speed in `[sys] env=... spi_hz=...`.

## Symptoms and what to try

| What you see | Most likely cause | Try |
|---|---|---|
| No serial output at all | charging-only cable, monitor opened late, wrong port | another cable; `ls /dev/ttyACM*`; press `r` in the monitor to replay; press RESET with the monitor open |
| No `/dev/ttyACM*` appears | cable, or board stuck | other cable / port; enter download mode (above) and check `dmesg | tail` |
| Board resets in a loop (`reset reason: brown-out` or `panic`) | power, or a crash | other USB port (no hub); send me the log, the monitor decodes crash addresses |
| Backlight never lights | backlight PWM | send me the `[display] backlight` lines |
| Backlight lit but screen stays blank / noise | display init or SPI | `PLATFORMIO_BUILD_FLAGS="-DBRINGUP_SPI_HZ=20000000"`; check the `[display] SPI` and `init` lines |
| Colours wrong in a mixed-up way (red shows as something odd, ramp is blotchy) | pixel byte order | `bringup_swap` |
| Colours inverted (red phase looks cyan, white looks black) | inversion setting | `bringup_noinv` |
| Both of the above | both | `bringup_swap_noinv` |
| Red and blue swapped (red phase looks blue), green fine | RGB/BGR order | not covered by an environment yet: tell me, it is a one-byte change (MADCTL) |
| Image mirrored (stripes go black ... red left to right, or the square is on the right) | panel scan direction | tell me which way; it is a one-byte change (MADCTL) |
| Image rotated or squashed, stripes horizontal | orientation | photo please |
| Thin stripes or a shifted image at an edge, frame not fully visible | address offset | photo please |
| `touch=none` | controller not detected | send me every `[touch]` line (especially `probe` and `i2c scan`) |
| Square appears away from your finger, mirrored or swapped | touch coordinate mapping | touch the four screen corners in turn (top-left, top-right, bottom-right, bottom-left) and send me the four `[touch] raw` lines |
| Touch works but the square never appears | display or coordinates out of range | send the `[touch] raw` lines |
| `wifi=0 networks` | radio / antenna | send the `[wifi]` lines; try near the router |

## What to send me

1. The `BRINGUP RESULT ...` line (and ideally the whole log: press `r` in the
   monitor, then copy everything).
2. The USB lines from `dmesg` right after plugging in:
   `dmesg | tail -20`.
3. A phone photo of the test card (screen upright, USB-C at the bottom).
4. The label on the back of the board: does it say V1 or V2?
5. Which environment you flashed (`bringup`, `bringup_swap`, ...).
6. If touch looked wrong: the four `[touch] raw` lines from touching the four
   corners in order top-left, top-right, bottom-right, bottom-left.

## Optional: Wi-Fi connection and download test

Without credentials the firmware only scans. To also test a real connection
and an HTTPS download:

```sh
cp secrets.ini.example secrets.ini
# edit secrets.ini: put your network name and password in
pio run -e bringup -t upload -t monitor
```

`secrets.ini` is ignored by git; never commit it. With it the log shows the IP
address, signal strength, and `[wifi] HTTPS status=200 bytes=...`.

## Files

- `platformio.ini`: build settings and the four environments.
- `src/main.cpp`: test sequence, touch-test loop, heartbeat.
- `src/board_pins.h`: every pin, with where it comes from.
- `src/display_st7789.*`: display driver and test card.
- `src/touch.*`: CST328 / CST3530 detection and reading, I2C scans.
- `src/wifi_scan.*`: Wi-Fi scan, optional connect and HTTPS GET.
- `src/bringup_log.*`: serial logging with the replayable boot log.
