// Pin map for the Waveshare ESP32-S3-Touch-LCD-2.8 (V1 and V2 share all pins).
//
// Sources (see docs/superpowers/specs/hardware-waveshare-esp32-s3-touch-lcd-2.8.md):
//   [vendor] Waveshare Arduino demo, Arduino/examples/LVGL_Arduino/ in
//            ESP32-S3-Touch-LCD-2.8-V2-Demo.zip (and the V1 zip, identical pins)
//   [cpy]    CircuitPython board definition (pins.c), independent confirmation
// All of these were confirmed on the real board by the bring-up firmware.
#pragma once

#include <stdint.h>

namespace pins {

// Power hold (SYS_EN / BAT_Control). Must be driven high to stay on when
// running from the battery. [vendor] PWR_Key.h PWR_Control_PIN, [cpy]
constexpr int kPowerHold = 7;
// Power key input, active low. [vendor] PWR_Key.h PWR_KEY_Input_PIN
constexpr int kPowerKey = 6;
// Battery voltage ADC (through a divider on the board). [vendor] BAT_Driver.cpp BAT_ADC
constexpr int kBatteryAdc = 8;
// BOOT button, active low (strapping pin). [cpy] BUTTON0
constexpr int kBootButton = 0;

// LCD (ST7789, 4-wire SPI, no MISO). [vendor] Display_ST7789.h, [cpy]
constexpr int kLcdMosi = 45;
constexpr int kLcdSclk = 40;
constexpr int kLcdCs = 42;
constexpr int kLcdDc = 41;
constexpr int kLcdRst = 39;
constexpr int kLcdMiso = -1;      // vendor: EXAMPLE_PIN_NUM_MISO -1
constexpr int kLcdBacklight = 5;  // active high, LEDC PWM. [vendor] LCD_Backlight_PIN

// Touch controller, own I2C bus (Wire1). [vendor] Touch_CST328.h / Touch_CST3530.h, [cpy]
constexpr int kTouchSda = 1;
constexpr int kTouchScl = 3;
constexpr int kTouchInt = 4;
constexpr int kTouchRst = 2;
constexpr uint32_t kTouchI2cHz = 400000;  // vendor I2C_MASTER_FREQ_HZ

// Main I2C bus (Wire): IMU QMI8658 at 0x6B, RTC PCF85063 at 0x51.
// [vendor] I2C_Driver.h I2C_SDA_PIN / I2C_SCL_PIN
constexpr int kMainSda = 11;
constexpr int kMainScl = 10;

// I2S audio (PCM5101A DAC + APA2068 stereo amplifier), no MCLK. [vendor] Audio_PCM5101.h
// No pin enables or mutes the DAC or the amplifier (schematic: XSMT to 3V3,
// amp MUTE pulled low, /SD pulled high); the speaker driver only stops I2S.
constexpr int kI2sBclk = 48;
constexpr int kI2sLrc = 38;
constexpr int kI2sDout = 47;

}  // namespace pins
