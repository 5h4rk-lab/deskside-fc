import { describe, expect, it } from "vitest";
import { checkToken } from "../src/auth";

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
  });
});
