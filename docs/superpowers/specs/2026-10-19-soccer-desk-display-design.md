# Soccer Desk Display: Design

Date: 2026-10-19
Status: draft for approval

## Intent

A birthday gift for a friend who is a big soccer fan: a small touchscreen that sits on his desk and shows live football without a phone. He works long hours and sometimes in a lab where phones are not allowed. Matches can kick off early in the morning. He should be able to glance at the screen and know the score, what is coming up, who won last night, and what happened (news).

The gift-giver (the user) is not a football person. The device must be self-explanatory, and labels such as formations and league names should be plain English.

## Hardware

**Target board (decided 2026-10-19):** Waveshare ESP32-S3-Touch-LCD-2.8 (touch version, bought on Amazon, arriving 2026-10-20). 2.8" ST7789 SPI display, 240x320 portrait, 5-point I2C capacitive touch, ESP32-S3R8 (16 MB flash, 8 MB octal PSRAM), Wi-Fi + BLE, USB-C, IMU, RTC, microSD, speaker, LiPo charger. All verified facts, pin table, and open questions are in `docs/superpowers/specs/hardware-waveshare-esp32-s3-touch-lcd-2.8.md`.

- **Two hardware revisions exist:** V1 uses a CST328 touch chip (I2C 0x1A), V2 (shipping since 2026-06-05) uses a CST3530 (I2C 0x58). The firmware must detect the touch chip at start-up and support both. CST3530 has no public datasheet, only vendor demo code.
- Vendor demos use LVGL 8.3 and Arduino-ESP32 3.x; there is no vendor LVGL 9 code, so the display/touch/LVGL port is written by us behind one thin layer.
- **Spare board:** the earlier Alibaba board (Dongheng Tech, "SC05 Plus" listing, ESP32-S3R8, display module ZX2D80CE-C0F, RS485) stays a spare; its chips and pins were never confirmed.
- All UI work happens on a PC simulator first; the board arrives after most of the simulator is built.

## Goals

1. Show live scores for his favourite team's match on the home screen, readable at a glance from a desk.
2. Show a list of live, upcoming and recent matches; tapping any match opens its detail.
3. Match detail shows a summary (goals, cards, substitutions), a pitch view with starting lineups and formation, and stats when available.
4. Show last night's results and a few football headlines.
5. Celebrate when his favourite team scores or wins (full-screen animation).
6. He chooses his favourite team on the device. Until he does, the default is Arsenal.
7. He can set up Wi-Fi himself and store several networks (home, work, lab, travel). The device joins whichever known network is in range.
8. Pictures: team crests, player cutouts, news photos.
9. Looks great. The approved mockups are the reference (see Visual design).

## Non-goals

- Live player tracking or heat maps. Free data sources only give starting formations, not positions during play.
- Betting odds, fantasy football, standings tables beyond what is trivial to show.
- A phone app or a web dashboard for configuration.
- Support for sports other than football (soccer).
- Copyright clearance for pictures. This is a personal gift and the user has said it does not matter.

## Anti-goals (failure even if it technically works)

- Looks like a generic dark-navy scoreboard with a neon accent.
- Needs a phone or a PC to use day to day.
- Shows a stale score without saying it is stale (offline or API down must be visible).
- Is slow or janky when swiping or celebrating.
- Needs a paid API key or a monthly bill.

## Visual design (approved)

Approved mockup: `docs/superpowers/specs/assets/ui-mockup-v2.html` (open in a browser; five screens at real 240x320: Live, Pitch, Matches, News, Goal). The navigation map is `assets/navigation-map-v1.html`.

**Not mocked yet:** Match detail Summary and Stats sub-tabs, Settings, Wi-Fi setup with keyboard, favourite-team picker, offline and empty states. The builder designs these in the same style (same palette, type and spacing as the five approved screens). The user reviews them in the simulator.

- Concept: "floodlit turf". Base `#0A1410` (night turf), panel `#0F1D17`, line `#1B3B2C`, chalk text `#F1EFE4`, dim text `#7C8F84`, floodlight amber `#FFC857` for live indicators, plus one accent that is the **favourite team's primary colour** (default Arsenal red `#EF0107`). Changing team recolours the whole UI.
- Type: Big Shoulders Display (scores, team names, headings), Hanken Grotesk (small text). Both converted to LVGL bitmap fonts at the needed sizes.
- Navigation: three bottom tabs: Live, Matches, News. Settings behind a small gear in the corner of the Live tab (location is the builder's call).
- **Live (home):** background diagonally split in home and away team colours, large crests, very large score, amber minute pill with a pulsing dot, and a glass strip showing the latest event with the player's face.
- **Match detail** (opened from Live or Matches): sub-tabs Summary / Pitch / Stats, swipe between them, back button top left. Pitch view is a striped pitch, his side in his team colour, the opponent in blue/their colour. Notable players show a face, others show a shirt number. Tapping a player shows a name card.
- **Matches:** live pinned on top with an amber edge, then grouped by day (today, tomorrow, yesterday). A dot marks his team.
- **News:** lead story with a large image, tiles for his team's last result and next game, then headline rows with thumbnails.
- **Goal celebration:** the screen floods with his team colour, rays rotate, "GOAL!" stamps in with a bounce, the scorer cutout pops up. About 4 seconds, then returns. A win gets a longer version.
- Motion: spring-style, fast (100 to 300 ms for interactions). Celebration is the one big moment; everything else stays quiet.

## Behaviour

### Data (no API key)
- Primary: ESPN's public site API (unofficial, can change): scoreboard for fixtures and scores, `summary?event=` for lineups with formation, key events and news, `news` for headlines with photos. Verified on 2026-10-19: scoreboard, lineups with formation, key events and news with photos all return data without a key. Player headshots are not included.
- Player photos: TheSportsDB (free) cutouts looked up by player name and cached. Fallback is a shirt number.
- Team crests: ESPN team logos or football-data.org crests (both free, PNG).
- Backup if ESPN breaks: football-data.org (free key, registered by the builder).

### Cloud helper
A small free serverless function (platform is the builder's call; Cloudflare Workers or Vercel are both fine) sits between the device and the public APIs. It fetches, trims and caches data, resizes pictures to the exact sizes the screen needs, and returns small JSON and small images. The ESP32 only talks to this one endpoint. Reasons: the ESP32 cannot decode or scale big images well, it keeps API quirks out of firmware, and it can be fixed without reflashing.

### Refresh and power
- Live match on screen: refresh about every 20 to 30 seconds. Otherwise every few minutes. Screen can dim at night (time-based or manual).
- When offline or the helper is unreachable, show last known data with a clear "last updated" or offline marker.

### Wi-Fi
- First boot or no known network: guided setup screen. Scan, pick a network, type the password on an on-screen keyboard.
- Stores several networks (suggest at least 8) in flash. Auto-joins the strongest known one. Settings screen lists saved networks with add and remove.
- Detail left to builder: captive-portal fallback if the on-screen keyboard proves hard to use.

### Favourite team
Picked in Settings from a searchable list of teams in the supported leagues. Stored in flash. Drives the accent colour, the Live home screen, the dot in the Matches list and the goal celebration.

### Time
Times shown in his local timezone, set once in Settings (or inferred from the network). Clock comes from NTP.

## Constraints

- Screen 240x320 portrait, touch-only input (no keyboard or buttons assumed).
- Flash 16 MB (per listing title). PSRAM 8 MB (ESP32-S3R8, per the board photo). Still confirm on the real board before fixing the memory budget.
- Free tiers only.
- Must run all day on USB power, unattended.

## Milestones (designer's call)

1. **Simulator UI.** All screens running on the PC in a 240x320 window with fake match data, including the goal celebration and team recolouring. User reviews and approves the look.
2. **Cloud helper with live data.** Helper on the user's own account returns real ESPN data and resized images; the simulator reads from it.
3. **Hardware.** Once the board arrives and is identified: display and touch drivers, Wi-Fi setup with on-screen keyboard, saved networks, favourite-team storage, offline marker.
4. **Hardening.** Over-the-air firmware update pulled from the helper (so a bug does not mean a physical recall), night dimming with touch to wake, long-run soak test.

Hardware risk rule: the first hardware milestone must not depend on PSRAM. If the real board is weaker than assumed (no PSRAM, or an RGB panel that eats memory), drop the goal celebration to a simpler two-frame version and keep all other screens. Set a memory budget in the plan before choosing image sizes.

## Calls made by the designer (reviewer's open questions, settled)

- Helper platform: Vercel function using the `sharp` image library, on the user's own free account. Chosen because Workers have weak image tooling. The helper URL is stored in device flash with the default baked in, and requests carry a simple shared token so strangers cannot use it.
- football-data.org key is registered by the builder under the user's email and kept only as a helper environment variable.
- OTA firmware update is in scope (milestone 4).
- Team picker at launch covers the top five European leagues (Premier League, La Liga, Serie A, Bundesliga, Ligue 1). Other leagues and national teams later. Team colours come from a small hand-written table for those clubs; any club missing from it falls back to the dominant colour of its crest, worked out by the helper.
- Night dimming is time-based with touch to wake. No light sensor assumed.
- Wi-Fi: WPA2-Personal and open networks only. The user confirmed his home, work and lab networks are ordinary password Wi-Fi. Enterprise and captive-portal networks are out of scope for now.

## Open items

- Board identity (partly known, see Hardware). Still needed: display controller, touch controller, pin map, PSRAM size. The builder should get these from the supplier's specification page and demo code before writing driver code.

## Decisions made so far

| Decision | Who |
|---|---|
| UI as approved mockups | User approved |
| Favourite team chosen on device, default Arsenal | User (picked on device); default is designer's call |
| He sets up Wi-Fi himself, multiple networks | User |
| PC preview before hardware | Designer's call |
| C++ with PlatformIO and LVGL 9 | Designer's call, open to change |
| Cloud helper between device and public APIs | Designer's call |
| ESPN primary, TheSportsDB for player photos, football-data.org as backup | Designer's call |

## Left to the builder

- Repository layout, build setup, how the simulator and firmware share UI code.
- Which cloud platform and image format (for example indexed PNG or RGB565 binary) fit memory best.
- Exact league coverage for the first version (suggest Premier League, La Liga, Serie A, Bundesliga, Ligue 1, Champions League, World Cup and Euros when in season).
- Settings placement, empty and error states, loading skeletons, brightness handling.
- Which stats appear in the Stats sub-tab, based on what ESPN returns.
- Testing approach.
