import sharp from "sharp";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import cutoutHandler from "../api/cutout.js";
import imgHandler from "../api/img.js";
import { ALLOWED_IMAGE_HOSTS, isAllowedImageUrl, resizeImage } from "../src/images.js";
import { findCutoutUrl } from "../src/sportsdb.js";

async function makePng(w: number, h: number): Promise<Buffer> {
  return sharp({
    create: { width: w, height: h, channels: 4, background: { r: 200, g: 30, b: 30, alpha: 1 } },
  })
    .png()
    .toBuffer();
}

async function makeJpeg(w: number, h: number): Promise<Buffer> {
  return sharp({
    create: { width: w, height: h, channels: 3, background: { r: 20, g: 120, b: 40 } },
  })
    .jpeg()
    .toBuffer();
}

async function alphaAt(buf: Buffer, x: number, y: number): Promise<number> {
  const { data, info } = await sharp(buf).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
  return data[(y * info.width + x) * info.channels + 3];
}

function stubRes() {
  const res: any = { statusCode: 0, headers: {} as Record<string, string>, body: undefined };
  res.status = (c: number) => ((res.statusCode = c), res);
  res.setHeader = (k: string, v: string) => ((res.headers[k] = v), res);
  res.json = (b: unknown) => ((res.body = b), res);
  res.send = (b: unknown) => ((res.body = b), res);
  return res;
}

describe("isAllowedImageUrl", () => {
  it("allows the three hosts over https", () => {
    expect(ALLOWED_IMAGE_HOSTS).toEqual(["a.espncdn.com", "r2.thesportsdb.com", "crests.football-data.org"]);
    for (const h of ALLOWED_IMAGE_HOSTS) {
      expect(isAllowedImageUrl(`https://${h}/x/y.png`)).toBe(true);
    }
  });

  it("refuses everything else", () => {
    for (const u of [
      "http://a.espncdn.com/x",
      "https://evil.com/a.png",
      "https://a.espncdn.com.evil.com/a.png",
      "https://evil.com/a.espncdn.com",
      "https://a.espncdn.com@evil.com/",
      "https://user:pw@a.espncdn.com/x.png",
      "https://a.espncdn.com:8443/x.png",
      "https://a.espncdn.com./x.png",
      "file:///etc/passwd",
      "ftp://a.espncdn.com/x",
      "https://127.0.0.1/x.png",
      "https://10.0.0.1/x.png",
      "https://[::1]/x.png",
      "https://169.254.169.254/latest/meta-data",
      "not a url",
      "",
    ]) {
      expect(isAllowedImageUrl(u), u).toBe(false);
    }
  });
});

describe("resizeImage", () => {
  it("contain: 200x100 PNG to 48x48 PNG with transparent padding", async () => {
    const out = await resizeImage(await makePng(200, 100), 48, 48, "contain");
    expect(out.type).toBe("image/png");
    const meta = await sharp(out.data).metadata();
    expect(meta.format).toBe("png");
    expect(meta.width).toBe(48);
    expect(meta.height).toBe(48);
    expect(await alphaAt(out.data, 0, 0)).toBe(0);
    expect(await alphaAt(out.data, 24, 24)).toBe(255);
  });

  it("contain keeps PNG even for a JPEG source", async () => {
    const out = await resizeImage(await makeJpeg(200, 100), 48, 48, "contain");
    expect(out.type).toBe("image/png");
    expect((await sharp(out.data).metadata()).format).toBe("png");
  });

  it("cover: exactly 48x48 without padding, PNG for a PNG source", async () => {
    const out = await resizeImage(await makePng(200, 100), 48, 48, "cover");
    expect(out.type).toBe("image/png");
    const meta = await sharp(out.data).metadata();
    expect([meta.width, meta.height]).toEqual([48, 48]);
    expect(await alphaAt(out.data, 0, 0)).toBe(255);
  });

  it("cover: JPEG source gives a 48x48 JPEG", async () => {
    const out = await resizeImage(await makeJpeg(200, 100), 48, 48, "cover");
    expect(out.type).toBe("image/jpeg");
    const meta = await sharp(out.data).metadata();
    expect(meta.format).toBe("jpeg");
    expect([meta.width, meta.height]).toEqual([48, 48]);
  });

  it("rejects SVG and garbage", async () => {
    const svg = Buffer.from('<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><rect width="10" height="10"/></svg>');
    await expect(resizeImage(svg, 48, 48, "contain")).rejects.toThrow();
    await expect(resizeImage(Buffer.from("nope"), 48, 48, "contain")).rejects.toThrow();
  });
});

describe("findCutoutUrl", () => {
  it("returns the first result's strCutout and queries by name", async () => {
    const fetchMock = vi.fn(
      async () =>
        new Response(
          JSON.stringify({
            player: [
              { strPlayer: "Kai Havertz", strCutout: "https://r2.thesportsdb.com/images/media/player/cutout/a.png" },
              { strPlayer: "Other", strCutout: "https://r2.thesportsdb.com/images/media/player/cutout/b.png" },
            ],
          }),
          { status: 200 },
        ),
    );
    const url = await findCutoutUrl("Kai Havertz", fetchMock as any);
    expect(url).toBe("https://r2.thesportsdb.com/images/media/player/cutout/a.png");
    const called = String((fetchMock.mock.calls as any)[0][0]);
    expect(called.startsWith("https://www.thesportsdb.com/api/v1/json/123/searchplayers.php?p=")).toBe(true);
    expect(called).toContain("Kai_Havertz");
  });

  it("returns null for player:null, empty array or empty cutout", async () => {
    const mk = (body: unknown) => vi.fn(async () => new Response(JSON.stringify(body), { status: 200 }));
    expect(await findCutoutUrl("Nobody", mk({ player: null }) as any)).toBeNull();
    expect(await findCutoutUrl("Nobody", mk({ player: [] }) as any)).toBeNull();
    expect(await findCutoutUrl("Nobody", mk({ player: [{ strCutout: null }] }) as any)).toBeNull();
    expect(await findCutoutUrl("Nobody", mk({ player: [{ strCutout: "" }] }) as any)).toBeNull();
  });

  it("returns null for a blank name without fetching", async () => {
    const fetchMock = vi.fn();
    expect(await findCutoutUrl("  ", fetchMock as any)).toBeNull();
    expect(fetchMock).not.toHaveBeenCalled();
  });
});

describe("GET /api/img", () => {
  const realToken = process.env.HELPER_TOKEN;
  beforeEach(() => {
    process.env.HELPER_TOKEN = "secret";
  });
  afterEach(() => {
    vi.unstubAllGlobals();
    process.env.HELPER_TOKEN = realToken;
  });

  const good = "https://a.espncdn.com/i/teamlogos/soccer/500/360.png";
  const call = (query: Record<string, string | undefined>, withToken = true) => {
    const res = stubRes();
    const p = imgHandler({ headers: withToken ? { "x-token": "secret" } : {}, query } as any, res);
    return p.then(() => res);
  };

  it("401 without token", async () => {
    expect((await call({ u: good, w: "48", h: "48" }, false)).statusCode).toBe(401);
  });

  it("400 for a disallowed host and never fetches", async () => {
    const fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);
    const res = await call({ u: "https://evil.com/a.png", w: "48", h: "48" });
    expect(res.statusCode).toBe(400);
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("400 for missing/invalid params and sizes out of range", async () => {
    const fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);
    for (const q of [
      { w: "48", h: "48" },
      { u: good, h: "48" },
      { u: good, w: "48" },
      { u: good, w: "481", h: "48" },
      { u: good, w: "48", h: "481" },
      { u: good, w: "0", h: "48" },
      { u: good, w: "-5", h: "48" },
      { u: good, w: "abc", h: "48" },
      { u: good, w: "4.5", h: "48" },
      { u: good, w: "48", h: "48", fit: "stretch" },
    ]) {
      expect((await call(q)).statusCode, JSON.stringify(q)).toBe(400);
    }
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("200 PNG with cache header (default fit contain)", async () => {
    const src = await makePng(200, 100);
    const fetchMock = vi.fn(async () => new Response(new Uint8Array(src), { status: 200, headers: { "content-type": "image/png" } }));
    vi.stubGlobal("fetch", fetchMock);
    const res = await call({ u: good, w: "48", h: "48" });
    expect(res.statusCode).toBe(200);
    expect(res.headers["Content-Type"]).toBe("image/png");
    expect(res.headers["Cache-Control"]).toBe("public, max-age=86400, immutable");
    const meta = await sharp(res.body).metadata();
    expect([meta.format, meta.width, meta.height]).toEqual(["png", 48, 48]);
    expect(await alphaAt(res.body, 0, 0)).toBe(0);
    const init = (fetchMock.mock.calls as any)[0][1];
    expect(init.redirect).toBe("manual");
  });

  it("200 JPEG for a JPEG source with fit=cover", async () => {
    const src = await makeJpeg(400, 225);
    vi.stubGlobal("fetch", vi.fn(async () => new Response(new Uint8Array(src), { status: 200 })));
    const res = await call({ u: good, w: "240", h: "135", fit: "cover" });
    expect(res.statusCode).toBe(200);
    expect(res.headers["Content-Type"]).toBe("image/jpeg");
    const meta = await sharp(res.body).metadata();
    expect([meta.format, meta.width, meta.height]).toEqual(["jpeg", 240, 135]);
  });

  it("follows an allowed redirect but refuses one to a disallowed host", async () => {
    const src = await makePng(20, 20);
    const ok = vi.fn(async (url: any) =>
      String(url) === good
        ? new Response(null, { status: 302, headers: { location: "https://crests.football-data.org/x.png" } })
        : new Response(new Uint8Array(src), { status: 200 }),
    );
    vi.stubGlobal("fetch", ok);
    expect((await call({ u: good, w: "48", h: "48" })).statusCode).toBe(200);
    expect(ok).toHaveBeenCalledTimes(2);

    const bad = vi.fn(async () => new Response(null, { status: 302, headers: { location: "https://evil.com/a.png" } }));
    vi.stubGlobal("fetch", bad);
    const res = await call({ u: good, w: "48", h: "48" });
    expect(res.statusCode).toBe(502);
    expect(bad).toHaveBeenCalledTimes(1);

    const http = vi.fn(async () => new Response(null, { status: 301, headers: { location: "http://a.espncdn.com/x.png" } }));
    vi.stubGlobal("fetch", http);
    expect((await call({ u: good, w: "48", h: "48" })).statusCode).toBe(502);
  });

  it("gives up after 3 redirects", async () => {
    const loop = vi.fn(async () => new Response(null, { status: 302, headers: { location: good } }));
    vi.stubGlobal("fetch", loop);
    expect((await call({ u: good, w: "48", h: "48" })).statusCode).toBe(502);
    expect(loop.mock.calls.length).toBeLessThanOrEqual(4);
  });

  it("502 for upstream error, SVG, garbage and oversize sources", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response("x", { status: 500 })));
    expect((await call({ u: good, w: "48", h: "48" })).statusCode).toBe(502);

    const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><rect width="10" height="10"/></svg>';
    vi.stubGlobal("fetch", vi.fn(async () => new Response(svg, { status: 200, headers: { "content-type": "image/svg+xml" } })));
    expect((await call({ u: good, w: "48", h: "48" })).statusCode).toBe(502);

    vi.stubGlobal("fetch", vi.fn(async () => new Response("not an image", { status: 200 })));
    expect((await call({ u: good, w: "48", h: "48" })).statusCode).toBe(502);

    const big = Buffer.alloc(5 * 1024 * 1024 + 1);
    vi.stubGlobal("fetch", vi.fn(async () => new Response(new Uint8Array(big), { status: 200 })));
    expect((await call({ u: good, w: "48", h: "48" })).statusCode).toBe(502);
  });
});

describe("GET /api/cutout", () => {
  const realToken = process.env.HELPER_TOKEN;
  beforeEach(() => {
    process.env.HELPER_TOKEN = "secret";
  });
  afterEach(() => {
    vi.unstubAllGlobals();
    process.env.HELPER_TOKEN = realToken;
  });

  const call = (query: Record<string, string | undefined>, withToken = true) => {
    const res = stubRes();
    const p = cutoutHandler({ headers: withToken ? { "x-token": "secret" } : {}, query } as any, res);
    return p.then(() => res);
  };

  it("401 without token", async () => {
    expect((await call({ name: "Kai Havertz", w: "40", h: "40" }, false)).statusCode).toBe(401);
  });

  it("400 for missing name or bad sizes", async () => {
    const fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);
    for (const q of [
      { w: "40", h: "40" },
      { name: "", w: "40", h: "40" },
      { name: "Kai Havertz", h: "40" },
      { name: "Kai Havertz", w: "481", h: "40" },
      { name: "Kai Havertz", w: "40", h: "0" },
    ]) {
      expect((await call(q)).statusCode, JSON.stringify(q)).toBe(400);
    }
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("404 when TheSportsDB has no cutout", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response(JSON.stringify({ player: null }), { status: 200 })));
    expect((await call({ name: "Nobody", w: "40", h: "40" })).statusCode).toBe(404);
  });

  it("404 when the cutout URL is on a disallowed host", async () => {
    const fetchMock = vi.fn(
      async () => new Response(JSON.stringify({ player: [{ strCutout: "https://evil.com/c.png" }] }), { status: 200 }),
    );
    vi.stubGlobal("fetch", fetchMock);
    expect((await call({ name: "Kai Havertz", w: "40", h: "40" })).statusCode).toBe(404);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  it("200 PNG with transparent padding and cache header", async () => {
    const cutout = "https://r2.thesportsdb.com/images/media/player/cutout/a.png";
    const png = await makePng(100, 200);
    const fetchMock = vi.fn(async (url: any) =>
      String(url).includes("searchplayers.php")
        ? new Response(JSON.stringify({ player: [{ strCutout: cutout }] }), { status: 200 })
        : new Response(new Uint8Array(png), { status: 200 }),
    );
    vi.stubGlobal("fetch", fetchMock);
    const res = await call({ name: "Kai Havertz", w: "40", h: "40" });
    expect(res.statusCode).toBe(200);
    expect(res.headers["Content-Type"]).toBe("image/png");
    expect(res.headers["Cache-Control"]).toBe("public, max-age=86400");
    const meta = await sharp(res.body).metadata();
    expect([meta.format, meta.width, meta.height]).toEqual(["png", 40, 40]);
    expect(await alphaAt(res.body, 0, 20)).toBe(0);
    expect(String((fetchMock.mock.calls as any)[1][0])).toBe(cutout);
  });

  it("502 when the lookup fails", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response("x", { status: 500 })));
    expect((await call({ name: "Kai Havertz", w: "40", h: "40" })).statusCode).toBe(502);
  });
});
