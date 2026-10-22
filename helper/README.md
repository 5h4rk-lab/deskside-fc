# soccer-display helper

Vercel project (TypeScript, Node 22) between the display device and ESPN: it normalises ESPN data
and proxies/resizes images. Every endpoint needs the `x-token` header.

- `GET /api/health`: liveness check.
- `GET /api/matches?leagues=eng.1&from=YYYYMMDD&to=YYYYMMDD`: matches by kickoff. Default window is yesterday..+7 days UTC (9 days); the maximum span is 14 days. League codes are lower-cased, duplicates are ignored (order kept) and at most 8 distinct leagues are accepted. ESPN rejects date ranges, so it makes one request per league per day; partial results (some requests failed or timed out) carry `x-helper-partial: 1` and a shorter cache lifetime.
- `GET /api/match/<id>?league=eng.1`: match detail (events, lineups, stats).
- `GET /api/news?league=eng.1`: latest headlines.
- `GET /api/teams`: team list for the team picker.
- `GET /api/img?u=<url>&w=48&h=48`: image proxy (allow-listed hosts, resized PNG).
- `GET /api/cutout?name=<player>&w=40&h=40`: player cutout photo.
- `GET /api/firmware/version`: the newest published device firmware (see "Firmware updates").

Every team object carries `color` (readable accent for small details, curated in `src/colors.ts` with a luminance floor) and `primary` (the club's real brand colour for large surfaces, may be dark; white-kit clubs use their dark identity colour).

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
- `s-maxage`: matches 20 s (5 s when partial; no `stale-while-revalidate`, so live scores are never served stale on top of the TTLs), match detail 15 s, news 300 s, teams 86400 s, images and cutouts 86400 s (with `max-age=86400`).
- In-process scoreboard cache (per warm function instance, not shared): one entry per league and date. TTL 20 s for the current UTC day and the previous UTC day (ESPN's dates are not necessarily UTC days, so yesterday's late matches may still be live around midnight) and 10 min for every other date. The TTL is chosen when the entry is written: a date fetched as "tomorrow" keeps its 10 min after UTC midnight turns it into "today". Only successful ESPN answers are cached; at most 256 entries, oldest evicted first.
- Upstream timeouts: 5 s per ESPN/TheSportsDB request (a timeout counts as a failure); `/api/matches` also has one 8 s deadline for the whole call: requests still running are aborted and requests not yet started are skipped, all counted as failed (partial response with `x-helper-partial: 1`, or 502 if nothing succeeded); `/api/cutout` shares one 7 s deadline for the player lookup and the image download, because the device gives up after 10 s.

Deploy (production URL https://soccer-display-helper.vercel.app):

    vercel link --yes --project soccer-display-helper
    vercel env add HELPER_TOKEN production --sensitive
    vercel deploy --prod --yes

Smoke test: `HELPER_TOKEN=... scripts/smoke.sh https://soccer-display-helper.vercel.app` (a token argument is still accepted but shows up in the process list). It also requests the authenticated matches URL without a token right afterwards and expects 401, which catches a CDN serving a cached body to anyone.
The token lives in `~/.config/soccer-display/helper.env`; never commit it. Unit tests: `npx vitest run`.

## Firmware updates

The device checks `GET /api/firmware/version` (token-gated, `Cache-Control: no-store`) about 2 minutes after
it starts, then every 6 hours, and when Settings is opened (at most every 10 minutes). It installs only
when the user taps "Version X available · Install" in Settings.

- Answer: `{"version": "0.1.1", "url": "https://<this host>/firmware/0.1.1.bin", "sha256": "<64 hex>", "size": <bytes>, "notes": "..."}`.
  The URL is built from the request's host, and the device accepts only an https URL on the helper's own host.
  While nothing is published (`{"version": null}`) the answer is 404, which the device reads as "up to date".
  A manifest that fails a check (version, file name, hash, size 100 KB to 5 MB, notes up to 200 characters) gives 500 and is never served.
- Source: `firmware-manifest.json` (`version`, `file` = `firmware/<version>.bin`, `sha256`, `size`, `notes`).
- Binaries: `public/firmware/<version>.bin`, served as static files. They are public and contain no secret.
  Their integrity comes from the SHA-256 in the token-gated manifest, which the device checks before switching,
  together with the image's project name ("soccer-display") and version. Keep older binaries: a device may be mid-download.
- Publish (from the repo root): `tools/publish-firmware.sh --notes "One line"`. It builds the release `app`, checks the image,
  copies it here and rewrites the manifest. It refuses a `secrets.ini` with HELPER_URL/HELPER_TOKEN, a set
  `PLATFORMIO_BUILD_FLAGS`, uncommitted firmware sources, and a version that is not newer. It deploys nothing:
  run `npx vitest run` (it checks the manifest against the file), commit both files, then deploy as above.
  After deploying, `curl -sI https://<helper>/firmware/<version>.bin` should answer 200 with the right Content-Length.
  The project has no framework preset, so Vercel serves `public/` as static output. Check this once on the first deploy.
