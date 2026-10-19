import { describe, expect, it } from "vitest";
import { TEAM_COLORS, teamColor } from "../src/colors.js";

function luminance(hex: string): number {
  const ch = [1, 3, 5].map((i) => {
    const c = parseInt(hex.slice(i, i + 2), 16) / 255;
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2];
}

describe("teamColor", () => {
  it("curated table wins over the ESPN hex", () => {
    expect(teamColor("ARS", "ff0000")).toBe("#EF0107");
  });

  it("falls back to the ESPN hex, uppercased with a leading #", () => {
    expect(teamColor("ZZZ", "12ab9f")).toBe("#12AB9F");
    expect(teamColor("ZZZ", "#12ab9f")).toBe("#12AB9F");
  });

  it("falls back to the neutral colour with no usable hex", () => {
    expect(teamColor("ZZZ")).toBe("#7C8F84");
    expect(teamColor("ZZZ", "nothex")).toBe("#7C8F84");
    expect(teamColor("ZZZ", "")).toBe("#7C8F84");
  });

  it("disambiguates abbreviations shared across leagues", () => {
    expect(teamColor("BRE", undefined, "eng.1")).not.toBe(teamColor("BRE", undefined, "fra.1"));
    expect(teamColor("MON", undefined, "ita.1")).not.toBe(teamColor("MON", undefined, "fra.1"));
    expect(teamColor("PAR", undefined, "ita.1")).not.toBe(teamColor("PAR", undefined, "fra.1"));
    // without a league the shared abbreviation is ambiguous, so the ESPN hex is used
    expect(teamColor("BRE", "123456")).toBe("#123456");
  });
});

describe("TEAM_COLORS table", () => {
  it("covers all 96 clubs of the five leagues", () => {
    expect(Object.keys(TEAM_COLORS)).toHaveLength(96);
  });

  it("has only valid #RRGGBB values readable on the #0A1410 background", () => {
    for (const [key, hex] of Object.entries(TEAM_COLORS)) {
      expect(hex, key).toMatch(/^#[0-9A-F]{6}$/);
      expect(luminance(hex), `${key} ${hex} too dark`).toBeGreaterThanOrEqual(0.08);
    }
  });
});
