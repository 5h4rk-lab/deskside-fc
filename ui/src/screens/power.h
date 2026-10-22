// What the power key shows: the "Hold to power off" hint while the key is
// held, and the "Powering off" screen. Both are plain widgets the app places
// on lv_layer_sys() (above the goal celebration on lv_layer_top()). Shared
// with the ESP32 firmware: no clock reads, no SDL, no stdio.
#pragma once

#include <lvgl.h>

// A centred card: power_key::kHoldHintText over a bar that fills from 0 to
// 100 over the rest of the hold (kLongPressMs - kHintAfterMs). `barOut` gets
// the bar.
lv_obj_t* power_hint_create(lv_obj_t* parent, lv_obj_t** barOut);

// The whole screen: a power symbol over power_key::kPoweringOffText on the
// turf colour. Clickable, so touches end there.
lv_obj_t* powering_off_create(lv_obj_t* parent);
