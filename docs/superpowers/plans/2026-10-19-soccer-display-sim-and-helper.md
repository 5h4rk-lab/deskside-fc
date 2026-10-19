# Soccer Desk Display: Simulator and Helper Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the cloud helper and the full touchscreen UI running in a 240x320 PC simulator with real data, so the look and behaviour can be approved before the board arrives.

**Architecture:** A TypeScript Vercel project (`helper/`) normalises free ESPN and TheSportsDB data into a small JSON contract and resizes images. A C++ LVGL 9 UI (`ui/`) reads that contract through a `DataSource` interface (fake data or HTTP) and runs in an SDL window (`sim/`). The same `ui/` code is reused by the firmware in the later hardware plan (`firmware/`, not in this plan).

**Tech Stack:** TypeScript, Node 24, Vercel Functions, `sharp`, `vitest`; C++17, CMake, LVGL 9.2, SDL2, libcurl, ArduinoJson 7, doctest, `lv_font_conv`.

**Spec:** `docs/superpowers/specs/2026-10-19-soccer-desk-display-design.md`. Approved look: `docs/superpowers/specs/assets/ui-mockup-v2.html`. Read both.

**Not in this plan:** firmware, display/touch drivers, Wi-Fi hardware, OTA (milestones 3 and 4). Their plan is written once the supplier confirms the display and touch chips.

## Global Constraints

- Screen 240x320 portrait, touch only.
- Palette: turf `#0A1410`, panel `#0F1D17`, line `#1B3B2C`, chalk `#F1EFE4`, dim `#7C8F84`, amber `#FFC857`; accent = favourite team's primary colour, default Arsenal `#EF0107`, opponent default `#034694`.
- Fonts: Big Shoulders Display (scores, names, headings), Hanken Grotesk (small text), as LVGL bitmap fonts.
- Free tiers only, no paid key. Data: ESPN public API (primary), TheSportsDB (player cutouts), football-data.org crests (only hosts allowed by the image proxy: `a.espncdn.com`, `r2.thesportsdb.com`, `crests.football-data.org`).
- Live refresh every 20 to 30 s (use 25 s), otherwise every 180 s. Times in the user's chosen timezone.
- Team picker covers Premier League (`eng.1`), La Liga (`esp.1`), Serie A (`ita.1`), Bundesliga (`ger.1`), Ligue 1 (`fra.1`) only.
- Wi-Fi: WPA2-Personal and open networks only; at least 8 saved networks.
- Stale or offline data must be visibly marked, never shown as current.
- Motion: spring-like, 100 to 300 ms for interactions. Goal celebration about 4 s (win: about 8 s).
- Plain-English labels for formations and leagues (user is not a football person).

## Review Focus

1. **No matches at all** (off-season, international break): Live shows a countdown to the favourite team's next fixture; Matches shows a designed empty state. Pinned by Tasks 10, 12, 14.
2. **Lineups not published yet** (before kickoff): Pitch tab shows "Lineups appear about an hour before kickoff", not a blank pitch. Pinned by Tasks 3, 13.
3. **False celebrations:** first poll after boot with an existing score, a goal disallowed by VAR (score goes down), and the opponent scoring must not trigger the celebration. Pinned by Task 10.
4. **Long names:** "Wolverhampton Wanderers", "Alexander-Arnold", and wide scores such as "10:0" must not overflow; names truncate with an ellipsis, abbreviations are used on Live and Matches. Pinned by Tasks 12, 14.
5. **Helper unreachable, 401 or slow mid-match:** last data stays on screen with a stale marker, no crash, no blank screen. Pinned by Task 18.

---

## Data contract (decided here; both sides must match it)

All times are epoch seconds (UTC). Image fields are helper-relative paths such as `/api/img?u=...&w=48&h=48`.

```
Team        { id:string, abbr:string, name:string, crest:string, color:"#RRGGBB" }
MatchSummary{ id:string, league:string, state:"pre"|"live"|"post"|"postponed",
              clock:string|null, kickoff:number, home:Team, away:Team,
              homeScore:number|null, awayScore:number|null }
MatchEvent  { minute:string, type:"goal"|"yellow"|"red"|"sub", team:"home"|"away",
              player:string, detail:string|null }
LineupPlayer{ name:string, number:number, x:number, y:number, photo:string|null }  // x,y in 0..1, y=0 own goal line
Lineup      { formation:string, players:LineupPlayer[] }     // empty players = not published
StatRow     { label:string, home:string, away:string }
MatchDetail { match:MatchSummary, events:MatchEvent[], home:Lineup, away:Lineup, stats:StatRow[] }
NewsItem    { id:string, headline:string, published:number, image:string|null }
```

Endpoints (all need header `x-token`, else 401):
`GET /api/matches?leagues=eng.1,esp.1&from=YYYYMMDD&to=YYYYMMDD` → `{updated:number, matches:MatchSummary[]}`
`GET /api/match/:id?league=eng.1` → `MatchDetail`
`GET /api/news?league=eng.1` → `{updated:number, articles:NewsItem[]}`
`GET /api/teams` → `{teams:(Team & {league:string})[]}`
`GET /api/img?u=<url>&w=<px>&h=<px>&fit=contain|cover` → PNG (JPEG when source is JPEG and `fit=cover`)
`GET /api/cutout?name=<player>&w=<px>&h=<px>` → PNG, or 404 when no cutout.

Shared fixtures live in `contract/fixtures/` (JSON files). Helper tests produce them from normalisers; UI tests parse them.

## File Structure

```
.gitignore
contract/fixtures/                 recorded + hand-edited JSON used by both sides
helper/                            Vercel project
  api/{health,matches,news,teams,img,cutout}.ts   thin handlers
  api/match/[id].ts
  src/{auth,contract,espn,formation,colors,images,sportsdb}.ts
  test/*.test.ts
  scripts/smoke.sh
ui/                                shared LVGL code (also used by firmware later)
  src/{theme,model,json_parse,store,goal_detector,lru_bytes,refresh_policy,dim_schedule}.{h,cpp}
  src/data_source.h                DataSource interface
  src/screens/{live,detail,matches,news,goal,settings,wifi,team_picker}.{h,cpp}
  src/app.{h,cpp}                  tab view, navigation
  fonts/                           generated bitmap fonts + generation script
sim/
  CMakeLists.txt, main.cpp, mock_source.cpp, http_source.cpp, snapshot.cpp
  tests/*.cpp                      doctest
```

---

### Task 1: Repo hygiene and helper scaffold with auth

**Files:** Create `.gitignore`, `helper/package.json`, `helper/tsconfig.json`, `helper/vitest.config.ts`, `helper/src/auth.ts`, `helper/api/health.ts`, `helper/test/auth.test.ts`.

**Interfaces:** Produces `checkToken(header: string | undefined, expected: string): boolean` and `requireToken(req: VercelRequest, res: VercelResponse): boolean` (sends 401 and returns false on failure; reads `process.env.HELPER_TOKEN`).

- [ ] **Step 1:** `socer-display/` sits inside a larger git repo that has no commits. Run `git init` inside `socer-display/` so it is its own repository. Add `.gitignore` with `.superpowers/`, `node_modules/`, `.vercel/`, `build/`, `.env*`.
- [ ] **Step 2: Write failing test** `auth.test.ts`: `checkToken("abc","abc")` is true; wrong value false; `undefined` false; `checkToken("","")` false (empty expected never matches).
- [ ] **Step 3:** Run `cd helper && npx vitest run`. Expected: FAIL (module missing).
- [ ] **Step 4:** Implement `checkToken` with `crypto.timingSafeEqual` (length check first). Implement `health.ts` returning `{ok:true}` after `requireToken`.
- [ ] **Step 5:** Run vitest. Expected: PASS.
- [ ] **Step 6:** Commit `chore: scaffold helper with token auth`.

### Task 2: Matches endpoint

**Files:** Create `helper/src/contract.ts` (types exactly as the contract above), `helper/src/espn.ts`, `helper/api/matches.ts`, `helper/test/matches.test.ts`, `contract/fixtures/scoreboard-post.json`, `scoreboard-live.json`, `scoreboard-pre.json`, `scoreboard-postponed.json`, `scoreboard-empty.json`, `matches-normalised.json`.

**Interfaces:** Produces `normalizeScoreboard(raw: unknown, league: string): MatchSummary[]` and `fetchMatches(leagues: string[], from: string, to: string, fetchImpl?: typeof fetch): Promise<MatchSummary[]>` (merges leagues, sorted by `kickoff`, one ESPN call per league using `?dates=from-to`).

- [ ] **Step 1:** Record a real response: `curl https://site.api.espn.com/apis/site/v2/sports/soccer/eng.1/scoreboard > contract/fixtures/scoreboard-post.json`. Make `-live`, `-pre`, `-postponed` copies by editing `status.type.state` (`in`/`pre`/`post`) and `status.type.name` (`STATUS_POSTPONED`), `status.displayClock`; `-empty` is `{"events":[]}`.
- [ ] **Step 2: Write failing tests:** post fixture gives `state:"post"`, scores `3`/`1` (numbers, not strings), home abbr from the competitor with `homeAway:"home"`; live gives `state:"live"` and `clock` equal to the fixture's `displayClock`; pre gives `state:"pre"`, null scores; postponed gives `state:"postponed"`; empty gives `[]`; a competitor without `score` gives null; `crest` starts with `/api/img?u=` and contains the encoded ESPN logo URL with `w=48&h=48`.
- [ ] **Step 3:** Run, expect FAIL. Implement. Team `color` comes from ESPN `team.color` prefixed with `#` (fallback `#7C8F84`); Task 4 replaces it with the curated table.
- [ ] **Step 4:** `api/matches.ts` parses query (`leagues` default `eng.1`; `from`/`to` default today-1 to today+3 in UTC), calls `fetchMatches`, sets `Cache-Control: s-maxage=20, stale-while-revalidate=60`. Test the handler with a mocked `fetch` returning the live fixture.
- [ ] **Step 5:** Run vitest, expect PASS. Commit `feat(helper): matches endpoint`.

### Task 3: Match detail and formation layout

**Files:** Create `helper/src/formation.ts`, `helper/api/match/[id].ts`, `helper/test/detail.test.ts`, `helper/test/formation.test.ts`; fixtures `contract/fixtures/summary-post.json` (record from `.../eng.1/summary?event=401879257`), `summary-prelineup.json` (same with `rosters` removed), `detail-normalised.json`.

**Interfaces:** Produces `layoutFormation(formation: string, count: number): {x:number;y:number}[]` (index 0 is the keeper; rows from the formation digits, left to right, `y` from 0.08 to 0.92) and `normalizeSummary(raw: unknown, league: string): MatchDetail`.

- [ ] **Step 1: Write failing tests:** `layoutFormation("4-3-3",11)` has 11 points, first has `x≈0.5,y<0.15`, then 4, 3, 3 points per increasing `y`, each row sorted by `x`; `"3-1-4-2"` gives rows 3,1,4,2; unparsable formation or wrong count falls back to even spread without throwing. `normalizeSummary` on the full fixture: both lineups have 11 starters (`starter:true` in ESPN roster), `events` contain goals with minute strings like `58'`, `team` mapped to `home`/`away`; on `summary-prelineup.json`: both lineups have `players: []` and `formation: ""`.
- [ ] **Step 2:** Run, expect FAIL. Implement. Assign layout positions to starters ordered by ESPN `formationPlace`; verify against the fixture that place 1 is the keeper. `photo` is `/api/cutout?name=...&w=40&h=40` for every starter. `stats` come from `boxscore.teams[].statistics` (label, home, away) limited to possession, shots, shots on target, corners, fouls.
- [ ] **Step 3:** Handler `api/match/[id].ts` with `Cache-Control: s-maxage=15`. Run vitest, expect PASS. Commit `feat(helper): match detail with formation layout`.

### Task 4: News, teams and colours

**Files:** Create `helper/src/colors.ts`, `helper/api/news.ts`, `helper/api/teams.ts`, `helper/test/news.test.ts`, `helper/test/teams.test.ts`; fixtures `news.json` (record `.../eng.1/news`), `teams-eng1.json` (record `.../eng.1/teams`).

**Interfaces:** Produces `normalizeNews(raw: unknown): NewsItem[]`, `normalizeTeams(raw: unknown, league: string): (Team & {league:string})[]`, `teamColor(abbr: string, espnHex?: string): string`.

- [ ] **Step 1: Failing tests:** news: 6 articles from the fixture, `image` begins `/api/img?u=` with `fit=cover&w=240&h=135`, `published` is epoch seconds, article without images gives `image:null`. Teams: 20 Premier League teams, each with a valid `#RRGGBB` colour; `teamColor("ARS", "ff0000")` returns `#EF0107` (curated table wins); unknown abbr with ESPN hex returns that hex; unknown abbr with no hex returns `#7C8F84`. The curated table in `colors.ts` covers all 96 clubs of the five leagues (user confirms any wrong ones in review).
- [ ] **Step 2:** Run, expect FAIL. Implement, wire `teamColor` into Task 2's normaliser, rerun all tests, expect PASS. Commit `feat(helper): news, teams, curated colours`.

### Task 5: Image proxy and player cutouts

**Files:** Create `helper/src/images.ts`, `helper/src/sportsdb.ts`, `helper/api/img.ts`, `helper/api/cutout.ts`, `helper/test/images.test.ts`.

**Interfaces:** Produces `isAllowedImageUrl(u: string): boolean`, `resizeImage(buf: Buffer, w: number, h: number, fit: "contain"|"cover"): Promise<{data:Buffer; type:"image/png"|"image/jpeg"}>`, `findCutoutUrl(name: string, fetchImpl?: typeof fetch): Promise<string | null>` (TheSportsDB `searchplayers.php?p=`, uses `strCutout`).

- [ ] **Step 1: Failing tests:** `isAllowedImageUrl` true for the three hosts over https; false for `http://a.espncdn.com/x`, `https://evil.com/a.png`, `https://a.espncdn.com.evil.com/a.png`, `file:///etc/passwd`, and any host resolving to a private IP literal. `resizeImage` on a 200x100 PNG with `contain` to 48x48 returns a 48x48 PNG with transparent padding; `cover` gives a full JPEG-or-PNG 48x48 without padding; `w`/`h` above 480 are rejected. `findCutoutUrl("Kai Havertz")` with a mocked fetch returns the `strCutout` of the first result; empty result gives `null`.
- [ ] **Step 2:** Run, expect FAIL. Implement with `sharp`. Handlers: `img` returns 400 for a disallowed host, `Cache-Control: public, max-age=86400, immutable`; `cutout` returns 404 when null, otherwise the resized PNG.
- [ ] **Step 3:** Run, expect PASS. Commit `feat(helper): image proxy and cutouts`.

### Task 6: Deploy the helper

**Files:** Create `helper/scripts/smoke.sh`, `helper/README.md` (deploy and token steps).

- [ ] **Step 1:** Ask the user to run `! cd helper && npx vercel login` (their own account), then `npx vercel link`, set `HELPER_TOKEN` (random 32 chars) with `npx vercel env add HELPER_TOKEN production`, and `npx vercel deploy --prod`.
- [ ] **Step 2:** `smoke.sh <base-url> <token>`: expects 401 on `/api/matches` without a token, 200 with it and JSON containing `"matches"`, 200 `image/png` from `/api/img?u=https%3A%2F%2Fcrests.football-data.org%2F57.png&w=48&h=48`.
- [ ] **Step 3:** Run it against the deployed URL; Expected: `OK` printed three times. Record the URL in `helper/README.md`. Commit `docs(helper): deployment and smoke test`.

### Task 7: Simulator scaffold and fonts

**Files:** Create `sim/CMakeLists.txt`, `sim/main.cpp`, `ui/src/app.{h,cpp}`, `ui/fonts/gen.sh`, `sim/tests/smoke_test.cpp`.

**Interfaces:** Produces `void app_create(lv_obj_t* root)` (builds a tab view with three tabs Live, Matches, News, bottom bar 44 px tall, active tab chalk with accent icon) and CMake targets `sim` and `sim_tests`.

- [ ] **Step 1:** CMake with `FetchContent` for LVGL 9.2, ArduinoJson 7, doctest; SDL2 and libcurl via `find_package`. LVGL config: 240x320, 32-bit colour, SDL driver, PNG and JPEG decoders on, 4 MB memory pool.
- [ ] **Step 2:** `gen.sh` runs `lv_font_conv` to produce `ui/fonts/bigshoulders_{24,40,96}.c` and `hanken_{10,12,14}.c` from the Google Fonts TTFs (download in the script). Document the glyph ranges (ASCII plus accented Latin for player names).
- [ ] **Step 3: Failing test:** `smoke_test.cpp` initialises LVGL headless, calls `app_create`, and asserts `lv_obj_get_child_count(tabview content) == 3` and screen size is 240x320.
- [ ] **Step 4:** Build, run `ctest`: Expected PASS. Run `./sim` and confirm a 240x320 window with three tabs. Commit `feat(sim): scaffold LVGL simulator`.

### Task 8: Theme

**Files:** Create `ui/src/theme.{h,cpp}`, `sim/tests/theme_test.cpp`.

**Interfaces:** Produces `struct Palette { lv_color_t turf, panel, line, chalk, dim, amber, accent, opponent; }`, `Palette& theme()`, `void theme_set_team(uint32_t accentHex, uint32_t opponentHex)`, `lv_color_t mix(lv_color_t a, lv_color_t b, uint8_t t)`, `lv_color_t darken(lv_color_t c, uint8_t amount)`, `uint32_t parse_hex(const char* s)` (`"#EF0107"` → `0xEF0107`, invalid → `0x7C8F84`).

- [ ] **Step 1: Failing tests:** `parse_hex("#EF0107")==0xEF0107`; `parse_hex("zzz")==0x7C8F84`; `mix(a,b,0)==a`, `mix(a,b,255)==b`; default `theme().accent` is `0xEF0107`; after `theme_set_team(0x123456,0x654321)` accent changes.
- [ ] **Step 2:** Run, expect FAIL. Implement, run, expect PASS. Commit `feat(ui): palette and team theming`.

### Task 9: Model and JSON parsing against shared fixtures

**Files:** Create `ui/src/model.h`, `ui/src/json_parse.{h,cpp}`, `sim/tests/parse_test.cpp`.

**Interfaces:** Produces C++ structs mirroring the contract (`Team`, `MatchSummary`, `MatchEvent`, `LineupPlayer`, `Lineup`, `StatRow`, `MatchDetail`, `NewsItem`, enum `MatchState {Pre, Live, Post, Postponed}`) and `bool parse_matches(const std::string&, std::vector<MatchSummary>&, int64_t& updated)`, `bool parse_detail(const std::string&, MatchDetail&)`, `bool parse_news(...)`, `bool parse_teams(...)`; all return false on malformed input without crashing.

- [ ] **Step 1: Failing tests** read `contract/fixtures/matches-normalised.json`, `detail-normalised.json`, `news-normalised.json` (each written by the helper test of Tasks 2, 3 and 4 from its normaliser output; add that write to those tests) and assert: match count, first match `state`, score ints, `home.color` parsed, detail has 11 + 11 players with `x,y` in 0..1, pre-lineup detail has empty players; truncated JSON and `""` return false.
- [ ] **Step 2:** Run, expect FAIL. Implement with ArduinoJson 7 (`JsonDocument`). Run, expect PASS. Commit `feat(ui): contract model and parsers`.

### Task 10: DataSource, Store and GoalDetector

**Files:** Create `ui/src/data_source.h`, `ui/src/store.{h,cpp}`, `ui/src/goal_detector.{h,cpp}`, `sim/mock_source.cpp`, `sim/tests/goal_test.cpp`, `sim/tests/store_test.cpp`.

**Interfaces:** Produces:
`class DataSource { virtual void fetch_matches(std::function<void(bool ok, std::vector<MatchSummary>)>) = 0; virtual void fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)>) = 0; virtual void fetch_news(std::function<void(bool, std::vector<NewsItem>)>) = 0; virtual void fetch_teams(std::function<void(bool, std::vector<Team>)>) = 0; }`;
`struct GoalEvent { std::string matchId; std::string scorer; bool favourite; bool fullTime; }`;
`class GoalDetector { std::optional<GoalEvent> update(const std::vector<MatchSummary>& next, const std::string& favTeamId); }`;
`class Store { const std::vector<MatchSummary>& matches(); const MatchSummary* live_for(const std::string& teamId); const MatchSummary* next_for(const std::string& teamId, int64_t now); bool stale(int64_t now); }`.

- [ ] **Step 1: Failing tests** (goal_test): first `update` after construction with an existing 2:1 score returns nothing; then favourite's score 2→3 returns `favourite:true`; opponent scoring 1→2 returns nothing for the celebration (returns an event with `favourite:false`, which the UI shows only as the event strip); score decreasing 3→2 (VAR) returns nothing and the next increase to 3 does fire again; state `live→post` with favourite ahead returns `fullTime:true` once and not on repeat polls. Store tests: `live_for` finds the favourite's live match; with no matches `next_for` returns null; `stale(now)` is true when last successful update is older than 90 s.
- [ ] **Step 2:** Run, expect FAIL. Implement. `MockSource` serves fixtures from `contract/fixtures/` and can be scripted (`advance_score()`) for demos. Run, expect PASS. Commit `feat(ui): data source, store, goal detection`.

### Task 11: Byte-budget image cache

**Files:** Create `ui/src/lru_bytes.h`, `ui/src/image_provider.{h,cpp}`, `sim/tests/lru_test.cpp`.

**Interfaces:** Produces `template<class V> class LruBytes { LruBytes(size_t budget); void put(const std::string& key, V value, size_t bytes); V* get(const std::string& key); size_t used() const; }` and `class ImageProvider { virtual void request(const std::string& path, std::function<void(const lv_image_dsc_t*)> done) = 0; }`.

- [ ] **Step 1: Failing tests:** budget 100, put A(60) B(60) evicts A; `get(A)` null; `get(B)` before a third put makes B most recent so the older entry evicts first; a single item larger than the budget is not stored and leaves `used()` unchanged.
- [ ] **Step 2:** Run, expect FAIL. Implement (LRU list plus map). Provide `MockImageProvider` loading `sim/assets/*.png` in the sim. Run, expect PASS. Commit `feat(ui): byte-budget image cache`.

### Task 12: Live screen

**Files:** Create `ui/src/screens/live.{h,cpp}`, `sim/snapshot.cpp`, `sim/tests/live_test.cpp`.

**Interfaces:** Consumes `Store`, `ImageProvider`, `theme()`. Produces `lv_obj_t* live_create(lv_obj_t* parent, Store&, ImageProvider&, std::function<void(std::string matchId)> onOpen)`, `void live_refresh(lv_obj_t*)`, and `bool snapshot_png(lv_obj_t*, const char* path)` plus `lv_color_t pixel_at(lv_obj_t*, int x, int y)` in `snapshot.cpp`.

- [ ] **Step 1: Failing tests** using the live fixture: pixel at (6,150) is within tolerance of the home accent mixed with turf; pixel at (234,150) near the away colour; the minute pill label text equals the fixture clock; with an empty store the screen shows "Next: <opponent> <weekday hh:mm>" for the favourite's next fixture, and "No matches soon" when none; a name of 24 characters is truncated with an ellipsis and does not exceed 100 px (label width check).
- [ ] **Step 2:** Run, expect FAIL. Implement per mockup screen 1: diagonal two-colour split, 84 px crests, 96 px Big Shoulders score, amber pill with pulsing dot (opacity animation 1.2 s), glass strip with the latest event and cutout, tap anywhere on score area calls `onOpen`. Stale marker: small amber dot with "Updated 2 min ago" in the status row when `Store::stale`.
- [ ] **Step 3:** Run, expect PASS; run `./sim` and compare to the mockup by eye. Commit `feat(ui): live screen`.

### Task 13: Match detail (Summary, Pitch, Stats)

**Files:** Create `ui/src/screens/detail.{h,cpp}`, `sim/tests/detail_test.cpp`.

**Interfaces:** Produces `lv_obj_t* detail_create(lv_obj_t* parent, const MatchDetail&, ImageProvider&, std::function<void()> onBack)`. Three sub-tabs swipeable; a tapped player opens a small name card.

- [ ] **Step 1: Failing tests:** with the full detail fixture the Pitch tab contains 22 player markers (child count on the pitch container), home markers in the lower half and away in the upper half; with `summary-prelineup` detail the pitch is replaced by the text "Lineups appear about an hour before kickoff"; Summary lists goals in minute order with the scorer; Stats tab shows one row per `StatRow` and "Stats are not available for this match" when empty; formation label shows "4-3-3" with a plain-English subtitle (`4-3-3` → "4 defenders, 3 midfielders, 3 attackers").
- [ ] **Step 2:** Run, expect FAIL. Implement per mockup screen 2 (striped pitch, face if `photo` loads else shirt number). Run, expect PASS. Commit `feat(ui): match detail`.

### Task 14: Matches list

**Files:** Create `ui/src/screens/matches.{h,cpp}`, `sim/tests/matches_test.cpp`.

**Interfaces:** Produces `lv_obj_t* matches_create(lv_obj_t* parent, Store&, ImageProvider&, std::function<void(std::string)> onOpen)`.

- [ ] **Step 1: Failing tests:** day headers in order LIVE NOW, TODAY, TOMORROW, then weekday names, and YESTERDAY for past results (headers computed from kickoff in the chosen timezone; test with timezone `+05:30` that a 23:30 UTC kickoff lands on the next local day); live row has the amber edge object; favourite's rows have the dot; row labels use `abbr`; empty store shows "No matches in the next few days" with a one-line hint; tapping a row calls `onOpen(id)`.
- [ ] **Step 2:** Run, expect FAIL. Implement. Run, expect PASS. Commit `feat(ui): matches list`.

### Task 15: News screen

**Files:** Create `ui/src/screens/news.{h,cpp}`, `sim/tests/news_test.cpp`.

**Interfaces:** Produces `lv_obj_t* news_create(lv_obj_t* parent, Store&, ImageProvider&)`; the Store gains `const std::vector<NewsItem>& news()`.

- [ ] **Step 1: Failing tests:** lead story is the newest article; the two tiles show the favourite's last result (`FT`, score) and next fixture, or "No fixture yet" when unknown; list shows up to 6 headlines with relative time ("2h ago"); headline longer than two lines truncates; empty news shows "No headlines right now".
- [ ] **Step 2:** Run, expect FAIL. Implement per mockup screen 4. Run, expect PASS. Commit `feat(ui): news screen`.

### Task 16: Goal celebration

**Files:** Create `ui/src/screens/goal.{h,cpp}`, `sim/tests/goal_screen_test.cpp`.

**Interfaces:** Consumes `GoalEvent`. Produces `void goal_celebrate(const GoalEvent&, ImageProvider&)` (overlay on the top layer, auto-dismiss, tap dismisses), `bool goal_active()`.

- [ ] **Step 1: Failing tests:** after `goal_celebrate` with `fullTime:false`, `goal_active()` is true and the overlay's top-left pixel equals the accent; after advancing LVGL time by 4.2 s it is false; with `fullTime:true` it is still active at 4.2 s and ends by 8.5 s; a second call while active replaces the scorer without stacking overlays (child count of top layer stays 1); `favourite:false` events do nothing.
- [ ] **Step 2:** Run, expect FAIL. Implement per mockup screen 5 (rotating rays 6 s/turn, "GOAL!" stamp with a spring overshoot, cutout slide-up; FT version shows "FULL TIME" and the score). Respect a `reduced_motion` flag (static version). Run, expect PASS. Commit `feat(ui): goal celebration`.

### Task 17: Settings, team picker, Wi-Fi setup, dimming

**Files:** Create `ui/src/screens/{settings,team_picker,wifi}.{h,cpp}`, `ui/src/dim_schedule.{h,cpp}`, `ui/src/wifi_service.h`, `sim/mock_wifi.cpp`, `sim/tests/dim_test.cpp`, `sim/tests/wifi_screen_test.cpp`, `sim/tests/picker_test.cpp`.

**Interfaces:** Produces `class WifiService { virtual void scan(std::function<void(std::vector<WifiNetwork>)>) = 0; virtual void connect(const std::string& ssid, const std::string& password, std::function<void(bool ok)>) = 0; virtual std::vector<std::string> saved() = 0; virtual void forget(const std::string& ssid) = 0; }` with `struct WifiNetwork { std::string ssid; int rssi; bool open; }`; `DimSchedule { DimSchedule(int startHour, int endHour, uint8_t dimLevel); uint8_t brightness_at(int hour, bool recentlyTouched) const; }`; `settings_create(parent, Store&, WifiService&, ...)`; `team_picker_create(parent, const std::vector<Team>&, std::function<void(Team)>)`.

- [ ] **Step 1: Failing tests:** dim: schedule 23 to 7 at level 20 returns 20 at hour 2, full brightness at hour 12, full brightness when `recentlyTouched` at hour 2, and wrap-around works at hour 23 and 6. Wi-Fi screen with `MockWifi`: list sorted by `rssi` descending; tapping an open network connects without a keyboard; a secured one shows the on-screen keyboard; wrong password (mock returns false) shows "Could not connect. Check the password." and keeps the keyboard; saved list shows entries with a forget button, and 9 saves keep all 9 (no silent limit below 8). Picker: typing "ars" filters to Arsenal; choosing a team calls the callback and `theme().accent` becomes that team's colour.
- [ ] **Step 2:** Run, expect FAIL. Implement in the same style as the approved screens (gear icon top right of Live opens Settings). Persist choices through a small `Prefs` interface in `ui/src/prefs.h` (`get/set` string) with an in-memory sim implementation.
- [ ] **Step 3:** Run, expect PASS. Commit `feat(ui): settings, team picker, wifi setup, dimming`.

### Task 18: HTTP source, refresh policy and offline behaviour

**Files:** Create `ui/src/refresh_policy.{h,cpp}`, `sim/http_source.cpp`, `sim/tests/refresh_test.cpp`, `sim/tests/offline_test.cpp`.

**Interfaces:** Produces `class RefreshPolicy { uint32_t next_delay_ms(const Store&, int64_t now) const; }` (25000 when any match is live, else 180000; doubles after failures up to 300000) and `HttpSource(std::string baseUrl, std::string token)` implementing `DataSource` with libcurl, 8 s timeout, header `x-token`.

- [ ] **Step 1: Failing tests:** policy returns 25000 with a live match, 180000 without, 50000 after two failures with live; offline test: `MockSource` set to fail keeps the previous matches in `Store`, `stale(now)` becomes true after 90 s, nothing throws, and `live_create` still renders the last score with the stale marker; a 401 response is reported as `ok=false` with reason `"auth"` which Settings displays as "Helper rejected the token".
- [ ] **Step 2:** Run, expect FAIL. Implement. Run, expect PASS.
- [ ] **Step 3:** Run `./sim --helper <deployed-url> --token <token>` and confirm real matches, crests and news appear. Commit `feat(sim): http data source and refresh policy`.

### Task 19: Review checkpoint with the user

- [ ] **Step 1:** Run the sim with the deployed helper and screenshot each screen with `snapshot_png` into `docs/superpowers/specs/assets/sim-*.png`.
- [ ] **Step 2:** Show them to the user, collect changes on the unmocked screens (Summary, Stats, Settings, Wi-Fi, picker) and the 96-club colour table, apply, and re-run all tests (`ctest` and `cd helper && npx vitest run`). Expected: all PASS.
- [ ] **Step 3:** Check whether the supplier has sent the display and touch chip names and pin map; if so, start the hardware plan (milestones 3 and 4). Commit `docs: simulator review screenshots`.
