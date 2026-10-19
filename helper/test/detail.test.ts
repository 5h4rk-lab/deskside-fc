import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import handler from "../api/match/[id].js";
import { normalizeSummary } from "../src/espn.js";
import { buildDetailFixture } from "../scripts/build-fixtures.js";

const fixtureDir = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "contract", "fixtures");
const load = (name: string): any => JSON.parse(readFileSync(join(fixtureDir, name), "utf8"));

describe("normalizeSummary (full lineups)", () => {
  const d = normalizeSummary(load("summary-post.json"), "eng.1");

  it("builds the match summary from the header", () => {
    expect(d.match.id).toBe("401879257");
    expect(d.match.league).toBe("eng.1");
    expect(d.match.state).toBe("post");
    expect(d.match.home.abbr).toBe("TOT");
    expect(d.match.away.abbr).toBe("COV");
    expect(d.match.homeScore).toBe(3);
    expect(d.match.awayScore).toBe(1);
    expect(d.match.home.crest.startsWith("/api/img?u=")).toBe(true);
    expect(d.match.kickoff).toBe(Date.parse("2026-10-19T19:00Z") / 1000);
  });

  it("has 11 starters per side with formations", () => {
    expect(d.home.players).toHaveLength(11);
    expect(d.away.players).toHaveLength(11);
    expect(d.home.formation).toBe("3-1-4-2");
    expect(d.away.formation).toBe("4-2-3-1");
  });

  it("puts the keeper first at the back and positions inside the pitch", () => {
    expect(d.home.players[0].name).toBe("Antonín Kinsky");
    expect(d.home.players[0].number).toBe(31);
    expect(d.away.players[0].name).toBe("Carl Rushworth");
    for (const side of [d.home, d.away]) {
      expect(side.players[0].y).toBeCloseTo(0.08, 5);
      expect(side.players[0].x).toBeCloseTo(0.5, 5);
      for (const p of side.players) {
        expect(p.x).toBeGreaterThan(0);
        expect(p.x).toBeLessThan(1);
        expect(p.y).toBeGreaterThanOrEqual(0.08 - 1e-9);
        expect(p.y).toBeLessThanOrEqual(0.92 + 1e-9);
      }
    }
  });

  it("groups players into the formation rows by role, left to right", () => {
    const rowsOf = (pl: { x: number; y: number; name: string }[]) => {
      const m = new Map<number, typeof pl>();
      for (const p of pl) m.set(p.y, [...(m.get(p.y) ?? []), p]);
      return [...m.entries()].sort((a, b) => a[0] - b[0]).map(([, v]) => v);
    };
    const home = rowsOf(d.home.players);
    expect(home.map((r) => r.length)).toEqual([1, 3, 1, 4, 2]);
    // defence row of 3-1-4-2 is the three centre-backs, not wide midfielders
    expect(home[1].map((p) => p.name).sort()).toEqual(
      ["Jan Paul van Hecke", "Micky van de Ven", "Tosin Adarabioyo"].sort(),
    );
    expect(home[1][0].name).toBe("Micky van de Ven"); // CD-L is leftmost
    expect(home[1][2].name).toBe("Jan Paul van Hecke"); // CD-R is rightmost
    expect(home[3][0].name).toBe("Andy Robertson"); // LM leftmost of the four mids
    expect(home[3][3].name).toBe("Mohammed Kudus"); // RM rightmost
    const away = rowsOf(d.away.players);
    expect(away.map((r) => r.length)).toEqual([1, 4, 2, 3, 1]);
    expect(away[1][0].name).toBe("Jay Da Silva");
    expect(away[1][3].name).toBe("Milan van Ewijk");
  });

  it("gives every starter a cutout photo path", () => {
    for (const p of [...d.home.players, ...d.away.players]) {
      expect(p.photo).toBe(`/api/cutout?name=${encodeURIComponent(p.name)}&w=40&h=40`);
    }
    expect(d.home.players[0].photo).toBe("/api/cutout?name=Anton%C3%ADn%20Kinsky&w=40&h=40");
  });

  it("maps key events to goal/yellow/red/sub with home/away teams and minutes", () => {
    const goals = d.events.filter((e) => e.type === "goal");
    expect(goals.map((g) => [g.minute, g.team, g.player])).toEqual([
      ["14'", "home", "James Maddison"],
      ["49'", "away", "Jack Rudoni"],
      ["58'", "home", "Omar Marmoush"],
      ["90'+6'", "home", "Ben Davies"],
    ]);
    expect(goals[0].detail).toBe("Assist: Mohammed Kudus");
    expect(d.events.filter((e) => e.type === "yellow")).toHaveLength(4);
    expect(d.events.filter((e) => e.type === "red")).toHaveLength(0);
    const subs = d.events.filter((e) => e.type === "sub");
    expect(subs).toHaveLength(10);
    expect(subs[0]).toEqual({
      minute: "60'",
      type: "sub",
      team: "away",
      player: "Taiwo Awoniyi",
      detail: "For: Sidiki Cherif",
    });
    // kickoff, delays, halftime etc. are ignored
    expect(d.events).toHaveLength(18);
  });

  it("limits stats to the five labelled rows in order", () => {
    expect(d.stats).toEqual([
      { label: "Possession", home: "65%", away: "35%" },
      { label: "Shots", home: "16", away: "12" },
      { label: "Shots on target", home: "5", away: "1" },
      { label: "Corners", home: "4", away: "4" },
      { label: "Fouls", home: "15", away: "14" },
    ]);
  });

  it("takes home from homeAway even when rosters and teams are reordered", () => {
    const raw = load("summary-post.json");
    raw.rosters.reverse();
    raw.boxscore.teams.reverse();
    const r = normalizeSummary(raw, "eng.1");
    expect(r.home.formation).toBe("3-1-4-2");
    expect(r.away.formation).toBe("4-2-3-1");
    expect(r.stats[1]).toEqual({ label: "Shots", home: "16", away: "12" });
  });

  it("recognises red cards, penalties and own goals", () => {
    const raw = load("summary-post.json");
    const ev = raw.keyEvents.find((e: any) => e.type.type === "yellow-card");
    ev.type = { id: "93", text: "Red Card", type: "red-card" };
    const goal = raw.keyEvents.find((e: any) => e.type.type === "goal");
    goal.type = { id: "98", text: "Penalty - Scored", type: "penalty---scored" };
    const r = normalizeSummary(raw, "eng.1");
    expect(r.events.filter((e) => e.type === "red")).toHaveLength(1);
    expect(r.events.find((e) => e.type === "goal")?.detail).toBe("Penalty");
  });

  it("still lays out all 11 players when the formation string is missing", () => {
    const raw = load("summary-post.json");
    raw.rosters[0].formation = "";
    const r = normalizeSummary(raw, "eng.1");
    expect(r.home.players).toHaveLength(11);
    expect(r.home.formation).toBe("");
  });

  it("throws on a payload without a header", () => {
    expect(() => normalizeSummary({}, "eng.1")).toThrow();
    expect(() => normalizeSummary(null, "eng.1")).toThrow();
  });
});

describe("normalizeSummary (no lineups yet)", () => {
  const d = normalizeSummary(load("summary-prelineup.json"), "eng.1");

  it("returns empty lineups", () => {
    expect(d.home).toEqual({ formation: "", players: [] });
    expect(d.away).toEqual({ formation: "", players: [] });
  });

  it("still carries match, events and stats", () => {
    expect(d.match.id).toBe("401879257");
    expect(d.events.length).toBeGreaterThan(0);
  });
});

describe("detail fixture drift", () => {
  it("detail-normalised.json matches the normaliser output (run `npm run gen:fixtures` to refresh)", () => {
    expect(
      load("detail-normalised.json"),
      "detail-normalised.json is stale: run `npm run gen:fixtures` in helper/",
    ).toEqual(buildDetailFixture());
  });
});

function stubRes() {
  const res: any = { statusCode: 0, headers: {} as Record<string, string>, body: undefined };
  res.status = (c: number) => ((res.statusCode = c), res);
  res.setHeader = (k: string, v: string) => ((res.headers[k] = v), res);
  res.json = (b: unknown) => ((res.body = b), res);
  return res;
}

describe("GET /api/match/:id", () => {
  const realToken = process.env.HELPER_TOKEN;
  const ok = { headers: { "x-token": "secret" } };
  beforeEach(() => {
    process.env.HELPER_TOKEN = "secret";
  });
  afterEach(() => {
    vi.unstubAllGlobals();
    process.env.HELPER_TOKEN = realToken;
  });

  it("returns 401 without a token and does not call ESPN", async () => {
    const fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);
    const res = stubRes();
    await handler({ headers: {}, query: { id: "401879257", league: "eng.1" } } as any, res);
    expect(res.statusCode).toBe(401);
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("returns the normalised detail with a 15s cache header", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify(load("summary-post.json")), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    const res = stubRes();
    await handler({ ...ok, query: { id: "401879257", league: "eng.1" } } as any, res);
    expect(res.statusCode).toBe(200);
    expect(res.headers["Cache-Control"]).toBe("s-maxage=15");
    expect(res.body.match.id).toBe("401879257");
    expect(res.body.home.players).toHaveLength(11);
    expect(String((fetchMock.mock.calls as any)[0][0])).toBe(
      "https://site.api.espn.com/apis/site/v2/sports/soccer/eng.1/summary?event=401879257",
    );
  });

  it("rejects bad id or league with 400 before calling ESPN", async () => {
    const fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);
    for (const query of [
      { id: "abc", league: "eng.1" },
      { id: "1; rm", league: "eng.1" },
      { league: "eng.1" },
      { id: "401879257", league: "../evil" },
      { id: "401879257", league: "eng.1/x" },
    ]) {
      const res = stubRes();
      await handler({ ...ok, query } as any, res);
      expect(res.statusCode).toBe(400);
    }
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("returns 502 when ESPN fails or returns garbage", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response("x", { status: 500 })));
    const res = stubRes();
    await handler({ ...ok, query: { id: "401879257", league: "eng.1" } } as any, res);
    expect(res.statusCode).toBe(502);

    vi.stubGlobal("fetch", vi.fn(async () => new Response("{}", { status: 200 })));
    const res2 = stubRes();
    await handler({ ...ok, query: { id: "401879257", league: "eng.1" } } as any, res2);
    expect(res2.statusCode).toBe(502);
  });
});
