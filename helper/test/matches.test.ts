import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import handler from "../api/matches.js";
import { buildMatchesFixture } from "../scripts/build-fixtures.js";
import {
  clearScoreboardCache,
  DateRangeError,
  expandDates,
  fetchMatches,
  fetchMatchesDetailed,
  normalizeScoreboard,
  SCOREBOARD_CACHE_MAX,
} from "../src/espn.js";
import { fastTimeouts, hangingFetch } from "./helpers.js";

// The scoreboard cache is module state: every test starts with it empty.
beforeEach(() => clearScoreboardCache());

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
    const team = raw.events[0].competitions[0].competitors[0].team;
    team.abbreviation = "ZZZ"; // not in the curated table
    delete team.color;
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

function scoreboardWith(ids: { id: string; date: string }[]): any {
  const base = load("scoreboard-post.json");
  return {
    ...base,
    events: ids.map(({ id, date }) => {
      const ev = structuredClone(base.events[0]);
      ev.id = id;
      ev.date = date;
      return ev;
    }),
  };
}

const ok = (body: unknown) => new Response(JSON.stringify(body), { status: 200 });
const datesOf = (url: string): string => new URL(url).searchParams.get("dates") ?? "";

describe("expandDates", () => {
  it("expands an inclusive UTC range across month and year ends", () => {
    expect(expandDates("20261019", "20261021")).toEqual(["20261019", "20261020", "20261021"]);
    expect(expandDates("20261230", "20270102")).toEqual(["20261230", "20261231", "20270101", "20270102"]);
    expect(expandDates("20260228", "20260301")).toEqual(["20260228", "20260301"]);
    expect(expandDates("20261019", "20261019")).toEqual(["20261019"]);
  });

  it("rejects bad formats, impossible dates, from > to and spans over 14 days", () => {
    expect(() => expandDates("yesterday", "20261019")).toThrow(DateRangeError);
    expect(() => expandDates("20261019", "2026-10-19")).toThrow(DateRangeError);
    expect(() => expandDates("20261332", "20261333")).toThrow(DateRangeError);
    expect(() => expandDates("20260230", "20260301")).toThrow(DateRangeError);
    expect(() => expandDates("20261021", "20261019")).toThrow(DateRangeError);
    expect(() => expandDates("20261001", "20261015")).toThrow(DateRangeError); // 15 days
    expect(expandDates("20261001", "20261014")).toHaveLength(14);
  });
});

describe("fetchMatches", () => {
  it("issues one request per league per date, each with a single dates=YYYYMMDD", async () => {
    const urls: string[] = [];
    const fetchImpl = vi.fn(async (url: any) => {
      urls.push(String(url));
      return ok(load("scoreboard-empty.json"));
    }) as unknown as typeof fetch;
    await fetchMatches(["eng.1", "esp.1"], "20261019", "20261021", fetchImpl);
    const base = "https://site.api.espn.com/apis/site/v2/sports/soccer";
    const expected = ["eng.1", "esp.1"].flatMap((l) =>
      ["20261019", "20261020", "20261021"].map((d) => `${base}/${l}/scoreboard?dates=${d}`),
    );
    expect(urls).toHaveLength(6);
    expect([...urls].sort()).toEqual([...expected].sort());
    for (const u of urls) expect(datesOf(u)).toMatch(/^\d{8}$/);
  });

  it("sorts by kickoff then id across leagues", async () => {
    const early = scoreboardWith([{ id: "9", date: "2026-10-19T19:00Z" }]);
    const late = scoreboardWith([
      { id: "3", date: "2026-10-20T19:00Z" },
      { id: "2", date: "2026-10-20T19:00Z" },
    ]);
    const fetchImpl = (async (url: any) =>
      ok(String(url).includes("/esp.1/") ? early : late)) as unknown as typeof fetch;
    const out = await fetchMatches(["eng.1", "esp.1"], "20261019", "20261019", fetchImpl);
    expect(out.map((m) => [m.league, m.id])).toEqual([
      ["esp.1", "9"],
      ["eng.1", "2"],
      ["eng.1", "3"],
    ]);
  });

  it("de-duplicates an event returned for adjacent dates", async () => {
    const dup = scoreboardWith([{ id: "77", date: "2026-10-20T00:00Z" }]);
    const other = scoreboardWith([{ id: "78", date: "2026-10-20T15:00Z" }]);
    const fetchImpl = (async (url: any) => {
      const d = datesOf(String(url));
      return ok(d === "20261019" || d === "20261020" ? dup : d === "20261021" ? other : load("scoreboard-empty.json"));
    }) as unknown as typeof fetch;
    const out = await fetchMatches(["eng.1"], "20261019", "20261021", fetchImpl);
    expect(out.map((m) => m.id)).toEqual(["77", "78"]);
  });

  it("never has more than 8 requests in flight", async () => {
    let inFlight = 0;
    let peak = 0;
    const fetchImpl = vi.fn(async () => {
      inFlight++;
      peak = Math.max(peak, inFlight);
      await new Promise((r) => setTimeout(r, 5));
      inFlight--;
      return ok(load("scoreboard-empty.json"));
    }) as unknown as typeof fetch;
    await fetchMatches(["eng.1", "esp.1", "ita.1"], "20261001", "20261010", fetchImpl); // 30 requests
    expect(fetchImpl).toHaveBeenCalledTimes(30);
    expect(peak).toBe(8);
  });

  it("tolerates failed requests (non-2xx, network error, bad JSON) and reports them", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    try {
      const fetchImpl = (async (url: any) => {
        const d = datesOf(String(url));
        if (d === "20261019") return new Response("no", { status: 500 });
        if (d === "20261020") throw new Error("socket hang up");
        if (d === "20261021") return new Response("<html>", { status: 200 });
        return ok(scoreboardWith([{ id: "5", date: "2026-10-22T19:00Z" }]));
      }) as unknown as typeof fetch;
      const res = await fetchMatchesDetailed(["eng.1"], "20261019", "20261022", fetchImpl);
      expect(res.matches.map((m) => m.id)).toEqual(["5"]);
      expect(res.failed).toBe(3);
      expect(res.total).toBe(4);
      expect(errors).toHaveBeenCalledTimes(3);
    } finally {
      errors.mockRestore();
    }
  });

  it("throws when every request fails", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    try {
      const fetchImpl = (async () => new Response("no", { status: 500 })) as unknown as typeof fetch;
      await expect(fetchMatches(["eng.1"], "20261019", "20261022", fetchImpl)).rejects.toThrow();
    } finally {
      errors.mockRestore();
    }
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

  it("returns normalised matches with cache headers, one single-date request per day", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify(load("scoreboard-live.json")), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    const res = stubRes();
    await handler(
      { headers: { "x-token": "secret" }, query: { leagues: "eng.1", from: "20261018", to: "20261022" } } as any,
      res,
    );
    expect(res.statusCode).toBe(200);
    expect(res.headers["Cache-Control"]).toBe("s-maxage=20");
    expect(res.headers["Vary"]).toBe("x-token");
    expect(res.headers["x-helper-partial"]).toBeUndefined();
    expect(typeof res.body.updated).toBe("number");
    // the same live event is returned for all 5 dates and de-duplicated by id
    expect(res.body.matches).toHaveLength(1);
    expect(res.body.matches[0].state).toBe("live");
    const urls = (fetchMock.mock.calls as any).map((c: any[]) => String(c[0])).sort();
    expect(urls).toEqual(
      ["20261018", "20261019", "20261020", "20261021", "20261022"].map(
        (d) => `https://site.api.espn.com/apis/site/v2/sports/soccer/eng.1/scoreboard?dates=${d}`,
      ),
    );
  });

  it("defaults leagues to eng.1 and dates to today-1..today+7 UTC (9 single-date requests)", async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date("2026-10-19T12:00:00Z"));
    try {
      const fetchMock = vi.fn(async () => new Response(JSON.stringify(load("scoreboard-empty.json")), { status: 200 }));
      vi.stubGlobal("fetch", fetchMock);
      const res = stubRes();
      await handler({ headers: { "x-token": "secret" }, query: {} } as any, res);
      expect(res.statusCode).toBe(200);
      expect(res.body.matches).toEqual([]);
      const dates = (fetchMock.mock.calls as any).map((c: any[]) => datesOf(String(c[0]))).sort();
      expect(dates).toEqual([
        "20261018",
        "20261019",
        "20261020",
        "20261021",
        "20261022",
        "20261023",
        "20261024",
        "20261025",
        "20261026",
      ]);
      for (const c of fetchMock.mock.calls as any) expect(String(c[0])).toContain("/eng.1/scoreboard?dates=");
    } finally {
      vi.useRealTimers();
    }
  });

  it("returns partial data with x-helper-partial when some requests fail", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    try {
      const fetchMock = vi.fn(async (url: any) =>
        datesOf(String(url)) === "20261019"
          ? new Response("x", { status: 500 })
          : new Response(JSON.stringify(load("scoreboard-live.json")), { status: 200 }),
      );
      vi.stubGlobal("fetch", fetchMock);
      const res = stubRes();
      await handler(
        { headers: { "x-token": "secret" }, query: { leagues: "eng.1", from: "20261018", to: "20261020" } } as any,
        res,
      );
      expect(res.statusCode).toBe(200);
      expect(res.headers["x-helper-partial"]).toBe("1");
      expect(res.headers["Cache-Control"]).toBe("s-maxage=5");
      expect(res.headers["Vary"]).toBe("x-token");
      expect(res.body.matches).toHaveLength(1);
    } finally {
      errors.mockRestore();
    }
  });

  it("rejects malformed parameters with 400 without calling ESPN", async () => {
    const fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);
    const q = (query: Record<string, string>) => ({ headers: { "x-token": "secret" }, query }) as any;
    const bad: Record<string, string>[] = [
      { leagues: "../evil" },
      { from: "yesterday" },
      { to: "2026-10-22" },
      { from: "20261332" },
      { from: "20261022", to: "20261019" }, // from > to
      { from: "20261001", to: "20261015" }, // 15-day span
      { leagues: "a.1,b.1,c.1,d.1,e.1,f.1,g.1,h.1,i.1" }, // 9 distinct leagues
      { leagues: "eng.1,bad name" },
      { leagues: "," },
    ];
    for (const query of bad) {
      const res = stubRes();
      await handler(q(query), res);
      expect(res.statusCode, JSON.stringify(query)).toBe(400);
    }
    const ok14 = stubRes();
    vi.stubGlobal("fetch", vi.fn(async () => new Response(JSON.stringify(load("scoreboard-empty.json")), { status: 200 })));
    await handler(q({ from: "20261001", to: "20261014" }), ok14);
    expect(ok14.statusCode).toBe(200);
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("error responses carry Cache-Control: no-store (401, 400, 502)", async () => {
    const r401 = stubRes();
    await handler({ headers: {}, query: {} } as any, r401);
    expect(r401.headers["Cache-Control"]).toBe("no-store");
    const r400 = stubRes();
    await handler({ headers: { "x-token": "secret" }, query: { from: "bad" } } as any, r400);
    expect(r400.statusCode).toBe(400);
    expect(r400.headers["Cache-Control"]).toBe("no-store");
    expect(r400.headers["Vary"]).toBeUndefined();
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    try {
      vi.stubGlobal("fetch", vi.fn(async () => new Response("x", { status: 500 })));
      const r502 = stubRes();
      await handler({ headers: { "x-token": "secret" }, query: {} } as any, r502);
      expect(r502.statusCode).toBe(502);
      expect(r502.headers["Cache-Control"]).toBe("no-store");
    } finally {
      errors.mockRestore();
    }
  });

  it("de-duplicates leagues keeping order: eng.1,eng.1,esp.1 is two leagues", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify(load("scoreboard-empty.json")), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    const res = stubRes();
    await handler(
      { headers: { "x-token": "secret" }, query: { leagues: "eng.1, eng.1,esp.1,eng.1", from: "20261019", to: "20261020" } } as any,
      res,
    );
    expect(res.statusCode).toBe(200);
    const urls = (fetchMock.mock.calls as any).map((c: any[]) => String(c[0]));
    expect(urls).toHaveLength(4);
    expect(urls.filter((u: string) => u.includes("/eng.1/"))).toHaveLength(2);
    expect(urls.filter((u: string) => u.includes("/esp.1/"))).toHaveLength(2);
  });

  it("accepts exactly 8 distinct leagues, rejects 9 with 'too many leagues', and counts distinct ones only", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify(load("scoreboard-empty.json")), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    const eight = ["a.1", "b.1", "c.1", "d.1", "e.1", "f.1", "g.1", "h.1"];
    const call = async (leagues: string) => {
      const res = stubRes();
      await handler({ headers: { "x-token": "secret" }, query: { leagues, from: "20261019", to: "20261019" } } as any, res);
      return res;
    };
    expect((await call(eight.join(","))).statusCode).toBe(200);
    expect((await call([...eight, ...eight].join(","))).statusCode).toBe(200); // 16 entries, 8 distinct
    fetchMock.mockClear();
    const nine = await call([...eight, "i.1"].join(","));
    expect(nine.statusCode).toBe(400);
    expect(nine.body).toEqual({ error: "too many leagues" });
    expect(nine.headers["Cache-Control"]).toBe("no-store");
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("a hanging upstream counts as a failure and the handler still answers (partial, then 502)", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    const t = fastTimeouts();
    try {
      const fetchMock = vi.fn((url: any, init: any) =>
        datesOf(String(url)) === "20261019"
          ? (hangingFetch() as any)(url, init)
          : Promise.resolve(new Response(JSON.stringify(load("scoreboard-live.json")), { status: 200 })),
      );
      vi.stubGlobal("fetch", fetchMock);
      const res = stubRes();
      await handler(
        { headers: { "x-token": "secret" }, query: { leagues: "eng.1", from: "20261018", to: "20261020" } } as any,
        res,
      );
      expect(res.statusCode).toBe(200);
      expect(res.headers["x-helper-partial"]).toBe("1");
      expect(res.headers["Cache-Control"]).toBe("s-maxage=5");
      expect(t.requested).toEqual([8000, 5000, 5000, 5000]); // call deadline first, then one per request

      clearScoreboardCache();
      vi.stubGlobal("fetch", hangingFetch());
      const all = stubRes();
      await handler({ headers: { "x-token": "secret" }, query: { from: "20261018", to: "20261019" } } as any, all);
      expect(all.statusCode).toBe(502);
    } finally {
      t.restore();
      errors.mockRestore();
    }
  });

  it("lower-cases league codes before de-duplication, the cap and the request URLs", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify(load("scoreboard-empty.json")), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    const res = stubRes();
    await handler(
      { headers: { "x-token": "secret" }, query: { leagues: "ENG.1,eng.1,Esp.1", from: "20261019", to: "20261019" } } as any,
      res,
    );
    expect(res.statusCode).toBe(200);
    const urls = (fetchMock.mock.calls as any).map((c: any[]) => String(c[0]));
    expect(urls.sort()).toEqual([
      "https://site.api.espn.com/apis/site/v2/sports/soccer/eng.1/scoreboard?dates=20261019",
      "https://site.api.espn.com/apis/site/v2/sports/soccer/esp.1/scoreboard?dates=20261019",
    ]);
    // 4 + 4 case variants are 8 distinct leagues; adding a 9th lower/upper pair must trip the cap
    const many = ["a", "b", "c", "d", "e", "f", "g", "h"].flatMap((x) => [`${x}.1`, `${x.toUpperCase()}.1`]);
    const ok8 = stubRes();
    await handler({ headers: { "x-token": "secret" }, query: { leagues: many.join(","), from: "20261019", to: "20261019" } } as any, ok8);
    expect(ok8.statusCode).toBe(200);
    const nine = stubRes();
    await handler({ headers: { "x-token": "secret" }, query: { leagues: [...many, "I.1"].join(","), from: "20261019", to: "20261019" } } as any, nine);
    expect(nine.statusCode).toBe(400);
  });

  describe("overall deadline (8 s per call)", () => {
    // Per-request 5 s timers never fire in these tests; only the 8 s call deadline does (after 30 ms).
    function deadlineOnly() {
      const real = AbortSignal.timeout.bind(AbortSignal);
      const requested: number[] = [];
      const spy = vi.spyOn(AbortSignal, "timeout").mockImplementation((ms: number) => {
        requested.push(ms);
        return real(ms === 8000 ? 30 : 600_000);
      });
      return { requested, restore: () => spy.mockRestore() };
    }

    it("answers within the call deadline with the partial data it had, and stops starting new requests", async () => {
      const errors = vi.spyOn(console, "error").mockImplementation(() => {});
      const d = deadlineOnly();
      try {
        const hang = hangingFetch();
        const fetchMock = vi.fn((url: any, init: any) =>
          String(url).includes("/eng.1/")
            ? Promise.resolve(new Response(JSON.stringify(load("scoreboard-live.json")), { status: 200 }))
            : (hang as any)(url, init),
        );
        vi.stubGlobal("fetch", fetchMock);
        const res = stubRes();
        const started = Date.now();
        // 4 leagues x 9 default dates = 36 requests; 27 hang, only 8 can be in flight at once
        await handler({ headers: { "x-token": "secret" }, query: { leagues: "eng.1,esp.1,ita.1,ger.1" } } as any, res);
        expect(Date.now() - started).toBeLessThan(2000);
        expect(res.statusCode).toBe(200);
        expect(res.headers["x-helper-partial"]).toBe("1");
        expect(res.headers["Cache-Control"]).toBe("s-maxage=5");
        expect(res.body.matches).toHaveLength(1);
        expect(d.requested.filter((ms) => ms === 8000)).toHaveLength(1); // ONE shared deadline
        // 9 eng.1 answers + the 8 hangers that hold every worker; the other 19 never started
        expect(fetchMock.mock.calls.length).toBeLessThan(36);
      } finally {
        d.restore();
        errors.mockRestore();
      }
    });

    it("502 when nothing succeeded before the deadline", async () => {
      const errors = vi.spyOn(console, "error").mockImplementation(() => {});
      const d = deadlineOnly();
      try {
        vi.stubGlobal("fetch", hangingFetch());
        const res = stubRes();
        await handler({ headers: { "x-token": "secret" }, query: { leagues: "eng.1,esp.1" } } as any, res);
        expect(res.statusCode).toBe(502);
        expect(res.headers["Cache-Control"]).toBe("no-store");
      } finally {
        d.restore();
        errors.mockRestore();
      }
    });
  });

  it("returns 502 when every ESPN request fails", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    try {
      vi.stubGlobal("fetch", vi.fn(async () => new Response("x", { status: 500 })));
      const res = stubRes();
      await handler({ headers: { "x-token": "secret" }, query: {} } as any, res);
      expect(res.statusCode).toBe(502);
    } finally {
      errors.mockRestore();
    }
  });

  it("checks the token before validating the query", async () => {
    const res = stubRes();
    await handler({ headers: {}, query: { from: "bad" } } as any, res);
    expect(res.statusCode).toBe(401);
  });
});

describe("upstream hygiene", () => {
  it("logs a FIXED string for invalid JSON, never the exception message or the body", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    try {
      const fetchImpl = (async () => new Response("<html>LEAKY-BODY-SNIPPET</html>", { status: 200 })) as unknown as typeof fetch;
      const res = await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", fetchImpl);
      expect(res.failed).toBe(1);
      const logged = errors.mock.calls.map((c) => c.join(" ")).join("\n");
      expect(logged).toContain("upstream returned invalid JSON");
      expect(logged).not.toContain("LEAKY-BODY-SNIPPET");
      expect(logged).not.toMatch(/Unexpected token|is not valid JSON/);
    } finally {
      errors.mockRestore();
    }
  });

  it("every scoreboard request carries a 5 s timeout signal", async () => {
    const t = fastTimeouts();
    try {
      const fetchImpl = vi.fn(async () => ok(load("scoreboard-empty.json")));
      await fetchMatchesDetailed(["eng.1"], "20261019", "20261021", fetchImpl as any);
      expect(t.requested).toEqual([8000, 5000, 5000, 5000]);
      for (const c of fetchImpl.mock.calls as any[]) expect(c[1].signal).toBeInstanceOf(AbortSignal);
    } finally {
      t.restore();
    }
  });

  it("a hanging request is a counted failure, not a hang", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    const t = fastTimeouts();
    try {
      const res = await fetchMatchesDetailed(["eng.1"], "20261019", "20261020", hangingFetch());
      expect(res.failed).toBe(2);
      expect(res.total).toBe(2);
    } finally {
      t.restore();
      errors.mockRestore();
    }
  });
});

describe("scoreboard cache", () => {
  const NOW = Date.parse("2026-10-19T12:00:00Z");
  const counting = () =>
    vi.fn(async () => new Response(JSON.stringify(scoreboardWith([{ id: "1", date: "2026-10-19T19:00Z" }])), { status: 200 }));

  it("serves a second call within the TTL from cache (one upstream request)", async () => {
    let t = NOW;
    const f = counting();
    const opts = { now: () => t };
    const a = await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", f as any, opts);
    t += 19_000;
    const b = await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", f as any, opts);
    expect(f).toHaveBeenCalledTimes(1);
    expect(b.matches).toEqual(a.matches);
    expect(b.failed).toBe(0);
    expect(b.total).toBe(1);
  });

  it("expires today's date after 20 s", async () => {
    let t = NOW;
    const f = counting();
    const opts = { now: () => t };
    await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", f as any, opts);
    t += 20_000;
    await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", f as any, opts);
    expect(f).toHaveBeenCalledTimes(2);
  });

  it("keeps other dates for 10 minutes", async () => {
    let t = NOW;
    const f = counting();
    const opts = { now: () => t };
    await fetchMatchesDetailed(["eng.1"], "20261021", "20261021", f as any, opts); // a future day
    await fetchMatchesDetailed(["eng.1"], "20261017", "20261017", f as any, opts); // an older day
    t += 599_000;
    await fetchMatchesDetailed(["eng.1"], "20261021", "20261021", f as any, opts);
    await fetchMatchesDetailed(["eng.1"], "20261017", "20261017", f as any, opts);
    expect(f).toHaveBeenCalledTimes(2);
    t += 1_000;
    await fetchMatchesDetailed(["eng.1"], "20261021", "20261021", f as any, opts);
    await fetchMatchesDetailed(["eng.1"], "20261017", "20261017", f as any, opts);
    expect(f).toHaveBeenCalledTimes(4);
  });

  it("today AND yesterday (UTC) get the 20 s TTL; the day before yesterday gets 10 min", async () => {
    let t = Date.parse("2026-10-20T10:00:00Z");
    const f = counting();
    const opts = { now: () => t };
    for (const d of ["20261020", "20261019", "20261018"]) await fetchMatchesDetailed(["eng.1"], d, d, f as any, opts);
    expect(f).toHaveBeenCalledTimes(3);
    t += 19_000;
    for (const d of ["20261020", "20261019", "20261018"]) await fetchMatchesDetailed(["eng.1"], d, d, f as any, opts);
    expect(f).toHaveBeenCalledTimes(3);
    t += 1_000; // 20 s: today and yesterday expired, the older day still cached
    for (const d of ["20261020", "20261019", "20261018"]) await fetchMatchesDetailed(["eng.1"], d, d, f as any, opts);
    expect(f).toHaveBeenCalledTimes(5);
  });

  it("yesterday's late matches stay fresh across midnight: written just after UTC midnight, 20 s TTL", async () => {
    let t = Date.parse("2026-10-20T00:00:10Z");
    const f = counting();
    const opts = { now: () => t };
    await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", f as any, opts);
    t += 20_000;
    await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", f as any, opts);
    expect(f).toHaveBeenCalledTimes(2);
  });

  it("the TTL is chosen at write time: a date written as 'tomorrow' keeps its 600 s after midnight", async () => {
    let t = Date.parse("2026-10-19T23:59:50Z");
    const f = counting();
    const opts = { now: () => t };
    await fetchMatchesDetailed(["eng.1"], "20261020", "20261020", f as any, opts); // tomorrow: 600 s
    t = Date.parse("2026-10-20T00:00:30Z"); // now "today", but the entry keeps the long TTL
    await fetchMatchesDetailed(["eng.1"], "20261020", "20261020", f as any, opts);
    expect(f).toHaveBeenCalledTimes(1);
    t = Date.parse("2026-10-20T00:10:00Z"); // 610 s after the write: expired
    await fetchMatchesDetailed(["eng.1"], "20261020", "20261020", f as any, opts);
    expect(f).toHaveBeenCalledTimes(2);
  });

  it("keys by league and date", async () => {
    const f = counting();
    const opts = { now: () => NOW };
    await fetchMatchesDetailed(["eng.1", "esp.1"], "20261019", "20261020", f as any, opts);
    expect(f).toHaveBeenCalledTimes(4);
    await fetchMatchesDetailed(["esp.1", "eng.1"], "20261020", "20261020", f as any, opts);
    expect(f).toHaveBeenCalledTimes(4);
  });

  it("does not cache failures (HTTP error, network error, bad JSON)", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    try {
      const opts = { now: () => NOW };
      const bad: (() => Response | never)[] = [
        () => new Response("no", { status: 500 }),
        () => {
          throw new Error("socket hang up");
        },
        () => new Response("<html>", { status: 200 }),
      ];
      for (const mk of bad) {
        clearScoreboardCache();
        const f = vi.fn(async () => mk());
        const first = await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", f as any, opts);
        expect(first.failed).toBe(1);
        const second = await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", f as any, opts);
        expect(second.failed).toBe(1);
        expect(f).toHaveBeenCalledTimes(2);
      }
      // and a failure after a success path recovers once the upstream is back
      clearScoreboardCache();
      let healthy = false;
      const flaky = vi.fn(async () =>
        healthy ? ok(scoreboardWith([{ id: "1", date: "2026-10-19T19:00Z" }])) : new Response("no", { status: 500 }),
      );
      await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", flaky as any, opts);
      healthy = true;
      const r = await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", flaky as any, opts);
      expect(r.failed).toBe(0);
      expect(r.matches).toHaveLength(1);
    } finally {
      errors.mockRestore();
    }
  });

  it("is bounded: at most SCOREBOARD_CACHE_MAX entries, oldest evicted first", async () => {
    expect(SCOREBOARD_CACHE_MAX).toBe(256);
    const f = vi.fn(async () => ok(load("scoreboard-empty.json")));
    const opts = { now: () => NOW };
    // 257 distinct leagues for one date; the first one inserted must be evicted
    const leagues = Array.from({ length: SCOREBOARD_CACHE_MAX + 1 }, (_, i) => `l${i}.1`);
    await fetchMatchesDetailed(leagues, "20261019", "20261019", f as any, opts);
    expect(f).toHaveBeenCalledTimes(257);
    f.mockClear();
    await fetchMatchesDetailed([leagues[1]], "20261019", "20261019", f as any, opts); // still cached
    await fetchMatchesDetailed([leagues[256]], "20261019", "20261019", f as any, opts); // newest, cached
    expect(f).toHaveBeenCalledTimes(0);
    await fetchMatchesDetailed([leagues[0]], "20261019", "20261019", f as any, opts); // evicted
    expect(f).toHaveBeenCalledTimes(1);
  });

  it("the public signature stays backward compatible (default clock, no options)", async () => {
    const f = counting();
    await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", f as any);
    await fetchMatchesDetailed(["eng.1"], "20261019", "20261019", f as any);
    expect(f).toHaveBeenCalledTimes(1);
  });
});
