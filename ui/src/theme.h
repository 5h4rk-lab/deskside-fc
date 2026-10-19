// Palette from the approved mockup (docs/superpowers/specs/assets/ui-mockup-v2.html).
#pragma once

#include <stdint.h>

namespace theme {

constexpr uint32_t kTurf = 0x0A1410;        // screen background
constexpr uint32_t kPanel = 0x0F1D17;       // cards and panels
constexpr uint32_t kLine = 0x1B3B2C;        // hairlines
constexpr uint32_t kChalk = 0xF1EFE4;       // primary text
constexpr uint32_t kDim = 0x7C8F84;         // secondary text
constexpr uint32_t kFlood = 0xFFC857;       // live indicators (floodlight amber)
constexpr uint32_t kDefaultAccent = 0xEF0107;  // Arsenal red until a team is picked
constexpr uint32_t kTabBar = 0x050907;      // bottom bar backdrop

constexpr int32_t kTabBarHeight = 44;

}  // namespace theme
