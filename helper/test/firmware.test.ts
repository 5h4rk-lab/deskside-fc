import { createHash } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { firmwareAnswer, FIRMWARE_MAX_BYTES, FIRMWARE_MIN_BYTES, publicHost } from "../src/firmware.js";
import { stubRes } from "./helpers.js";

const helperDir = join(dirname(fileURLToPath(import.meta.url)), "..");
const HASH = "0123456789abcdef".repeat(4);
const good = { version: "0.1.1", file: "firmware/0.1.1.bin", sha256: HASH, size: 1634512, notes: "Faster pictures" };

describe("firmwareAnswer", () => {
  it("serves a published manifest with an https URL on the request's host", () => {
    const a = firmwareAnswer(good, "soccer-display-helper.vercel.app");
    expect(a.status).toBe(200);
    expect(a.body).toEqual({
      version: "0.1.1",
      url: "https://soccer-display-helper.vercel.app/firmware/0.1.1.bin",
      sha256: HASH,
      size: 1634512,
      notes: "Faster pictures",
    });
  });

  it("answers 404 while nothing is published", () => {
    expect(firmwareAnswer({ version: null }, "h.example")).toEqual({
      status: 404,
      body: { error: "no firmware published" },
    });
    expect(firmwareAnswer({}, "h.example").status).toBe(404);
  });

  it("notes are optional", () => {
    const { notes: _n, ...rest } = good;
    const a = firmwareAnswer(rest, "h.example");
    expect(a.status).toBe(200);
    expect((a.body as { notes: string }).notes).toBe("");
  });

  it("never serves a manifest that fails a check", () => {
    const bad: unknown[] = [
      null,
      [],
      "0.1.1",
      { ...good, version: "1.2" },
      { ...good, version: "01.2.3" },
      { ...good, version: "v0.1.1" },
      { ...good, version: 11 },
      { ...good, file: "firmware/0.1.2.bin" },
      { ...good, file: "../secrets/0.1.1.bin" },
      { ...good, file: undefined },
      { ...good, sha256: HASH.toUpperCase() },
      { ...good, sha256: HASH.slice(1) },
      { ...good, sha256: undefined },
      { ...good, size: FIRMWARE_MIN_BYTES - 1 },
      { ...good, size: FIRMWARE_MAX_BYTES + 1 },
      { ...good, size: 1634512.5 },
      { ...good, size: "1634512" },
      { ...good, notes: 5 },
      { ...good, notes: "x".repeat(201) },
    ];
    for (const m of bad) expect(firmwareAnswer(m, "h.example").status, JSON.stringify(m)).toBe(500);
    expect(firmwareAnswer(good, null).status).toBe(500);
  });
});

describe("publicHost", () => {
  it("takes x-forwarded-host first, then host, in lower case", () => {
    expect(publicHost({ "x-forwarded-host": "Soccer-Display-Helper.vercel.app", host: "internal" })).toBe(
      "soccer-display-helper.vercel.app",
    );
    expect(publicHost({ host: "localhost:3000" })).toBe("localhost:3000");
    expect(publicHost({ "x-forwarded-host": ["a.example, b.example"] })).toBe("a.example");
  });
  it("refuses anything that is not a plain host", () => {
    expect(publicHost({})).toBeNull();
    expect(publicHost({ host: "evil.example/path" })).toBeNull();
    expect(publicHost({ host: "user@evil.example" })).toBeNull();
    expect(publicHost({ host: "a b" })).toBeNull();
  });
});

describe("the committed manifest source", () => {
  const source = JSON.parse(readFileSync(join(helperDir, "firmware-manifest.json"), "utf8"));
  it("is valid (or says nothing is published)", () => {
    expect([200, 404]).toContain(firmwareAnswer(source, "h.example").status);
  });
  it("names a binary that exists with exactly that size and SHA-256", () => {
    if (source.version === null) return;
    const path = join(helperDir, "public", source.file);
    expect(existsSync(path)).toBe(true);
    const bin = readFileSync(path);
    expect(bin.length).toBe(source.size);
    expect(createHash("sha256").update(bin).digest("hex")).toBe(source.sha256);
  });
});

describe("GET /api/firmware/version", () => {
  const realToken = process.env.HELPER_TOKEN;
  beforeEach(() => {
    process.env.HELPER_TOKEN = "secret";
    vi.resetModules();
  });
  afterEach(() => {
    vi.doUnmock("../firmware-manifest.json");
    if (realToken === undefined) delete process.env.HELPER_TOKEN;
    else process.env.HELPER_TOKEN = realToken;
  });

  it("needs the token", async () => {
    const { default: handler } = await import("../api/firmware/version.js");
    for (const headers of [{}, { "x-token": "wrong" }]) {
      const res = stubRes();
      handler({ headers: { ...headers, host: "h.example" } } as any, res);
      expect(res.statusCode).toBe(401);
      expect(res.headers["Cache-Control"]).toBe("no-store");
      expect(res.body).toEqual({ error: "unauthorized" });
    }
  });

  it("serves the manifest, never cached", async () => {
    vi.doMock("../firmware-manifest.json", () => ({ default: good }));
    const { default: handler } = await import("../api/firmware/version.js");
    const res = stubRes();
    handler({ headers: { "x-token": "secret", "x-forwarded-host": "soccer-display-helper.vercel.app" } } as any, res);
    expect(res.statusCode).toBe(200);
    expect(res.headers["Cache-Control"]).toBe("no-store");
    expect(res.body.url).toBe("https://soccer-display-helper.vercel.app/firmware/0.1.1.bin");
    expect(res.body.sha256).toBe(HASH);
  });

  it("answers 404 while nothing is published", async () => {
    vi.doMock("../firmware-manifest.json", () => ({ default: { version: null } }));
    const { default: handler } = await import("../api/firmware/version.js");
    const res = stubRes();
    handler({ headers: { "x-token": "secret", host: "h.example" } } as any, res);
    expect(res.statusCode).toBe(404);
    expect(res.headers["Cache-Control"]).toBe("no-store");
  });
});
