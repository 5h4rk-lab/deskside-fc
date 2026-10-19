import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import handler from "../api/news.js";
import { buildNewsFixture } from "../scripts/build-fixtures.js";
import { normalizeNews } from "../src/espn.js";

const fixtureDir = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "contract", "fixtures");
const load = (name: string): any => JSON.parse(readFileSync(join(fixtureDir, name), "utf8"));

describe("normalizeNews", () => {
  const raw = load("news.json");
  const items = normalizeNews(raw);

  it("returns the 6 articles of the fixture", () => {
    expect(items).toHaveLength(6);
    expect(items[0].id).toBe("50221891");
    expect(items[0].headline).toBe("Nicol: Manchester United's recruitment has to change");
  });

  it("builds a helper image path with cover fit", () => {
    const url = raw.articles[0].images[0].url;
    expect(items[0].image).not.toBeNull();
    expect(items[0].image!.startsWith("/api/img?u=")).toBe(true);
    expect(items[0].image).toContain(encodeURIComponent(url));
    expect(items[0].image!.endsWith("&fit=cover&w=240&h=135")).toBe(true);
  });

  it("gives published as epoch seconds", () => {
    expect(items[0].published).toBe(Date.parse("2026-10-19T22:17:50Z") / 1000);
    expect(Number.isInteger(items[0].published)).toBe(true);
  });

  it("gives image:null for an article without images", () => {
    const r = load("news.json");
    delete r.articles[0].images;
    r.articles[1].images = [];
    r.articles[2].images = [{ name: "no url" }];
    const out = normalizeNews(r);
    expect(out[0].image).toBeNull();
    expect(out[1].image).toBeNull();
    expect(out[2].image).toBeNull();
    expect(out[3].image).not.toBeNull();
  });

  it("returns [] for garbage", () => {
    expect(normalizeNews(null)).toEqual([]);
    expect(normalizeNews({})).toEqual([]);
  });

  it("news-normalised.json matches the normaliser output (run `npm run gen:fixtures` to refresh)", () => {
    expect(
      load("news-normalised.json"),
      "news-normalised.json is stale: run `npm run gen:fixtures` in helper/",
    ).toEqual(buildNewsFixture());
  });
});

function stubRes() {
  const res: any = { statusCode: 0, headers: {} as Record<string, string>, body: undefined };
  res.status = (c: number) => ((res.statusCode = c), res);
  res.setHeader = (k: string, v: string) => ((res.headers[k] = v), res);
  res.json = (b: unknown) => ((res.body = b), res);
  return res;
}

describe("GET /api/news", () => {
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

  it("returns articles with cache header, defaulting to eng.1", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify(load("news.json")), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    const res = stubRes();
    await handler({ headers: { "x-token": "secret" }, query: {} } as any, res);
    expect(res.statusCode).toBe(200);
    expect(res.headers["Cache-Control"]).toBe("s-maxage=300");
    expect(typeof res.body.updated).toBe("number");
    expect(res.body.articles).toHaveLength(6);
    expect(String((fetchMock.mock.calls as any)[0][0])).toBe(
      "https://site.api.espn.com/apis/site/v2/sports/soccer/eng.1/news",
    );
  });

  it("rejects a bad league with 400", async () => {
    const res = stubRes();
    await handler({ headers: { "x-token": "secret" }, query: { league: "../evil" } } as any, res);
    expect(res.statusCode).toBe(400);
  });

  it("returns 502 when ESPN fails", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response("x", { status: 500 })));
    const res = stubRes();
    await handler({ headers: { "x-token": "secret" }, query: {} } as any, res);
    expect(res.statusCode).toBe(502);
  });
});
