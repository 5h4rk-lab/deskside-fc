# Hardware notes: Waveshare ESP32-S3-Touch-LCD-2.8

Research date: 2026-10-19. Board: Waveshare SKU 27690 (touch version, 2.8 inch, 240x320). Not the non-touch ESP32-S3-LCD-2.8 (SKU 30223).

Marker legend:

- **VERIFIED (page quote)**: stated on a fetched web page.
- **VERIFIED (vendor code)**: read directly from Waveshare demo source or schematic that was downloaded and inspected (files in the Sources section). Strong evidence, but it is firmware behaviour, not a datasheet.
- **VERIFIED (3rd-party code)**: read from the Adafruit CircuitPython board definition or a community library. Independent confirmation.
- **INFERRED**: reasoned from the above or from general ESP32-S3/ST7789 knowledge; check before relying on it.
- **NOT FOUND**: searched, no source.

## Overview

- 2.8 inch IPS-style (the page says "2.8-inch", 240 x 320, 262K colours), SPI display, 5-point I2C capacitive touch, ESP32-S3 with Wi-Fi 4 and BLE 5, onboard IMU, RTC, microSD slot, I2S audio with speaker, LiPo charger, USB-C. VERIFIED (page quote): https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8 and https://www.waveshare.com/esp32-s3-touch-lcd-2.8.htm
- **Two hardware revisions exist. This is the most important gotcha.** VERIFIED (page quote), https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8: "V1 is discontinued, and shipments switched to V2 as of June 5, 2026." V1 = ST7789 + **CST328** touch; V2 = ST7789 + **CST3530** touch. The version is on a label on the back of the board. A board bought now is most likely V2 (INFERRED from the shipment date). The product page (https://www.waveshare.com/esp32-s3-touch-lcd-2.8.htm) lists CST3530; the CircuitPython page (https://circuitpython.org/board/waveshare_esp32_s3_touch_lcd_2_8/) still lists CST328. Firmware must support both (see Touch).
- Pin assignments are identical between V1 and V2 in the vendor Arduino demos (only the touch driver and a few LVGL driver lines differ). VERIFIED (vendor code, `diff` of the two demo zips: only `Display_ST7789.h` (adds a CST3530 include), `LVGL_Driver.*`, `Touch_CST328.cpp` differ, plus new `Touch_CST3530.*`).
- Vendor demos use **LVGL 8.3.10**, not 9. A LVGL 9 UI must use a self-written port (see LVGL port notes).

## Memory and module

| Item | Value | Marker and source |
|---|---|---|
| SoC / module | ESP32-S3R8 (a bare SoC with 8 MB co-packaged PSRAM, mounted on the board; not a WROOM module) | VERIFIED (page quote) "MCU: ESP32-S3R8": https://www.waveshare.com/esp32-s3-touch-lcd-2.8.htm; the schematic text also contains `ESP32-S3R8` (V1 schematic PDF) |
| CPU | Xtensa LX7 dual-core up to 240 MHz | VERIFIED (page quote), same pages |
| SRAM / ROM | 512 KB / 384 KB | VERIFIED (page quote), same pages |
| Flash | 16 MB, external chip W25Q128JVSI (128 Mbit SPI NOR) | Size: VERIFIED (page quote). Part number: VERIFIED (vendor schematic text, V1 PDF). Quad-SPI mode (QIO): INFERRED from the Arduino-ESP32 board entry (`flash_mode=qio`, https://github.com/espressif/arduino-esp32/blob/master/boards.txt) |
| PSRAM | 8 MB, **octal (OPI)**, 80 MHz | Size: VERIFIED (page quote). Octal mode: VERIFIED (vendor code): ESP-IDF demo `sdkconfig.defaults` has `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y`; also Arduino-ESP32 boards.txt `psram_type=opi` and PlatformIO board JSON `memory_type: qio_opi` |
| Wi-Fi / BT | 2.4 GHz 802.11 b/g/n, Bluetooth 5 LE, ceramic antenna; IPEX connector with a switch resistor (needs resoldering to use) | VERIFIED (page quote) |

Wrong-module warning: the module is not "WROOM-1-N16R8"; "ESP32-S3R8" is the chip part number. PlatformIO setting is `qio_opi` (QIO flash, OPI PSRAM), the same as for N16R8 modules. VERIFIED (3rd-party config): https://raw.githubusercontent.com/platformio/platform-espressif32/v8.0.0/boards/waveshare_esp32_s3_touch_lcd_28.json

## Display

| Item | Value | Marker and source |
|---|---|---|
| Controller | ST7789 (datasheet link says ST7789T3; the schematic and vendor ESP-IDF driver name it `st7789t`) | VERIFIED (page quote): product page "ST7789"; datasheet `ST7789T3_SPEC_V1.0.pdf` linked at https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/Resources-And-Documents |
| Bus | 4-wire SPI (MOSI, SCLK, CS, DC, RST), no MISO | VERIFIED (page quote) "Interface: SPI"; pins VERIFIED (vendor code) `Display_ST7789.h` (MISO = -1) |
| Resolution / native orientation | 240 x 320, portrait natively. `MADCTL = 0x00` gives 240 wide x 320 tall | VERIFIED (vendor code): `LCD_WIDTH 240`, `LCD_HEIGHT 320`, init writes `0x36` = `0x00` |
| Colour format | RGB565 (`0x3A` = `0x05` in the Arduino demo, `0x55` in the ESP-IDF demo; both select 16 bit/pixel) | VERIFIED (vendor code) |
| Colour order | RGB in MADCTL (bit 3 = 0). The ESP-IDF demo declares `LCD_RGB_ENDIAN_BGR` in the panel config but its own init then overrides MADCTL to `0x00`. Net result with vendor code: standard RGB565 | VERIFIED (vendor code) for MADCTL; "net result" INFERRED |
| Byte order | Vendor init sends `0xB0` params `0x00, 0xE8`. On ST7789, bit 3 of the second param of RAMCTRL (0xB0) selects little-endian RGB565 data. With this, the demo sends LVGL's native little-endian `lv_color_t` memory straight over SPI and has `LV_COLOR_16_SWAP 0`. | Command bytes VERIFIED (vendor code); explanation INFERRED (ST7789 datasheet knowledge). Safe alternatives: keep vendor init and `LV_COLOR_16_SWAP`/byte-swap off, or drop `0xB0` and swap bytes in the flush callback |
| Inversion | Display inversion ON (`0x21` INVON) is sent in the vendor init (Arduino and CircuitPython). Needed for correct colours | VERIFIED (vendor code, `Display_ST7789.cpp`); VERIFIED (3rd-party code) CircuitPython `board.c` also sends `0x21` |
| Offsets | `Offset_X = 0`, `Offset_Y = 0` (no gap) | VERIFIED (vendor code) |
| SPI clock | Vendor Arduino: 80 MHz, mode 0, on `FSPI`. Vendor ESP-IDF: 80 MHz on `SPI3_HOST`. CircuitPython: 40 MHz | VERIFIED (vendor code) / VERIFIED (3rd-party code) |
| Backlight | GPIO5, active high, driven with LEDC PWM (vendor Arduino: 20 kHz, 10 bit; vendor ESP-IDF: 5 kHz, 13 bit). Direct GPIO, not via an I/O expander (there is no I/O expander on this board) | VERIFIED (vendor code) `Display_ST7789.cpp`, ESP-IDF `ST7789.c` (`EXAMPLE_LCD_BK_LIGHT_ON_LEVEL 1`); VERIFIED (3rd-party code) CircuitPython `backlight_on_high = true`, PWM 50 kHz |
| Reset | GPIO39, hardware reset pulse (50 ms low) before init | VERIFIED (vendor code) |
| Panel mirror | The ESP-IDF demo calls `esp_lcd_panel_mirror(true, false)` after its init, while the Arduino demo does not touch mirroring. The two demos may therefore show the image mirrored relative to each other | VERIFIED (vendor code) for the calls; consequence INFERRED. Verify on hardware: the Arduino path (MADCTL 0x00) is the one this project should copy |

Vendor Arduino init sequence (V2, `Display_ST7789.cpp`, abbreviated; full list at the URL in Sources): reset, `0x11` sleep-out (120 ms), `0x36` 0x00, `0x3A` 0x05, `0xB0` 00 E8, `0xB2` 0C 0C 00 33 33, `0xB7` 75, `0xBB` 1A, `0xC0` 2C, `0xC2` 01 FF, `0xC3` 13, `0xC4` 20, `0xC6` 0F, `0xD0` A4 A1, `0xD6` A1, gamma `0xE0`/`0xE1` (14 bytes each), `0x21` INVON, `0x29` DISPON, `0x2C`. VERIFIED (vendor code).

## Touch

| Item | V1 | V2 |
|---|---|---|
| Controller | CST328 | CST3530 |
| I2C address | 0x1A | 0x58 |
| Marker | VERIFIED (vendor code) `Touch_CST328.h`; CST328 name VERIFIED (page quote) on CircuitPython page and wiki | VERIFIED (vendor code) `Touch_CST3530.h`; CST3530 name VERIFIED (page quote) on wiki and product page |

- Common to both: dedicated I2C bus on **SDA = GPIO1, SCL = GPIO3, INT = GPIO4, RST = GPIO2**, 400 kHz. Vendor code uses `Wire1.begin(1, 3, 400000)`. VERIFIED (vendor code) `Touch_CST328.h/.cpp`; VERIFIED (3rd-party code) CircuitPython `pins.c` (TP_SDA=GPIO1, TP_SCL=GPIO3, TP_RST=GPIO2, TP_INT=GPIO4) and WS_ESP32_Touch28 README.
- No I/O expander involved. Touch reset is a direct GPIO (GPIO2). VERIFIED (vendor code).
- Reset sequence is required before any I2C access:
  - CST328: RST high 50 ms, low 5 ms, high 50 ms. VERIFIED (vendor code) `CST328_Touch_Reset()`.
  - CST3530: RST low 100 ms, high **500 ms** wait. VERIFIED (vendor code) `CST3530_Touch_Reset()`.
- Vendor V2 firmware auto-detects: it resets, tries the CST328 identity read (expects `0xCACA` from register 0xD1F4 block), and falls back to probing 0x58 for CST3530. VERIFIED (vendor code) `Touch_CST328.cpp` `Touch_Init()`. Copy this approach so one firmware image runs on V1 and V2.
- Register scheme: 16-bit register addresses (CST328, e.g. 0xD000 coordinates, 0xD005 touch count, ack by writing 0 to 0xD005) and 32-bit addresses (CST3530, e.g. 0xD0070000 data). VERIFIED (vendor code); the vendor files are the only reference found. No public datasheet found (NOT FOUND).
- Coordinates: vendor LVGL read callback passes touch X/Y straight to LVGL with no swap or mirror, for a 240 x 320 portrait display. VERIFIED (vendor code) `Lvgl_Touchpad_Read`. Whether the V2 CST3530 needs any transform is not verified separately (see Open questions).
- Interrupt: INT on GPIO4, vendor attaches a RISING ISR that sets a flag, then reads over I2C in the LVGL read callback. VERIFIED (vendor code).

## Other peripherals

- **IMU QMI8658 (C)**: on the main I2C bus (SDA 11, SCL 10); vendor code uses address 0x6B (`QMI8658_L_SLAVE_ADDRESS`), 0x6A is defined as the alternate. INT1 = GPIO13, INT2 = GPIO12 (from CircuitPython). VERIFIED (vendor code) for address and bus; INT pins VERIFIED (3rd-party code).
- **RTC PCF85063**: main I2C bus, address 0x51; RTC battery header present; loses time without battery. VERIFIED (vendor code) `RTC_PCF85063.h`; battery header VERIFIED (page quote).
- **Main I2C bus is shared**: IMU and RTC on SDA 11 / SCL 10; the same two pins are brought out on the I2C header and the 12-pin header. Wiki: "Internally tied to other on-board chips, so only external I2C devices can be added. Pins cannot be remapped." VERIFIED (page quote) https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8
- **Touch is on a separate I2C bus** (GPIO1/3), so there is no address clash with the main bus. VERIFIED (vendor code).
- **No I/O expander** on the board. No TCA9554/PCA95xx in the schematic text and all control lines (LCD reset, backlight, touch reset, power control) are direct GPIOs in vendor code. VERIFIED (vendor code) for direct GPIOs; "no expander" INFERRED (absence in the schematic text extraction and in all vendor drivers).
- **Battery**: MX1.25 2-pin header, 3.7 V LiPo, charger chip ETA6098 (schematic text), LDO ME6217C33M5G (800 mA max). VERIFIED (page quote) for header and LDO; ETA6098 VERIFIED (vendor schematic text).
  - Voltage measurement: ADC on GPIO8 (`BAT_ADC`). Vendor formula: `analogReadMilliVolts(8) * 3.0 / 1000 / 0.990476` volts (the factor 3.0 implies a 1:3 divider on the pin; INFERRED, check the schematic). VERIFIED (vendor code) `BAT_Driver.cpp`, `Measurement_offset 0.990476`.
  - Power control: PWR key input on GPIO6 (`Key_BAT`, active low), power hold output GPIO7 (`BAT_Control`/`SYS_EN`). Firmware must drive GPIO7 high to stay powered on battery; driving it low shuts the board off. Long-press logic in vendor code: 10 s sleep, 15 s restart, 20 s shutdown. VERIFIED (vendor code) `PWR_Key.cpp`; FAQ: "Pressing it starts the system from battery, and the board should hold SYS_EN high to stay powered." VERIFIED (page quote) https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/FAQ
  - On USB power the board runs without GPIO7 being held. INFERRED (the FAQ speaks only of the battery case); the schematic agrees (VCC from VBUS through D3), see "Measured on the board: battery, power key, power hold".
- **TF (microSD) slot**: up to 16 GB per product page text ("TF card slot, up to 16GB", VERIFIED page quote). Vendor demo uses the Arduino `SD_MMC` driver in **1-bit mode**: CLK = GPIO14, CMD = GPIO17, D0 = GPIO16, D3 = GPIO21 (card CS line, d1/d2 unused). The schematic names the same nets SD_SCLK/SD_MOSI/SD_MISO/SD_CS, and CircuitPython treats the slot as SPI (SCK 14, MOSI 17, MISO 16, CS 21). VERIFIED (vendor code) + VERIFIED (3rd-party code); either SPI or SD_MMC 1-bit works on these pins. Wiki: format FAT32.
- **Audio**: PCM5101A I2S DAC + amplifier, 8 ohm 2 W 2030 speaker (included), volume knob. I2S: BCLK = GPIO48, LRC/WS = GPIO38, DOUT = GPIO47 (no MCLK). VERIFIED (page quote) for parts; pins VERIFIED (vendor code) `Audio_PCM5101.h` and (3rd-party code) CircuitPython. No buzzer. Amplifier APA2068 (stereo, two bridge-tied outputs), always on, no GPIO control; speaker connector J9 carries left and right separately: see "Measured on the board: audio" below. VERIFIED (vendor schematic, V1 PDF).
- **Buttons**: BOOT = GPIO0 (also usable as a user button in firmware); RESET (EN); PWR key = GPIO6 (see above). VERIFIED (page quote) for existence; GPIO0 VERIFIED (3rd-party code) CircuitPython `BOOT`/`BUTTON0`. There is no separate user key besides BOOT and PWR (INFERRED from the product page list: "RESET and BOOT buttons, battery power control button, volume knob").
- **LEDs**: power and charge indicators (hardware, not GPIO controlled as far as the sources show). VERIFIED (page quote) for existence.
- **Headers**: 12-pin multifunction header (VBUS, D-/D+ = GPIO19/20, 3V3, SCL/SDA = GPIO10/11, TXD/RXD = GPIO43/44, IO18, IO15, GND), I2C header, UART header, speaker header, RTC battery header. VERIFIED (page quote) https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8

## Pin table

| GPIO | Function | Source |
|---|---|---|
| 0 | BOOT button (strapping pin; low at reset enters download mode) | CircuitPython pins.c, schematic text (IO0) |
| 1 | Touch I2C SDA (Wire1) | Vendor `Touch_CST328.h`; CircuitPython |
| 2 | Touch RST | Vendor `Touch_CST328.h`; CircuitPython |
| 3 | Touch I2C SCL (Wire1) | Vendor `Touch_CST328.h`; CircuitPython |
| 4 | Touch INT | Vendor `Touch_CST328.h`; CircuitPython |
| 5 | LCD backlight (PWM, active high) | Vendor `Display_ST7789.h`; CircuitPython |
| 6 | PWR key input (`Key_BAT`, active low) | Vendor `PWR_Key.h`; CircuitPython |
| 7 | Power hold output (`BAT_Control`/SYS_EN, drive high to stay on battery) | Vendor `PWR_Key.h`; CircuitPython |
| 8 | Battery voltage ADC | Vendor `BAT_Driver.h`; CircuitPython |
| 10 | Main I2C SCL (IMU, RTC, headers) | Vendor `I2C_Driver.h`; wiki header table; Arduino-ESP32 variant `SCL = 10` |
| 11 | Main I2C SDA (IMU, RTC, headers) | Vendor `I2C_Driver.h`; wiki header table; Arduino-ESP32 variant `SDA = 11` |
| 12 | IMU INT2 | CircuitPython pins.c |
| 13 | IMU INT1 | CircuitPython pins.c |
| 14 | SD CLK | Vendor `SD_Card.h`; CircuitPython |
| 15 | Free GPIO on 12-pin header | Wiki header table |
| 16 | SD D0 / MISO | Vendor `SD_Card.h`; CircuitPython |
| 17 | SD CMD / MOSI | Vendor `SD_Card.h`; CircuitPython |
| 18 | Free GPIO on 12-pin header | Wiki header table |
| 19 | USB D- (header can also use as GPIO) | Wiki header table |
| 20 | USB D+ (header can also use as GPIO) | Wiki header table |
| 21 | SD CS / D3 | Vendor `SD_Card.h`; CircuitPython |
| 38 | I2S LRCK/WS | Vendor `Audio_PCM5101.h`; CircuitPython |
| 39 | LCD RST | Vendor `Display_ST7789.h`; CircuitPython |
| 40 | LCD SCLK | Vendor `Display_ST7789.h`; CircuitPython |
| 41 | LCD DC | Vendor `Display_ST7789.h`; CircuitPython |
| 42 | LCD CS | Vendor `Display_ST7789.h`; CircuitPython |
| 43 | UART TXD (header) | Wiki header table |
| 44 | UART RXD (header) | Wiki header table |
| 45 | LCD MOSI | Vendor `Display_ST7789.h`; CircuitPython |
| 46 | CircuitPython lists as LCD MISO; vendor code has MISO = -1 (not used) | CircuitPython pins.c; vendor header |
| 47 | I2S DOUT | Vendor `Audio_PCM5101.h`; CircuitPython |
| 48 | I2S BCLK | Vendor `Audio_PCM5101.h`; CircuitPython |
| 35, 36, 37 | Not available as GPIO: used by the octal PSRAM on ESP32-S3R8 (INFERRED from Espressif ESP32-S3 datasheet knowledge). Note the Arduino-ESP32 variant lists `MOSI = 35`, `MISO = 37`, `SCK = 36`, `SS = 34` as generic SPI defaults. Do not use them on this board. | INFERRED |
| 9, 33, 34 | Not referenced by any source found. 33/34 are not a problem with octal PSRAM only; confirm in the schematic before use. | NOT FOUND |

## USB and flashing

- **Native USB-C (ESP32-S3 USB Serial/JTAG), no USB-UART bridge.** Page: "USB Type-C (programming and log output, integrated full-speed USB serial)" and D+/D- of the USB connector are GPIO20/GPIO19 on the header. VERIFIED (page quote) https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8. The schematic text has no CH34x/CP210x part (NOT FOUND in an extraction of the V1 schematic text; INFERRED absence). Arduino-ESP32 board entry uses `usb_mode=1` (hardware CDC and JTAG), `cdc_on_boot=0`. VERIFIED (3rd-party config) https://github.com/espressif/arduino-esp32/blob/master/boards.txt
- USB IDs: Arduino-ESP32 `boards.txt` lists VID 0x303a PID 0x8242 (the Espressif USB Serial/JTAG default; that is what the device will enumerate as), while the variant `pins_arduino.h` defines `USB_PID 0x825E`. The two disagree inside the same upstream core. Treat 303a:1001 (the ROM default) / 303a:8242 as what `lsusb` will show (INFERRED; not verified on hardware).
- **Entering download mode**:
  - Vendor wiki (Firmware flashing page): "hold down BOOT and power cycle the device"; no BOOT+RESET step list is given. VERIFIED (page quote) https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/Firmware-Flashing
  - Vendor FAQ: "fully power off the board, hold the BOOT button, power it on to enter forced download mode, and flash. Power-cycle afterward, since the board doesn't leave download mode on its own." VERIFIED (page quote) https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/FAQ
  - The standard ESP32-S3 sequence (hold BOOT, tap RESET, release BOOT) also applies because BOOT is GPIO0 and RESET is EN. INFERRED (Espressif standard behaviour).
  - If the firmware crashes or reconfigures the USB pins (GPIO19/20 used as GPIO), the USB port can vanish; use the BOOT method above.
- Vendor flashing: Espressif Flash Download Tool, chip ESP32-S3, address 0x0 for the merged demo image (`Firmware/ESP32-S3-Touch-LCD-2.8.bin`), the wiki says to set BAUD to "1152000" (not a standard rate; probably a typo for 115200 or 921600; with PlatformIO use the default 460800 from the board JSON). VERIFIED (page quote) for the instructions.
- **Linux serial port**: expect `/dev/ttyACM0` (USB CDC from the native USB peripheral; INFERRED from how ESP32-S3 USB Serial/JTAG enumerates; not stated by Waveshare, whose FAQ only says to use `dmesg` or list `/dev/ttyS*`, `/dev/ttyUSB*`). On Arch/Manjaro the device group for serial ports is `uucp` (Debian/Ubuntu use `dialout`): `sudo usermod -aG uucp $USER`, then log out and back in. INFERRED (general Linux knowledge; not from Waveshare). PlatformIO's udev rules (`99-platformio-udev.rules`) also cover Espressif 303a vendor IDs. INFERRED.
- Brown-out/battery note: flash from USB with the battery disconnected for the first attempt (INFERRED precaution, no source).

## Toolchain and example config

Vendor (VERIFIED, page quote and vendor files):

- Arduino: "ESP32 by Espressif Systems, version **3.0.0 or later**" (https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/Arduino). Libraries: **LVGL v8.3.10** (bundled in demo zip, offline install) and **ESP32-audioI2S-master v2.0.0**. The page warns LVGL and drivers are strongly version dependent and LVGL v8 drivers may not work with v9. The Arduino page gives no board-menu settings (flash size, PSRAM, partition) (NOT FOUND).
- ESP-IDF: page screenshots use **V5.5.2** (https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/ESP-IDF). The demo's `idf_component.yml` requires `idf >=4.4`, `lvgl/lvgl ~8.3.0`, `chmorgan/esp-audio-player ==1.0.7`, `chmorgan/esp-libhelix-mp3 ==1.0.3`.
- ESP-IDF demo `sdkconfig.defaults`: `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y`, `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`, `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`, `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`, custom partition table (nvs 24K, factory 3M, a 528K FAT partition).
- No vendor PlatformIO example or Arduino Board Manager entry from Waveshare (NOT FOUND; the Waveshare `ESP32-display-support` repo does not list this board and says PlatformIO is only planned: https://github.com/waveshareteam/ESP32-display-support).

Upstream board definitions found (VERIFIED (3rd-party config)):

- Arduino-ESP32 master `boards.txt`: board `waveshare_esp32_s3_touch_lcd_28` ("Waveshare ESP32-S3-Touch-LCD-2.8"), `flash_size=16MB`, `flash_mode=qio`, `flash_freq=120m`, `psram_type=opi`, `memory_type=qio_opi`, `partitions=default`, `usb_mode=1`, `cdc_on_boot=0`. https://github.com/espressif/arduino-esp32/blob/master/boards.txt and variant https://github.com/espressif/arduino-esp32/tree/master/variants/waveshare_esp32_s3_touch_lcd_28
- PlatformIO `platform-espressif32` board `waveshare_esp32_s3_touch_lcd_28`, present on `develop` (commit dated 2026-10-17, "Add Waveshare boards") and in **release v8.0.0 (published 2026-10-19)**. It is NOT in v7.1.3 or earlier (404). Its content: `memory_type: qio_opi`, partitions `app3M_fat9M_16MB.csv`, `f_flash 80000000L`, `-DBOARD_HAS_PSRAM`, `-DARDUINO_USB_MODE=1`, `flash_size 16MB`, upload speed 460800, frameworks arduino and espidf. https://raw.githubusercontent.com/platformio/platform-espressif32/v8.0.0/boards/waveshare_esp32_s3_touch_lcd_28.json . Platform v8.0.0 pins `framework-arduinoespressif32 ~4.30312.0` (= Arduino-ESP32 3.3.12) per https://raw.githubusercontent.com/platformio/platform-espressif32/v8.0.0/platform.json . Not built or tested by this research.

Candidate `platformio.ini` (INFERRED: assembled from the verified board JSON and vendor facts; not compiled on hardware):

```ini
; A) Using the registry board (needs platform-espressif32 >= 8.0.0)
[env:waveshare_s3_touch_28]
platform = espressif32@^8.0.0
board = waveshare_esp32_s3_touch_lcd_28
framework = arduino
monitor_speed = 115200
build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=1     ; Serial over the native USB port (board JSON only sets USB_MODE=1)
    -DLV_CONF_INCLUDE_SIMPLE
    -DBOARD_HAS_PSRAM
lib_deps =
    lvgl/lvgl@^9
```

```ini
; B) Fallback if the registry board is missing: generic S3 devkit with overrides
[env:waveshare_s3_touch_28_generic]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
board_build.arduino.memory_type = qio_opi   ; QIO flash + OPI (octal) PSRAM
board_build.flash_mode = qio
board_upload.flash_size = 16MB
board_build.partitions = default_16MB.csv   ; or app3M_fat9M_16MB.csv
build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
monitor_speed = 115200
```

Notes on the ini: the stock `esp32-s3-devkitc-1` entry is 8 MB flash with no PSRAM (https://docs.platformio.org/en/latest/boards/espressif32/esp32-s3-devkitc-1.html), so the overrides in (B) are mandatory. A `lv_conf.h` for LVGL 9 is the author's responsibility; no vendor LVGL 9 config exists.

## LVGL port notes

Where the vendor code lives (all inside the demo zips; no public Git repo hosting them was found, only these zip files):

- V1 zip: https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8-Demo.zip
- V2 zip: https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8-V2-Demo.zip
- Arduino sketch `Arduino/examples/LVGL_Arduino/`: `Display_ST7789.cpp/.h` (SPI init, command init, `LCD_addWindow()` blit, `Backlight_Init()`/`Set_Backlight()` with `ledcAttach`), `Touch_CST328.cpp/.h` and `Touch_CST3530.cpp/.h` (touch init, reset, read), `LVGL_Driver.cpp/.h` (LVGL port), `I2C_Driver.*` (main bus), `LVGL_Arduino.ino` (`setup()` order: Flash_test, PWR_Init, BAT_Init, I2C_Init, PCF85063_Init, QMI8658_Init, Backlight_Init, SD_Init, Audio_Init, LCD_Init (which also calls Touch_Init), Lvgl_Init, UI).
- ESP-IDF project `ESP-IDF/ESP32-S3-Touch-LCD-2.8-Test/main/`: `LCD_Driver/ST7789.c` (uses `esp_lcd` SPI panel IO + custom `Vernon_ST7789T` panel driver), `LVGL_Driver/LVGL_Driver.c`, `Touch_Driver/`, `PWR_Key/`, `BAT_Driver/`.

How the vendor LVGL 8.3 port works (VERIFIED (vendor code), `LVGL_Driver.cpp` and ESP-IDF `LVGL_Driver.c`):

- Draw buffers (Arduino): two static `lv_color_t` arrays in internal RAM, each `240 * 320 / 20` = 3840 pixels (7.5 KB). Arduino demo has a commented-out PSRAM variant. `full_refresh = 1`.
- Draw buffers (ESP-IDF): two buffers, each `240 * 320 / 10` = 7680 pixels, allocated with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` (PSRAM). SPI bus max transfer is the full frame (`240*320*2`) with `SPI_DMA_CH_AUTO`.
- Flush callback (Arduino): synchronous. Sets the window with `0x2A`/`0x2B`/`0x2C`, then one `SPI.transferBytes()` of the whole area, then calls `lv_disp_flush_ready()` immediately. No async DMA completion. ESP-IDF version uses an `esp_lcd` transfer-done callback (`example_notify_lvgl_flush_ready`) to call `lv_disp_flush_ready`.
- Tick: `esp_timer` periodic every 2 ms calling `lv_tick_inc(2)`. Arduino's `lv_conf.h` has `LV_TICK_CUSTOM 0`.
- Loop: Arduino `loop()` calls `lv_timer_handler()` then `vTaskDelay(5 ms)`; a second FreeRTOS task pinned to core 0 (4 KB stack) polls PWR key, battery, RTC, IMU every 100 ms and also runs Wi-Fi scan. No LVGL mutex is used in the demo; any other task touching LVGL objects must lock.
- `lv_conf.h` in the vendor Arduino library: `LV_COLOR_DEPTH 16`, `LV_COLOR_16_SWAP 0`, `LV_MEM_CUSTOM 0` with `LV_MEM_SIZE 48 KB` (internal heap), refresh period 30 ms.
- Touch read: pointer indev read callback polls the driver, reports only the first point.

Suggested port for LVGL 9 (INFERRED, not verified on hardware):

- Create the display with `lv_display_create(240, 320)`, `lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565)`, `lv_display_set_buffers(...)` with two partial buffers (for example 240 x 40 px, 19 KB each) allocated with `heap_caps_malloc(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)` so the SPI DMA reads from internal RAM; keep PSRAM for `LV_MEM` (use `LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB` or `LV_MEM_SIZE` in PSRAM) and image/decoder buffers. Reason: PSRAM draw buffers with 80 MHz SPI DMA work in the vendor ESP-IDF demo but compete with Wi-Fi and PSRAM cache traffic.
- Flush callback: `lv_display_flush_ready(disp)` after the transfer (ideally from an SPI DMA done callback). LVGL 9 RGB565 buffers are little-endian in memory, which matches the vendor panel setup (`0xB0` little-endian) so no `lv_draw_sw_rgb565_swap` is needed; if colours look byte-swapped, call `lv_draw_sw_rgb565_swap(px, len)` in flush instead of changing the init.
- Tick: `lv_tick_set_cb(millis)` (Arduino) or the 2 ms `esp_timer`.
- Input: `lv_indev_create()` with `LV_INDEV_TYPE_POINTER`, read callback mapping the touch driver output straight to coordinates (vendor does no transform).
- Existing community LVGL 9 compatible library: WS_ESP32_Touch28 v1.0.2 (MIT, https://github.com/jeffvan302/WS_ESP32_Touch28, listed at https://www.arduinolibraries.info/libraries/ws_esp32_touch28) claims LVGL v9 integration, ST7789 with PSRAM framebuffer and dirty-rect, **CST328 touch only** (CST3530 not mentioned), JPEGDEC/PNGdec image loading. Not evaluated beyond its README; for a V2 board its touch driver would likely not work (INFERRED).
- PNG/JPEG over HTTPS: use JPEGDEC/PNGdec (Larry Bank) or LVGL's built-in decoders (`LV_USE_LODEPNG`/`LV_USE_TJPGD`), decode into PSRAM. INFERRED (design suggestion, not a hardware fact).

## Known issues

Sourced items:

- Two touch controllers across revisions (see Overview). The vendor Arduino page says "download the example matching your product". A single binary should detect the controller at runtime (vendor V2 code already does). VERIFIED (page quote / vendor code).
- LVGL v8 vs v9: vendor warns that LVGL v8 drivers may not work with v9. VERIFIED (page quote).
- LVGL forum report (Jan 2025): with this board, touch coordinates print to serial but LVGL does not react; the poster suspected a pin declaration problem. https://forum.lvgl.io/t/lvgl-with-waveshare-esp32-s3-touch-lcd-2-8/19841 . Another user got a dark screen. VERIFIED (page quote), cause not stated.
- Flashing when crashed: need forced download mode (hold BOOT while power cycling) and power-cycle after flashing since the board does not leave download mode by itself. VERIFIED (page quote), FAQ.
- PWR button / SYS_EN: on battery power the firmware must hold GPIO7 high or the board powers off after the button is released. VERIFIED (page quote and vendor code).
- RTC loses the time without a battery on the RTC header. VERIFIED (page quote).
- SD card: format FAT32; if not recognised wait and reset. VERIFIED (page quote).
- Vendor note: "If the compilation succeeds the first time and ... fails during subsequent tests, decompress the folder again and compile the new file." VERIFIED (vendor ReadMe.txt in the demo zips).
- Arduino-ESP32 variant lists SPI default pins 34/35/36/37, which collide with octal PSRAM lines; irrelevant here because the vendor LCD uses its own pins (INFERRED).
- USB PID mismatch inside Arduino-ESP32 (0x8242 in `boards.txt`, 0x825E in the variant) (VERIFIED, see USB section). Harmless in most cases.

Not found (searched, nothing located):

- Touch needing a reset sequence before the I2C scan: not documented, but the vendor code shows a mandatory reset with a 500 ms wait for CST3530; treat as required (INFERRED from the code, no wiki statement).
- Wi-Fi vs SPI/DMA contention: NOT FOUND in any source. The vendor demo runs a Wi-Fi scan in a core-0 task while LVGL runs on the Arduino loop core (core 1), with 80 MHz SPI; no problems are reported.
- Battery connector polarity: NOT FOUND (wiki and product page say only "MX1.25 2-pin, 3.7 V"). Check the silkscreen and schematic before connecting a cell; many third-party LiPo packs use the opposite polarity.
- Boot-loop behaviour: NOT FOUND.

## Measured on the board: audio

Task 7a, 2026-10-22, board V2 with two small speakers plugged in and a 3.7 V cell connected. Three kinds of statement below; do not mix them up.

**Docs say** (V1 schematic PDF `ESP32-S3-Touch-LCD-2.8.pdf`, read from a 400 dpi render; no V2 schematic was found, so the V2 board is assumed to match):

- DAC U8 PCM5101APWR: XSMT (pin 17) tied to 3V3, so the DAC is never soft-muted by a pin; FMT (16) to GND = I2S format; FLT and DEMP to GND; **SCK (12) to GND**, so the DAC makes its system clock from BCK with its PLL (no MCLK needed). OUTL/OUTR go through 470 ohm + 2.2 nF to the nets PCM_AUDIO_L and PCM_AUDIO_R. The PCM510x PLL wants BCK of about 1 MHz or more (INFERRED from TI's PCM510x clocking table, not re-read here): the firmware sends 16-bit samples in 32-bit slots, BCK = 64 fs (not 32 fs) = 64 x 16 kHz = 1.024 MHz. WS (LRCK) must then be 32 BCK high and 32 low (50 % duty): the ESP-IDF default `ws_width` equals the data width (16), which with 32-bit slots gives a 25 % duty LRCK that the DAC may not lock to (a channel missing or silence). The firmware sets `ws_width = 32` (Task 7a fix round 1). Neither BCK nor the LRCK duty was measured: there is no scope or logic analyser; only the driver's configuration (logged at start) says so.
- Amplifier U7 **APA2068KAI-TRG** (stereo, 2 x BTL, analog volume input), supply 5V_PCM (from VCC through a 0 ohm link). INLN <- PCM_AUDIO_L and INRN <- PCM_AUDIO_R through 100 nF each. MUTE pulled to ground (1 k), /SD pulled up to 5V_PCM (10 k), HP-IN pulled to ground (10 k, speaker mode), VOLUME from the knob (R53, 10 k pot with R55 2 k). **No ESP32 pin controls the amplifier or the DAC mute**: the firmware can stop the I2S clock (the DAC then goes quiet), but cannot switch the amplifier off; any hiss the amplifier makes on its own is outside firmware control (the knob affects it).
- Speaker connector **J9, 1.25 mm 4-pin**: pin 1 VOLN, pin 2 VOLP, pin 3 VORP, pin 4 VORN. That is **two separate speaker outputs: left on pins 1-2, right on pins 3-4**. So the docs say the board is **stereo** and drives two speakers, one per channel. Whether the V2 board has this one 4-pin J9 or two 2-pin sockets was not checked (look at the board).
- The volume knob acts in the amplifier, after the DAC: it is independent of the software volume and multiplies with it.

**Measured over the serial console** (no ear involved; release `app` and `app_diag`, 2026-10-22):

- I2S starts: `[sound] ready: I2S 16000 Hz, 16-bit in 32-bit slots (BCK 1024000 Hz), 6 x 240 frames DMA (5760 B)`.
- Every clip ran for exactly its length plus 30 ms pre-roll and 105 ms tail (Goal 2385 ms, fanfare 1545, kick-off 645, full time 1185, low 465, critical 585, Wi-Fi 345, test tone 1139/1140 ms); no write errors, **0 DMA underruns** (send-queue overflows) in all runs, including during a Wi-Fi scan, a 20-request HTTPS burst and a 30-picture download churn; no watchdog, no panic.
- A Goal sent 0.25 s after a KickOff replaced it (`replacing the clip that plays`); `:sound stop` ended the channel 135 ms later (20 ms fade + the 105 ms of queued silence).
- These say the driver ran. They say nothing about how it sounds.

**To be confirmed by listening** (open):

1. Left/right: `:sound tone l` should sound only in the speaker on pins 1-2 (VOL), `:sound tone r` only in the one on pins 3-4 (VOR). ESP-IDF puts the first sample of each frame in the WS-low slot, which is "left" in I2S Philips format (INFERRED). Answers: left only / right only / both / nothing, for each command. If both commands sound in both speakers, the board mixes to mono after all; if one speaker stays silent for both, it is a spare (or not connected).
2. Whether any clip clicks or pops at its start or end, whether there is hiss after a clip ends (the I2S clock is stopped then), whether the start of a clip is cut (the DAC needs time to lock on its PLL after the clock starts; the pre-roll is 30 ms), and whether `:sound goal 100` distorts or crackles with the knob turned up.

## Measured on the board: battery, power key, power hold (2026-10-22, Task 8)

Setup: V2 board on USB (PC), the user's 3.7 V 3000 mAh LiPo (protection circuit) connected (polarity checked by the user earlier). Nobody at the bench: no multimeter, no unplugging, no key presses; the key was exercised only through the `app_diag` injection `:diag-key`.

From the schematic (V1 PDF, power part; the V2 pins are the same):

- Battery divider: BAT -> R5 200k -> BAT_ADC (GPIO8) -> R11 100k -> GND, 100 nF on the pin: exactly 1/3. The vendor formula `adc_mV * 3 / 0.990476` (about +0.96 %) is kept as the factory formula; `:cal-battery <mV>` stores a scale in NVS `batt.cal`.
- PWR key (Key1) pulls `Key_BAT` (GPIO6, 10k pull-up to 3V3) low through D2 and, through D4, pulls the gate of P-MOSFET Q1 (BAT -> system) low: the key itself switches the battery on. GPIO7 (`BAT_Control`) drives NPN T2, which holds the same gate low. So GPIO6 is readable by the MCU (active low) and the board stays on from battery while GPIO7 is high OR the key is down.
- USB: VCC is fed from VBUS through Schottky D3 unconditionally; Q2 (gate on USB_5V) cuts the battery path while USB is present. So on USB, GPIO7 low cannot switch the board off (INFERRED from the schematic; the real GPIO7 write was not run).
- Charger ETA6098: its STAT pin only drives the red charge LED (LED1 to BAT through R6); no GPIO. There is no VBUS divider to any GPIO. The only USB signal the MCU has is USB data activity (`usb_serial_jtag_is_connected()`, SOF based): it says "a host talks to us", not "external power" (a wall charger gives power and no data).
- BOOT = GPIO0 (Key2, 10k pull-up) is a strapping pin; not used (the PWR key is readable, so the BOOT button is not needed for the screen toggle).

Readings (release `app` and `app_diag`, 16-sample average per second, many minutes):

- GPIO8: 1360..1365 mV at the pin -> 4119..4134 mV by the factory formula; smoothed 4122..4131 mV; the model says 90..93 %, not charging (flat). Earlier bring-up reading with the pack on USB: ~4.10 V. Without a battery the bring-up read ~4.18 V (charger float). Not checked against a multimeter (nobody there): accuracy unknown.
- Charging on USB: the reading stayed flat (+-6 mV over the session), so the voltage inference never reported "charging". Whether the pack is full (ETA6098 terminated) or charging slowly cannot be told without the LED or a meter.
- `usb_serial_jtag_is_connected()`: true the whole time (PC attached). The "USB removed" state could not be observed (nobody to unplug). Decision: NOT used for `set_external_charging` (not proven, and it does not mean charging); the voltage inference stays.
- GPIO6 at rest: high (key up) at every [power] line; GPIO7 high.

Power key and switch-off, exercised with the injected key (`:diag-key`, app_diag; dry run):

- 200 ms: screen off (backlight 0) and on again; a scripted touch on the dark screen woke it and was swallowed.
- 1500 ms: "Hold to power off" at +1.0 s, gone on release. 2500 ms: same, no power-off.
- 3500 ms: hint at +1.0 s; at +3.0 s speaker stopped, silent after 5 ms (limit 300), "Powering off" shown, 1.52 s later settings saved, Wi-Fi stopped (`esp_wifi_stop` ESP_OK), GPIO7 write SKIPPED (dry run), off state (backlight 0, no refresh, no network). An injected press 23 s later restarted the board, which rejoined Wi-Fi and had live data again ~10 s later.
- `:diag-batt 3690` (model fed 3.69 V): 15 %, the low-battery banner showed; `:diag-batt off` back to the ADC.

The "off state" on USB is dark, not low power (accepted ruling): the backlight is at 0, the radio is stopped and nothing is redrawn, but the CPU, PSRAM and the LVGL task keep running. The key is read every 10 ms tick; the controller is fed every 10 s, or at once when the key's level changes. A key press restarts the board. On battery the board is really off once GPIO7 is low and the key is up.

UNTESTED on hardware (needs the user):

- The real GPIO7 low (power-off) on battery, and that the board then really goes dark once the key is up.
- The same on USB: per the schematic the board stays powered and enters the off state; whether a press of the real key then restarts it (the 10 s / level-change key watch in the off state).
- The real PWR key presses (short, 1 s hint, 3 s off), its bounce, and that the key that switched the board on from battery is ignored (held at boot).
- Battery accuracy: measure the pack with a multimeter and run `:cal-battery <mV>` (3000..4400); target within 3 %.
- Unplugging USB: the board keeps running on the pack; the reading drops (the charger's rise goes away); a real drain to 15 % shows the banner (deferred: hours on battery).
- Charging detection over a real charge from a low pack (voltage inference only).

## Measured on the board: software updates (2026-10-22, Task 9)

- The prebuilt bootloader honours `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: a newly installed image starts in `pending-verify` and is rolled back by the bootloader after its first start that does not confirm it. The firmware's own NVS trial record (`ota` / `ota.trial`, at most 3 starts) is a fallback that never fired on this board.
- **Rollback window: until the first successful data, or 60 s of running** (TrialHealth: the display drew a frame, the Wi-Fi service started, then the first matches or 60 s). It is not a fixed 15 s: with a slow network the first data can take longer, and without data it is 60 s.
- **Any restart inside that window reverts a good image to the previous one**: the power key (off, or a press in the off state), `:restart`, `:wipe-wifi`, `:erase-settings`, a crash or a watchdog reset. The console prints a warning before `:restart`, `:wipe-wifi` and `:erase-settings` while the update is not confirmed; `:ota` shows the state. Wait for "[ota] healthy after ..." (or the Settings row "Updated to X") before restarting.
- The health inputs are read as levels every tick (frames drawn since boot > 0, the Wi-Fi service started), so no dropped message can leave a good image unconfirmed.

## Measured on the board: watchdog, crash counter, safe mode (2026-10-23, Task 10)

- **Prebuilt config** (Arduino-ESP32 3.3.x libs, `sdkconfig.h`): task watchdog started at boot with 5 s, panic on, the idle task of core 0 watched (core 1's is not); interrupt watchdog 300 ms; brownout detector level 7 with the interrupt handler (it resets at once, nothing of ours can run); core dump to the flash partition `coredump` in ELF format, checked at boot.
- **Task watchdog: 15 s, panic -> reboot** (`esp_task_wdt_reconfigure` at start, core 0 idle task still watched). Watched: the LVGL task and the Wi-Fi worker (subscribed tasks, fed once per loop), the two HTTP workers `http_data` and `http_images` (watchdog *users* fed from their loops; idle waits are 2 s slices). A connect (DNS, TCP, TLS) is covered by a timer for at most its timeout + 30 s, because lwIP's DNS lookup has no deadline of its own (about 20 s with the router up and the internet down); a connect hung longer still resets the board. Unsubscribing a task for a long call was tried first and dropped: every `esp_task_wdt_add` restarts the period for all tasks, which hid the stalled task while pictures were loading. The OTA download task is not watched; the Arduino loop task is watched since Task 11 (subscribed at the end of `setup()`, fed at the top of every `loop()`).
- **Stall test** (`:diag-wdt-stall`, diagnostics builds): reset exactly 15.0 s later, only `wdt_stall` listed, reset reason `ESP_RST_TASK_WDT` (6), counted as a crash, the core dump names it.
- **Reset reason** at every start: `[health] reset reason: <name> (esp_reset_reason N)`, also in `:status`. Counted as crashes: panic, task / interrupt / other watchdog, CPU lock-up, brownout, power glitch. Not counted: power-on, software restart (`:reboot`, an update, the power key), external/USB reset (flashing, and **opening the USB serial port resets this board**), deep sleep.
- **Crash counter**: NVS namespace `health`, key `crash` (nothing else). 3 abnormal resets within 10 min of running time -> **safe mode**. Running time, not the wall clock (it may be unset): each start adds how long the previous one ran, read from RTC memory (`RTC_NOINIT`, rewritten every second; survives panic, watchdog and software resets, not a power-on, which then counts as 0 s). 10 min of stable running in normal mode clears the counter.
- **Safe mode**: banner "Safe mode: updates paused" at the top left (the gear stays free and tappable), no data transport, no pictures, no update checks, no clip preparation; Settings, Wi-Fi and the power key work. Measured: gear tap opens Settings (152 ms frame), Wi-Fi joins, internal RAM free about 131 KB. It ends after 10 min of running in safe mode (then 20, 40, 80 min when it comes back right after; a stable normal run resets that), at a normal restart once that time has run, or with `:safe-mode off` (clears the counter, restarts). A crash in safe mode restarts its time.
- **Update rollback from safe mode**: when one of the counted resets was a software fault and the other app slot holds an intact OLDER soccer-display image (valid or untracked, never aborted/invalid/pending; the whole image is verified), the counter is cleared and the board restarts into that image.
- **Core dump**: the summary (task, PC, panic reason, 6 backtrace addresses) is printed at start and kept for `:status`; the release build then erases the dump (it may hold the helper token from RAM), diagnostics builds keep it for `espcoredump.py`. Since Task 11 the summary line (at most 200 B, prefixed with the reset kind) is also kept in NVS (`health` / `dump`) and shown by `:status` and `:safe-mode` on later starts; a damaged dump is erased in the release build too; `:status` / `:safe-mode` list the counted crashes by kind and age ("panic under 1 min ago"). Measured with `:diag-panic`: `crashes counted 1 of 3: panic under 1 min ago`, `last core dump kept: panic: task loopTask, PC 0x40382ddd, reason "abort() was called ..."`.
- **Allocation failure** (`:diag-img-fail [n]`): picture decodes with every LVGL allocation of 1 KB or more failing all ended as `nullptr` (placeholder, remembered failure); no crash, internal heap change -32 B over 40 pictures.


## Measured on the board: performance (2026-10-23, Task 11)

Measured over the serial console with the diagnostics build (`app_diag`, -O2 like the release) unless noted; the demo build (`app_demo` with `-DAPP_DIAG=1`) for the live-match screens. Tools: the 1 s / 5 s `[perf]`/`[status]` lines (frames, time inside `lv_timer_handler`), `[trace]` frame lists of scripted gestures (`h j n`, each entry `t+ms:frame ms`), `:diag-cpu [s]` (FreeRTOS run-time counters per task, new), `:diag-picker` (timed team-picker build, new), `[http] ... handled on the LVGL task / parsed on the worker` lines (new), `:diag-shot` (the screen as LVGL rendered it, new). "Before" = 34816bb, "after" = the end of Task 11. Board on USB, Wi-Fi rssi -61..-73, 62 matches, 96 teams in the helper's answers.

| What | Before | After | Notes |
|---|---|---|---|
| Boot: `setup()` to first frame on the panel | 1375 ms (release) | 1426 ms (diag) | + about 0.45 s ROM/bootloader before `setup()`; backlight lit with the first frame (100 %) |
| Wi-Fi: radio ready / scan done / joined | 1.9 s / 4.5 s / 7.9 s after start | same | saved network, rssi -61; one 2.6 s scan before the join |
| Clock valid (SNTP) / first matches on screen | 11.6 s / 12.5 s after start | 9.7 to 13.9 s (first matches) | depends on SNTP |
| Goal celebration (live build test `g`, about 4 s) | 19.6 to 24.4 fps, `lv_timer_handler` 91 to 98 % of the LVGL core, frames 35 to 50 ms avg, first frame 81 to 95 ms | same (not changed) | demo goal: 18 to 26 fps, first frame 102 ms; LVGL task 71.5 % of one core over a 5 s window. `kIdleFrameMs` 40 kept (see below) |
| Live pulse (demo live match) | | 28 to 31 frames/s of the dot, 1.6 ms each, LVGL task 9.3 % of one core (both cores busy 11.4 % of 200 %) | |
| Idle, Live tab, no live match | 8.2 % LVGL core, one 20 to 23 ms call every second (Matches recomputed 62 rows: 17 ms) | 6.7 %, longest call 3.2 ms | fix: Matches caches its rows |
| Team picker, 96 rows | 1006 ms to build (one frame held 1.07 s); LVGL +149 KB, PSRAM -205 KB | opens in 104 ms (first frame 168 ms) with 12 rows; the rest 3 rows per 30 ms (21 to 27 ms steps), complete after about 0.9 s; same memory when complete | fixes: no layout per crest (560 ms), then progressive build |
| Matches list scroll (`n`) | 39 to 47 ms per frame (21 to 26 fps) | same | render bound |
| News list scroll | 42 to 55 ms per frame | 38 to 54 ms | lead picture 240 x 158 drawn 1:1 |
| Swipe Live <-> Matches | 45 to 51 ms frames, first 50 to 60 ms | 47 to 62 ms | render bound, see "swipes" below |
| Swipe Matches <-> News | 58 to 86 ms frames | 55 to 70 ms | render bound |
| Full repaint (`y`) per tab | Live 52 ms, Matches 48 to 50 ms, News 63 ms; SPI 34 ms per full frame | | the SPI bus alone allows about 29 fps; rendering is the limit |
| Matches answer (31 KB JSON) | parsed on the LVGL task: 48.9 ms (teams 19 KB: 28.5 ms) | parsed on the HTTP worker: 50 to 51 ms (60 to 107 ms under stress); LVGL task < 10 ms | |
| Picture decode (on the LVGL task) | 2 to 40 ms, average 3.2 ms | same | one per tick; left there (LVGL decoder not thread safe) |
| Internal RAM free, idle | 110.8 KB (largest block 55 KB) | 110.8 KB | PSRAM free 7.15 MB |
| Internal RAM lowest, normal start (two TLS sessions) | 65.6 KB | 65.6 KB | |
| Internal RAM lowest, stress: 60 new pictures + 30 matches requests + a celebration | 56.3 KB | 93.4 KB | fix: JSON documents in PSRAM; PSRAM lowest 5.5 MB |
| CPU, everything active (same stress, 8 s) | | both cores 134 to 138 % of 200 %: http_data 63 %, lvgl 51 to 54 %, wifi 8 %, http_images 7 to 8 %, tiT 3.5 % | |
| CPU, idle | both cores 8.3 % of 200 % (lvgl 6.5 %) | | |
| Stack headroom (lowest) | lvgl 6.1 KB of 16 KB, loop 8 KB, wifi worker 3.6 KB, http 4.9 to 5.0 KB, sound 3.2 KB | same | the LVGL low point comes from swipes / the picker |

- **Swipes and scrolls stay render bound** (45 to 70 ms per full frame against 34 ms of SPI per frame). Not changed in Task 11; options for later: LVGL's parallel software draw units (`LV_USE_OS` FreeRTOS, 2 units: core 0 is 80 to 99 % idle, costs about 16 KB internal RAM for two draw-thread stacks), or sliding snapshots of the two pages. 80 MHz SPI would not help render-bound frames.
- **`kIdleFrameMs` stays 40 ms**: celebration frames already render in 32 to 50 ms, so 33 ms would only raise the LVGL core from about 90 % to 100 % and starve polling and pictures.
- **Gradients (`LV_USE_DRAW_SW_COMPLEX_GRADIENTS`)**: `:diag-shot` of the demo Live screen on the board matches the simulator's snapshot pixel for pixel except the pulsing dot (32 pixels, animation phase): the radial halos and the vignette render on the device as in the simulator. Whether the RGB565 rings of the radial gradient show on the real panel is for the eye.
- **Backlight curve** (`backlight_duty_for_percent`, gamma 2.2, 10 bit): 1 to 4 % all give duty 1 of 1024, 10 % duty 6, 20 % (the default night level) duty 30, 50 % duty 223. `:diag-backlight` steps through 20 levels with the duty printed for the calibration by eye.

## Physical

From the vendor structure drawing (`ESP32-S3-Touch-LCD-2.8_structure.pdf`; VERIFIED (vendor drawing), read from the rendered image; dimensions in mm):

- PCB: 49.90 x 69.00. Overall board with LCD stack thickness 10.00 (parts 6.00 under the board, TP+LCD 3.60, lens 0.70).
- Four mounting holes at the PCB corners: spacing 41.00 (horizontal) x 60.00 (vertical); the holes sit 4.50 from the PCB edges (4.50 horizontal on the right, 4.50 from the top; the drawing also shows 1.9 and 9.0 offsets near the bottom connectors). Hole diameter: not legible in the drawing (NOT FOUND; typical M2 is INFERRED, verify against the STEP/3D file).
- Touch glass outline: 50.54 x 73.06 (+/- 0.05), corner radius R2.00; touch viewing area 43.80 x 58.20; LCD active area 43.20 x 57.60; the glass top margin to active area 3.37 (side) and 10.43 (bottom edge of glass to LCD AA).
- USB-C at the bottom edge, with the speaker and battery connectors either side (offsets 9.00, 11.60 and 11.40 from the drawing); microSD slot on the left edge; three side buttons (BOOT/RESET/PWR) on the left edge at 7.84, 7.82, 7.82 mm pitch; volume knob top-right of the PCB (position from drawing).
- Connectors: battery MX1.25 2-pin, RTC battery header, speaker header, 12-pin multifunction header, I2C header, UART header, IPEX antenna connector. VERIFIED (page quote). Pitch of the I2C/UART headers is not stated (NOT FOUND; typical 1.0 or 1.25 mm is INFERRED, do not rely on it).
- 3D-printable case: Waveshare provides a **3D model (STEP-type ZIP)** of the board, not an enclosure: https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8_3D.zip (VERIFIED, listed at the Resources page; contents not opened). No printable case file from the vendor was found (NOT FOUND). The FAQ says bulk casing customisation is available through sales.

## Open questions

1. Hardware revision of the board in hand (V1 CST328 or V2 CST3530): read the label on the back. Firmware should probe both (0x1A then 0x58).
2. CST3530 specifics: no datasheet found. Does it need the same coordinate mapping as the CST328 in portrait? Vendor code applies none, but that was not exercised on hardware.
3. Is the Arduino-core 3.3.x USB CDC behaviour right with `ARDUINO_USB_CDC_ON_BOOT=1` on this board (the board JSON sets only `USB_MODE=1`)? Verify the first flash and serial monitor, and what `/dev/ttyACM*` appears on Manjaro.
4. Is PlatformIO platform-espressif32 8.0.0 (released the same day as this research) stable enough? Fallback (B) above avoids depending on it.
5. Actual USB VID:PID the board enumerates (0x8242 vs 0x825E vs ROM 0x1001).
6. Exact mounting hole diameter and the header pitches (I2C, UART, battery) for the enclosure; inspect the 3D ZIP.
7. Battery connector polarity relative to common LiPo packs; check the schematic (net names around the MX1.25 and ETA6098).
8. Whether GPIO9, 33 and 34 are free and where the three left-edge buttons are wired besides BOOT (GPIO0) and PWR (GPIO6): not in any source read.
9. Safe max SPI clock: vendor Arduino and ESP-IDF run 80 MHz; CircuitPython 40 MHz. 80 MHz was not tested together with Wi-Fi and PSRAM load.
10. Whether the vendor `0xB0` 0xE8 byte-order setting should be kept for LVGL 9 (see Display); confirm colours on first boot.
11. Flash and PSRAM speeds: Arduino-ESP32 uses `flash_freq=120m` and PlatformIO JSON uses 80 MHz; 80 MHz is the safe choice.

## Sources

Vendor pages and files (fetched or downloaded and inspected 2026-10-19):

- Product page: https://www.waveshare.com/esp32-s3-touch-lcd-2.8.htm
- Wiki home: https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8
- Resources: https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/Resources-And-Documents
- Arduino page: https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/Arduino
- ESP-IDF page: https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/ESP-IDF
- Firmware flashing: https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/Firmware-Flashing
- FAQ: https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/FAQ
- Technical support (no technical content): https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/Technical-Support
- Schematic PDF (V1): https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8.pdf
- Structure (dimension) drawing: https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8_structure.pdf
- 3D model ZIP (not opened): https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8_3D.zip
- Demo V1 (CST328): https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8-Demo.zip
- Demo V2 (CST3530): https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8-V2-Demo.zip
- ST7789T3 datasheet (not read): https://files.waveshare.com/wiki/common/ST7789T3_SPEC_V1.0.pdf
- QMI8658 datasheet (not read): https://files.waveshare.com/wiki/common/QMI8658C.pdf
- Waveshare display repo (no entry for this board): https://github.com/waveshareteam/ESP32-display-support

Third-party:

- CircuitPython board page: https://circuitpython.org/board/waveshare_esp32_s3_touch_lcd_2_8/
- CircuitPython `pins.c`: https://raw.githubusercontent.com/adafruit/circuitpython/main/ports/espressif/boards/waveshare_esp32_s3_touch_lcd_2_8/pins.c
- CircuitPython `board.c` (display init, 40 MHz SPI, backlight on high): https://raw.githubusercontent.com/adafruit/circuitpython/main/ports/espressif/boards/waveshare_esp32_s3_touch_lcd_2_8/board.c
- Arduino-ESP32 `boards.txt`: https://raw.githubusercontent.com/espressif/arduino-esp32/master/boards.txt
- Arduino-ESP32 variant `pins_arduino.h`: https://raw.githubusercontent.com/espressif/arduino-esp32/master/variants/waveshare_esp32_s3_touch_lcd_28/pins_arduino.h
- PlatformIO board JSON (v8.0.0): https://raw.githubusercontent.com/platformio/platform-espressif32/v8.0.0/boards/waveshare_esp32_s3_touch_lcd_28.json
- PlatformIO platform manifest (v8.0.0): https://raw.githubusercontent.com/platformio/platform-espressif32/v8.0.0/platform.json
- PlatformIO esp32-s3-devkitc-1 board doc: https://docs.platformio.org/en/latest/boards/espressif32/esp32-s3-devkitc-1.html
- WS_ESP32_Touch28 community library: https://github.com/jeffvan302/WS_ESP32_Touch28 and https://www.arduinolibraries.info/libraries/ws_esp32_touch28
- LVGL forum thread: https://forum.lvgl.io/t/lvgl-with-waveshare-esp32-s3-touch-lcd-2-8/19841
