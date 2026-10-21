# Bring-up test firmware: Waveshare ESP32-S3-Touch-LCD-2.8

A small test program that answers three questions on the first day with the
board: does the display work, does touch work, does Wi-Fi work. It prints
everything it does to the USB serial port and ends with one summary line:

```
BRINGUP RESULT display=? touch=CST3530 wifi=9 networks battery=none
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
- Optional, for the battery test later: a multimeter (DC volts) and the LiPo
  battery pack (see "Battery test" below).

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
line, `a` arms and `o` runs the opt-in power-off test (see the battery section).

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
| `[power] no battery detected (ADC reads near 0)` | no battery plugged in (normal in the USB-only test), or a loose plug | nothing to do without a battery; with one, check the plug is fully seated |
| `[power] battery very low or ADC offset wrong` | pack nearly empty, or the voltage formula is off | measure the pack with the multimeter and compare |
| `[power] voltage above 4.35 V: check wiring` | wrong reading or wiring | unplug the battery and send me the lines |
| `[power] battery=` far from the multimeter value | ADC offset or divider factor wrong (the formula comes from the vendor demo and is unverified) | send me both numbers; it is a one-line change |

## Battery test (do this AFTER the first USB-only test works)

The battery is a JLJLUP LP103665, 3.7 V, 3000 mAh, single-cell LiPo pack with
a built-in protection circuit and a 1.25 mm 2-pin plug. Flash and run
everything above first with the battery **unplugged**. The `[power]` lines
already work without a battery (they then say `no battery detected`).

**WARNING: Waveshare does not document which pin of the board's battery
connector is + and which is -, and many packs have the opposite polarity to
the board. A reversed connection can destroy the board and is a fire risk.
Never force the plug. Do the polarity check below before the first connection.**

### Safe polarity check (multimeter on DC volts)

1. **Board side.** Power the board from USB only, battery unplugged. Touch the
   two metal pins of the board's battery header with the probes. The pin that
   reads positive with the **red** probe is the board's + pin. The charger
   usually puts about 4.2 V there; the reading may also float or jump, in which
   case rely on the `+` / `-` or `BAT` marks printed on the board next to the
   header.
2. **Battery side.** With the pack unplugged from the board, touch the two
   metal contacts in the pack's plug. The contact that gives a positive
   reading with the **red** probe (about 3.7 to 4.2 V) is the pack's + pin.
   Write that voltage down.
3. **Compare.** Plug in only if the pack's + pin ends up on the same side as
   the board's + pin. If not, **STOP** and send me photos. The pins can be
   swapped inside the plug with a pin extractor, or a reversing adapter can be
   used. Never swap wires on a pack that is live.

Also look for `+` / `-` or `BAT` marks near the header and compare with the
pack's wires (red is usually +, black is usually -), but treat the multimeter
result as the final word.

### What should happen once it is connected correctly

- **USB plugged in, battery attached:** the log shows the battery voltage
  every 5 seconds, for example
  `[power] battery=3950 mV (~80%) trend=rising pwr_key=up hold=high`. While
  the voltage climbs (trend `rising`, 4000 mV or more) you also get
  `probably charging from USB`. That is inferred from the voltage only; the
  board has no charger status pin the program can read.
- **Unplug USB:** the screen should keep running on the battery and the log
  continues (the `[power]` line then shows `falling` or `steady`). This only
  works because the program holds GPIO7 (power hold) high from the first
  instruction, as the vendor says it must.
- **Power button:** pressing the board's power key prints
  `[power] pwr_key pressed` and `pwr_key released after N ms`. That is all it
  does. The vendor's long-press logic (10 s sleep, 15 s restart, 20 s
  shutdown) is deliberately **not** implemented, so the button is inert,
  except for the opt-in power-off test below.
- The `~NN%` is a rough guess from the voltage (approximate table). It is not
  a fuel gauge and is off while charging or under load.

### Optional: power-off test (is there a real off switch?)

We do not yet know whether the board can really be switched off by software
or by the power button while USB is attached. This opt-in test finds out. It
never starts by itself and has to be armed again after every reset or power
cycle. It drives GPIO7 (the power hold line) low for a few seconds, which is
what the vendor does to shut the board down.

How to start it (serial monitor open):

- Type `o` to run it immediately, or
- type `a` (arms it: `power-off test ARMED`), then hold the board's power key
  for 3 seconds. Holding the key does nothing unless you armed it first.

The log counts down (`POWER-OFF TEST: driving GPIO7 low in 3 s`, then
`3 ...`, `2 ...`, `1 ...`) and then drives GPIO7 low.

- **On USB only, no battery (do this first):** expect the board to keep
  running. After 5 seconds the log says
  `board stayed on after GPIO7 low: USB (or something else) keeps it powered;
  GPIO7 restored high`.
- **On battery only, USB unplugged (only after the battery test above worked):**
  without USB there is no serial port, so use the power key. With USB still
  attached, type `a` in the monitor to arm the test, then unplug the USB
  cable (the screen keeps running on the battery), then hold the power key
  for 3 seconds. Expect the board to turn off completely (screen dark, no
  backlight). Pressing the power key again should start it, and the log
  (after you plug USB back in and reopen the monitor) shows a fresh boot. If
  the board does not turn off, GPIO7 is restored high after 5 seconds and
  the screen just keeps running.

What to send me: the exact `[power]` lines (`ARMED`, `POWER-OFF TEST`,
`stayed on`, or the last line before the board went dark) and what the screen
did. This is how we learn whether the board can really be switched off while
USB is attached (expected: no, USB keeps it powered) and whether it switches
off on battery.

### What to send me

1. The `[power]` lines from before and after plugging the battery in.
2. The voltage your multimeter reads across the pack's plug.
3. Whether the screen kept running after the USB cable was unplugged.
4. Whether the pack gets warm. **Stop and unplug it if it gets hot or swells.**

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
- `src/power.*`: battery voltage (ADC on GPIO8), LiPo percentage estimate,
  voltage trend, power key (GPIO6) logging.
- `src/bringup_log.*`: serial logging with the replayable boot log.
