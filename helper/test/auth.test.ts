import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { checkToken } from "../src/auth.js";
import { stubRes } from "./helpers.js";

describe("checkToken", () => {
  it("accepts a matching token", () => {
    expect(checkToken("abc", "abc")).toBe(true);
  });

  it("rejects a wrong token", () => {
    expect(checkToken("abd", "abc")).toBe(false);
    expect(checkToken("abcd", "abc")).toBe(false);
  });

  it("rejects an undefined header", () => {
    expect(checkToken(undefined, "abc")).toBe(false);
  });

  it("rejects when expected is empty", () => {
    expect(checkToken("", "")).toBe(false);
    expect(checkToken("abc", "")).toBe(false);
  });

  it("rejects an empty header against a real token", () => {
    expect(checkToken("", "abc")).toBe(false);
  });
});

describe("requireToken", () => {
  const realToken = process.env.HELPER_TOKEN;
  beforeEach(() => {
    vi.resetModules(); // fresh module state: the "not set" warning is logged once per process
  });
  afterEach(() => {
    vi.restoreAllMocks();
    if (realToken === undefined) delete process.env.HELPER_TOKEN;
    else process.env.HELPER_TOKEN = realToken;
  });

  it("answers 401 with no-store and logs once when HELPER_TOKEN is not set", async () => {
    delete process.env.HELPER_TOKEN;
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    const { requireToken } = await import("../src/auth.js");
    for (let i = 0; i < 3; i++) {
      const res = stubRes();
      expect(requireToken({ headers: { "x-token": "anything" } } as any, res)).toBe(false);
      expect(res.statusCode).toBe(401);
      expect(res.headers["Cache-Control"]).toBe("no-store");
    }
    expect(errors).toHaveBeenCalledTimes(1);
    expect(errors).toHaveBeenCalledWith("HELPER_TOKEN is not set");
  });

  it("treats an empty HELPER_TOKEN as not set", async () => {
    process.env.HELPER_TOKEN = "";
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    const { requireToken } = await import("../src/auth.js");
    expect(requireToken({ headers: { "x-token": "" } } as any, stubRes())).toBe(false);
    expect(errors).toHaveBeenCalledTimes(1);
  });

  it("does not log when the variable is set, and never logs its value", async () => {
    process.env.HELPER_TOKEN = "s3cret-value";
    const errors = vi.spyOn(console, "error").mockImplementation(() => {});
    const { requireToken } = await import("../src/auth.js");
    expect(requireToken({ headers: { "x-token": "s3cret-value" } } as any, stubRes())).toBe(true);
    expect(requireToken({ headers: { "x-token": "wrong" } } as any, stubRes())).toBe(false);
    expect(errors).not.toHaveBeenCalled();
  });

  it("uses the first value of an array-valued header", async () => {
    process.env.HELPER_TOKEN = "secret";
    const { requireToken } = await import("../src/auth.js");
    expect(requireToken({ headers: { "x-token": ["secret", "other"] } } as any, stubRes())).toBe(true);
    const res = stubRes();
    expect(requireToken({ headers: { "x-token": ["other", "secret"] } } as any, res)).toBe(false);
    expect(res.statusCode).toBe(401);
    expect(requireToken({ headers: { "x-token": [] } } as any, stubRes())).toBe(false);
  });
});
