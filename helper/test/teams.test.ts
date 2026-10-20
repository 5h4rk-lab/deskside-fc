import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import handler from "../api/teams.js";
import { fetchTeams, normalizeScoreboard, normalizeTeams } from "../src/espn.js";
import { fastTimeouts, hangingFetch } from "./helpers.js";
import { buildTeamsFixture } from "../scripts/build-fixtures.js";

const fixtureDir = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "contract", "fixtures");
const load = (name: string): any => JSON.parse(readFileSync(join(fixtureDir, name), "utf8"));

describe("normalizeTeams", () => {
  const teams = normalizeTeams(load("teams-eng1.json"), "eng.1");

  it("returns the 20 Premier League teams tagged with the league", () => {
    expect(teams).toHaveLength(20);
    expect(teams.every((t) => t.league === "eng.1")).toBe(true);
    const ars = teams.find((t) => t.abbr === "ARS")!;
    expect(ars.name).toBe("Arsenal");
    expect(ars.id).toBe("359");
    expect(ars.color).toBe("#EF0107");
  });

  it("gives every team a valid #RRGGBB colour", () => {
    for (const t of teams) expect(t.color).toMatch(/^#[0-9A-F]{6}$/);
  });

  it("builds a helper crest path at 48x48", () => {
    const logo = load("teams-eng1.json").sports[0].leagues[0].teams[0].team.logos[0].href;
    expect(teams[0].crest.startsWith("/api/img?u=")).toBe(true);
    expect(teams[0].crest).toContain(encodeURIComponent(logo));
    expect(teams[0].crest.endsWith("&w=48&h=48")).toBe(true);
  });

  it("teams-normalised.json matches the normaliser output (run `npm run gen:fixtures` to refresh)", () => {
    expect(
      load("teams-normalised.json"),
      "teams-normalised.json is stale: run `npm run gen:fixtures` in helper/",
    ).toEqual(buildTeamsFixture());
  });

  it("returns [] for garbage", () => {
    expect(normalizeTeams(null, "eng.1")).toEqual([]);
    expect(normalizeTeams({}, "eng.1")).toEqual([]);
  });
});

describe("curated colours in the match normalisers", () => {
  it("uses the curated colour on the scoreboard", () => {
    const [m] = normalizeScoreboard(load("scoreboard-post.json"), "eng.1");
    expect(m.home.color).toBe("#FFFFFF"); // TOT
    expect(m.away.color).toBe("#5CBFEB"); // COV
  });
});

function stubRes() {
  const res: any = { statusCode: 0, headers: {} as Record<string, string>, body: undefined };
  res.status = (c: number) => ((res.statusCode = c), res);
  res.setHeader = (k: string, v: string) => ((res.headers[k] = v), res);
  res.json = (b: unknown) => ((res.body = b), res);
  return res;
}

describe("GET /api/teams", () => {
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
    expect(res.headers["Cache-Control"]).toBe("no-store");
  });

  it("fetches the five leagues and merges them with a day-long cache", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify(load("teams-eng1.json")), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    const res = stubRes();
    await handler({ headers: { "x-token": "secret" }, query: {} } as any, res);
    expect(res.statusCode).toBe(200);
    expect(res.headers["Cache-Control"]).toBe("s-maxage=86400");
    expect(res.headers["Vary"]).toBe("x-token");
    const urls = (fetchMock.mock.calls as any).map((c: any) => String(c[0]));
    for (const l of ["eng.1", "esp.1", "ita.1", "ger.1", "fra.1"]) {
      expect(urls).toContain(`https://site.api.espn.com/apis/site/v2/sports/soccer/${l}/teams`);
    }
    expect(res.body.teams).toHaveLength(100);
    expect(res.body.teams[0].league).toBe("eng.1");
    expect(res.body.teams[20].league).toBe("esp.1");
  });

  it("returns 502 when ESPN fails", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response("x", { status: 500 })));
    const res = stubRes();
    await handler({ headers: { "x-token": "secret" }, query: {} } as any, res);
    expect(res.statusCode).toBe(502);
    expect(res.headers["Cache-Control"]).toBe("no-store");
  });

  it("still answers when one league fails (the picker is not emptied)", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    try {
      vi.stubGlobal(
        "fetch",
        vi.fn(async (url: any) =>
          String(url).includes("/ita.1/") ? new Response("x", { status: 500 }) : new Response(JSON.stringify(load("teams-eng1.json")), { status: 200 }),
        ),
      );
      const res = stubRes();
      await handler({ headers: { "x-token": "secret" }, query: {} } as any, res);
      expect(res.statusCode).toBe(200);
      expect(res.body.teams).toHaveLength(80);
      expect(res.body.teams.some((t: any) => t.league === "ita.1")).toBe(false);
    } finally {
      errors.mockRestore();
    }
  });

  it("a hanging league is a failure like any other", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    const t = fastTimeouts();
    try {
      vi.stubGlobal("fetch", hangingFetch());
      const res = stubRes();
      await handler({ headers: { "x-token": "secret" }, query: {} } as any, res);
      expect(res.statusCode).toBe(502);
      expect(t.requested).toEqual([5000, 5000, 5000, 5000, 5000]);
    } finally {
      t.restore();
      errors.mockRestore();
    }
  });
});

describe("fetchTeams", () => {
  it("returns the leagues that worked and throws only when every league fails", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    try {
      const body = JSON.stringify(load("teams-eng1.json"));
      const some = (async (url: any) =>
        String(url).includes("/esp.1/") ? new Response("x", { status: 503 }) : new Response(body, { status: 200 })) as unknown as typeof fetch;
      const teams = await fetchTeams(["eng.1", "esp.1"], some);
      expect(teams).toHaveLength(20);
      expect(new Set(teams.map((t) => t.league))).toEqual(new Set(["eng.1"]));

      const none = (async () => new Response("x", { status: 500 })) as unknown as typeof fetch;
      await expect(fetchTeams(["eng.1", "esp.1"], none)).rejects.toThrow();
      const throws = (async () => {
        throw new Error("boom");
      }) as unknown as typeof fetch;
      await expect(fetchTeams(["eng.1"], throws)).rejects.toThrow();
    } finally {
      errors.mockRestore();
    }
  });

  it("logs failed leagues with a fixed message for invalid JSON", async () => {
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    try {
      const f = (async () => new Response("<html>LEAKY</html>", { status: 200 })) as unknown as typeof fetch;
      await expect(fetchTeams(["eng.1"], f)).rejects.toThrow();
      const logged = errors.mock.calls.map((c) => c.join(" ")).join("\n");
      expect(logged).toContain("upstream returned invalid JSON");
      expect(logged).not.toContain("LEAKY");
    } finally {
      errors.mockRestore();
    }
  });
});
