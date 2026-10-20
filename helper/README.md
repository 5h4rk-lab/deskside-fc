# soccer-display helper

Vercel project (TypeScript, Node 22) between the display device and ESPN: it normalises ESPN data
and proxies/resizes images. Every endpoint needs the `x-token` header.

- `GET /api/health`: liveness check.
- `GET /api/matches?leagues=eng.1&from=YYYYMMDD&to=YYYYMMDD`: matches by kickoff. Default window is yesterday..+7 days UTC (9 days); the maximum span is 14 days. Duplicate leagues are ignored (order kept) and at most 8 distinct leagues are accepted. ESPN rejects date ranges, so it makes one request per league per day; partial results (some requests failed or timed out) carry `x-helper-partial: 1` and a shorter cache lifetime.
- `GET /api/match/<id>?league=eng.1`: match detail (events, lineups, stats).
- `GET /api/news?league=eng.1`: latest headlines.
- `GET /api/teams`: team list for the team picker.
- `GET /api/img?u=<url>&w=48&h=48`: image proxy (allow-listed hosts, resized PNG).
- `GET /api/cutout?name=<player>&w=40&h=40`: player cutout photo.

### 400 responses

`/api/matches` answers 400 (JSON `{"error": ...}`, before any ESPN request) for:

- `from` later than `to`;
- a span over 14 days (`from..to` inclusive);
- bad dates: not `YYYYMMDD`, or not a real calendar date (`20261332`, `2026-10-22`, `yesterday`);
- more than 8 distinct leagues (`too many leagues`);
- bad league names (anything outside letters, digits, `.`, `-`, `_`, or containing `..`; an empty list).

`/api/match`, `/api/news` (bad league or id) and `/api/img`, `/api/cutout` (bad URL host, size outside 1..480, bad fit or name) also answer 400.

### Caching

- Every success response that sets `s-maxage` also sends `Vary: x-token`, so the CDN never serves a cached body to a request without the right token. Every error response (401, 400, 404, 502) sends `Cache-Control: no-store`.
- `s-maxage`: matches 20 s (5 s when partial), match detail 15 s, news 300 s, teams 86400 s, images and cutouts 86400 s (with `max-age=86400`).
- In-process scoreboard cache (per warm function instance, not shared): one entry per league and date. TTL 20 s for the current UTC day and 10 min for every other day. Only successful ESPN answers are cached; at most 256 entries, oldest evicted first.
- Upstream timeouts: 5 s per ESPN/TheSportsDB request (a timeout counts as a failure); `/api/cutout` shares one 7 s deadline for the player lookup and the image download, because the device gives up after 10 s.

Deploy (production URL https://soccer-display-helper.vercel.app):

    vercel link --yes --project soccer-display-helper
    vercel env add HELPER_TOKEN production --sensitive
    vercel deploy --prod --yes

Smoke test: `HELPER_TOKEN=... scripts/smoke.sh https://soccer-display-helper.vercel.app` (a token argument is still accepted but shows up in the process list). It also requests the authenticated matches URL without a token right afterwards and expects 401, which catches a CDN serving a cached body to anyone.
The token lives in `~/.config/soccer-display/helper.env`; never commit it. Unit tests: `npx vitest run`.
