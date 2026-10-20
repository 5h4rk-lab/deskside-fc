import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import handler from "../api/match/[id].js";
import { normalizeSummary } from "../src/espn.js";
import { buildDetailFixture } from "../scripts/build-fixtures.js";
import { fastTimeouts, hangingFetch } from "./helpers.js";

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

describe("normalizeSummary row assignment guard", () => {
  // Rewrite the home starters: arr[i] gives the position abbreviation of formationPlace i+1.
  function withHomeRoles(formation: string, abbrs: string[]) {
    const raw = load("summary-post.json");
    const home = raw.rosters.find((r: any) => r.homeAway === "home");
    home.formation = formation;
    const starters = home.roster
      .filter((p: any) => p.starter)
      .sort((a: any, b: any) => a.formationPlace - b.formationPlace);
    const abbrOf = new Map<string, string>();
    starters.forEach((p: any, i: number) => {
      p.formationPlace = i + 1;
      p.position = { abbreviation: abbrs[i] };
      abbrOf.set(p.athlete.displayName, abbrs[i]);
    });
    const lineup = normalizeSummary(raw, "eng.1").home;
    const byY = new Map<number, string[]>();
    for (const p of lineup.players) byY.set(p.y, [...(byY.get(p.y) ?? []), abbrOf.get(p.name)!]); // players are in x order per row
    return [...byY.entries()].sort((a, b) => a[0] - b[0]).map(([, v]) => v);
  }

  it("role-rank rule: 4-3-3 with LW/CF/RW matches the formation, so rows follow positions", () => {
    // formationPlace order deliberately differs from the pitch order
    const rows = withHomeRoles("4-3-3", ["G", "CM", "RW", "CD-R", "LB", "CM-R", "CF", "RB", "CD-L", "LW", "CM-L"]);
    expect(rows).toEqual([["G"], ["LB", "CD-L", "CD-R", "RB"], ["CM-L", "CM", "CM-R"], ["LW", "CF", "RW"]]);
  });

  it("fallback rule: 4-2-3-1 written as AM/LW/CF/RW does not match the formation, so rows follow formationPlace", () => {
    // role ranks give row sizes 4,2,1,3 against 4-2-3-1, so the position table is not trusted
    const rows = withHomeRoles("4-2-3-1", ["G", "RB", "LB", "CD-R", "CD-L", "CM-L", "CM-R", "LW", "AM", "RW", "CF"]);
    expect(rows).toEqual([["G"], ["LB", "CD-L", "CD-R", "RB"], ["CM-L", "CM-R"], ["LW", "AM", "RW"], ["CF"]]);
  });

  it("fallback rule: an unknown abbreviation is not silently ranked as midfield", () => {
    // with role rank an unknown would join the midfield; the fallback slices by formationPlace instead
    const rows = withHomeRoles("4-3-3", ["G", "RB", "LB", "CD-R", "CD-L", "CM-L", "RW", "CM-R", "LW", "CF", "XX"]);
    expect(rows).toEqual([["G"], ["LB", "CD-L", "CD-R", "RB"], ["CM-L", "CM-R", "RW"], ["LW", "CF", "XX"]]);
  });

  it("fallback rule: two keepers or a missing keeper never crashes and keeps all 11", () => {
    const rows = withHomeRoles("4-3-3", ["G", "G", "LB", "CD-R", "CD-L", "CM-L", "CM", "CM-R", "LW", "CF", "RW"]);
    expect(rows.flat()).toHaveLength(11);
    expect(rows[0]).toEqual(["G"]);
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
    expect(res.headers["Cache-Control"]).toBe("no-store");
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("returns the normalised detail with a 15s cache header", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify(load("summary-post.json")), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    const res = stubRes();
    await handler({ ...ok, query: { id: "401879257", league: "eng.1" } } as any, res);
    expect(res.statusCode).toBe(200);
    expect(res.headers["Cache-Control"]).toBe("s-maxage=15");
    expect(res.headers["Vary"]).toBe("x-token");
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
      expect(res.headers["Cache-Control"]).toBe("no-store");
    }
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("returns 502 when ESPN fails or returns garbage", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response("x", { status: 500 })));
    const res = stubRes();
    await handler({ ...ok, query: { id: "401879257", league: "eng.1" } } as any, res);
    expect(res.statusCode).toBe(502);
    expect(res.headers["Cache-Control"]).toBe("no-store");

    vi.stubGlobal("fetch", vi.fn(async () => new Response("{}", { status: 200 })));
    const res2 = stubRes();
    await handler({ ...ok, query: { id: "401879257", league: "eng.1" } } as any, res2);
    expect(res2.statusCode).toBe(502);
  });

  it("a hanging ESPN answers 502 within the 5 s deadline", async () => {
    const t = fastTimeouts();
    try {
      vi.stubGlobal("fetch", hangingFetch());
      const res = stubRes();
      await handler({ ...ok, query: { id: "401879257", league: "eng.1" } } as any, res);
      expect(res.statusCode).toBe(502);
      expect(res.headers["Cache-Control"]).toBe("no-store");
      expect(t.requested).toEqual([5000]);
    } finally {
      t.restore();
    }
  });
});
