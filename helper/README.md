# soccer-display helper

Vercel project (TypeScript, Node 22) between the display device and ESPN: it normalises ESPN data
and proxies/resizes images. Every endpoint needs the `x-token` header.

- `GET /api/health`: liveness check.
- `GET /api/matches?leagues=eng.1&from=YYYYMMDD&to=YYYYMMDD`: matches by kickoff (default yesterday..+3 days UTC, max 14 days). ESPN rejects date ranges, so it makes one request per league per day; partial results carry `x-helper-partial: 1`.
- `GET /api/match/<id>?league=eng.1`: match detail (events, lineups, stats).
- `GET /api/news?league=eng.1`: latest headlines.
- `GET /api/teams`: team list for the team picker.
- `GET /api/img?u=<url>&w=48&h=48`: image proxy (allow-listed hosts, resized PNG).
- `GET /api/cutout?name=<player>&w=40&h=40`: player cutout photo.

Deploy (production URL https://soccer-display-helper.vercel.app):

    vercel link --yes --project soccer-display-helper
    vercel env add HELPER_TOKEN production --sensitive
    vercel deploy --prod --yes

Smoke test: `HELPER_TOKEN=... scripts/smoke.sh https://soccer-display-helper.vercel.app`.
The token lives in `~/.config/soccer-display/helper.env`; never commit it. Unit tests: `npx vitest run`.
