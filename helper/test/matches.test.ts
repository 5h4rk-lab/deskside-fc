import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import handler from "../api/matches.js";
import { buildMatchesFixture } from "../scripts/build-fixtures.js";
import { fetchMatches, normalizeScoreboard } from "../src/espn.js";

const fixtureDir = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "contract", "fixtures");
const load = (name: string): any => JSON.parse(readFileSync(join(fixtureDir, name), "utf8"));

describe("normalizeScoreboard", () => {
  it("normalises a finished match", () => {
    const [m] = normalizeScoreboard(load("scoreboard-post.json"), "eng.1");
    expect(m.id).toBe("401879257");
    expect(m.league).toBe("eng.1");
    expect(m.state).toBe("post");
    expect(m.clock).toBeNull();
    expect(m.kickoff).toBe(Date.parse("2026-10-19T19:00Z") / 1000);
    expect(m.homeScore).toBe(3);
    expect(m.awayScore).toBe(1);
    expect(m.home.abbr).toBe("TOT");
    expect(m.away.abbr).toBe("COV");
    expect(m.home.id).toBe("367");
    expect(m.home.name).toBe("Tottenham Hotspur");
    expect(m.home.color).toMatch(/^#[0-9A-Fa-f]{6}$/);
  });

  it("takes home from the competitor flagged homeAway=home regardless of order", () => {
    const raw = load("scoreboard-post.json");
    raw.events[0].competitions[0].competitors.reverse();
    const [m] = normalizeScoreboard(raw, "eng.1");
    expect(m.home.abbr).toBe("TOT");
    expect(m.homeScore).toBe(3);
    expect(m.away.abbr).toBe("COV");
    expect(m.awayScore).toBe(1);
  });

  it("normalises a live match with its display clock", () => {
    const raw = load("scoreboard-live.json");
    const [m] = normalizeScoreboard(raw, "eng.1");
    expect(m.state).toBe("live");
    expect(m.clock).toBe(raw.events[0].status.displayClock);
  });

  it("normalises a pre-match fixture with null scores", () => {
    const [m] = normalizeScoreboard(load("scoreboard-pre.json"), "eng.1");
    expect(m.state).toBe("pre");
    expect(m.homeScore).toBeNull();
    expect(m.awayScore).toBeNull();
    expect(m.clock).toBeNull();
  });

  it("nulls scores for a pre match even when ESPN sends score \"0\"", () => {
    const raw = load("scoreboard-pre.json");
    for (const c of raw.events[0].competitions[0].competitors) c.score = "0";
    const [m] = normalizeScoreboard(raw, "eng.1");
    expect(m.state).toBe("pre");
    expect(m.homeScore).toBeNull();
    expect(m.awayScore).toBeNull();
  });

  it("nulls scores for a postponed match even when ESPN sends score \"0\"", () => {
    const raw = load("scoreboard-postponed.json");
    for (const c of raw.events[0].competitions[0].competitors) c.score = "0";
    const [m] = normalizeScoreboard(raw, "eng.1");
    expect(m.state).toBe("postponed");
    expect(m.homeScore).toBeNull();
    expect(m.awayScore).toBeNull();
  });

  it("normalises a postponed match", () => {
    const [m] = normalizeScoreboard(load("scoreboard-postponed.json"), "eng.1");
    expect(m.state).toBe("postponed");
  });

  it("returns [] for an empty scoreboard and for garbage", () => {
    expect(normalizeScoreboard(load("scoreboard-empty.json"), "eng.1")).toEqual([]);
    expect(normalizeScoreboard(null, "eng.1")).toEqual([]);
    expect(normalizeScoreboard({}, "eng.1")).toEqual([]);
  });

  it("gives null for a competitor without a score", () => {
    const raw = load("scoreboard-post.json");
    delete raw.events[0].competitions[0].competitors[0].score;
    const [m] = normalizeScoreboard(raw, "eng.1");
    expect(m.homeScore).toBeNull();
    expect(m.awayScore).toBe(1);
  });

  it("builds a helper-relative crest path", () => {
    const raw = load("scoreboard-post.json");
    const [m] = normalizeScoreboard(raw, "eng.1");
    const logo = raw.events[0].competitions[0].competitors[0].team.logo;
    expect(m.home.crest.startsWith("/api/img?u=")).toBe(true);
    expect(m.home.crest).toContain(encodeURIComponent(logo));
    expect(m.home.crest.endsWith("&w=48&h=48")).toBe(true);
  });

  it("uses the fallback colour when ESPN has none", () => {
    const raw = load("scoreboard-post.json");
    delete raw.events[0].competitions[0].competitors[0].team.color;
    const [m] = normalizeScoreboard(raw, "eng.1");
    expect(m.home.color).toBe("#7C8F84");
  });

  it("matches-normalised.json matches the normaliser output (run `npm run gen:fixtures` to refresh)", () => {
    expect(
      load("matches-normalised.json"),
      "matches-normalised.json is stale: run `npm run gen:fixtures` in helper/",
    ).toEqual(buildMatchesFixture());
  });
});

describe("fetchMatches", () => {
  it("calls ESPN once per league with the date range and sorts by kickoff", async () => {
    const early = load("scoreboard-post.json");
    const late = load("scoreboard-pre.json");
    late.events[0].date = "2026-10-20T19:00Z";
    const urls: string[] = [];
    const fetchImpl = vi.fn(async (url: any) => {
      urls.push(String(url));
      const body = String(url).includes("/esp.1/") ? early : late;
      return new Response(JSON.stringify(body), { status: 200 });
    }) as unknown as typeof fetch;
    const out = await fetchMatches(["eng.1", "esp.1"], "20261019", "20261022", fetchImpl);
    expect(urls).toHaveLength(2);
    expect(urls[0]).toContain("/soccer/eng.1/scoreboard?dates=20261019-20261022");
    expect(out.map((m) => m.league)).toEqual(["esp.1", "eng.1"]);
    expect(out[0].kickoff).toBeLessThan(out[1].kickoff);
  });

  it("throws on a non-200 ESPN response", async () => {
    const fetchImpl = (async () => new Response("no", { status: 500 })) as unknown as typeof fetch;
    await expect(fetchMatches(["eng.1"], "20261019", "20261022", fetchImpl)).rejects.toThrow();
  });
});

function stubRes() {
  const res: any = { statusCode: 0, headers: {} as Record<string, string>, body: undefined };
  res.status = (c: number) => ((res.statusCode = c), res);
  res.setHeader = (k: string, v: string) => ((res.headers[k] = v), res);
  res.json = (b: unknown) => ((res.body = b), res);
  return res;
}

describe("GET /api/matches", () => {
  const realToken = process.env.HELPER_TOKEN;
  beforeEach(() => {
    process.env.HELPER_TOKEN = "secret";
  });
  afterEach(() => {
    vi.unstubAllGlobals();
    process.env.HELPER_TOKEN = realToken;
  });

  it("returns 401 without a token", async () => {
    const res = stubRes();
    await handler({ headers: {}, query: {} } as any, res);
    expect(res.statusCode).toBe(401);
  });

  it("returns normalised matches with cache headers", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify(load("scoreboard-live.json")), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    const res = stubRes();
    await handler(
      { headers: { "x-token": "secret" }, query: { leagues: "eng.1", from: "20261018", to: "20261022" } } as any,
      res,
    );
    expect(res.statusCode).toBe(200);
    expect(res.headers["Cache-Control"]).toBe("s-maxage=20, stale-while-revalidate=60");
    expect(typeof res.body.updated).toBe("number");
    expect(res.body.matches).toHaveLength(1);
    expect(res.body.matches[0].state).toBe("live");
    const calledUrl = String((fetchMock.mock.calls as any)[0][0]);
    expect(calledUrl).toContain("/eng.1/scoreboard?dates=20261018-20261022");
  });

  it("defaults leagues to eng.1 and dates to today-1..today+3 UTC", async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date("2026-10-19T12:00:00Z"));
    try {
      const fetchMock = vi.fn(async () => new Response(JSON.stringify(load("scoreboard-empty.json")), { status: 200 }));
      vi.stubGlobal("fetch", fetchMock);
      const res = stubRes();
      await handler({ headers: { "x-token": "secret" }, query: {} } as any, res);
      expect(res.statusCode).toBe(200);
      expect(res.body.matches).toEqual([]);
      expect(String((fetchMock.mock.calls as any)[0][0])).toContain("/eng.1/scoreboard?dates=20261018-20261022");
    } finally {
      vi.useRealTimers();
    }
  });

  it("rejects malformed parameters with 400", async () => {
    const res = stubRes();
    await handler({ headers: { "x-token": "secret" }, query: { leagues: "../evil" } } as any, res);
    expect(res.statusCode).toBe(400);
    const res2 = stubRes();
    await handler({ headers: { "x-token": "secret" }, query: { from: "yesterday" } } as any, res2);
    expect(res2.statusCode).toBe(400);
  });

  it("returns 502 when ESPN fails", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response("x", { status: 500 })));
    const res = stubRes();
    await handler({ headers: { "x-token": "secret" }, query: {} } as any, res);
    expect(res.statusCode).toBe(502);
  });
});
