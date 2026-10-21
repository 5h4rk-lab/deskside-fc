import { describe, expect, it } from "vitest";
import { TEAM_COLORS, TEAM_PRIMARIES, teamColor, teamPrimary } from "../src/colors.js";

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

describe("teamColor with hostile abbreviations", () => {
  it("never reads inherited Object.prototype properties", () => {
    for (const abbr of ["constructor", "__proto__", "toString", "hasOwnProperty", "valueOf"]) {
      expect(teamColor(abbr), abbr).toBe("#7C8F84");
      expect(teamColor(abbr, "12ab9f"), abbr).toBe("#12AB9F");
      expect(teamColor(abbr, undefined, "eng.1"), abbr).toBe("#7C8F84");
    }
    // a league-qualified key that only exists on the prototype chain is not found either
    expect(teamColor("x", undefined, "__proto__")).toBe("#7C8F84");
  });
});

describe("teamPrimary", () => {
  it("returns the real brand colour, even when it is dark", () => {
    expect(teamPrimary("CHE")).toBe("#034694");
    expect(teamPrimary("ARS")).toBe("#EF0107");
    expect(teamPrimary("TOT")).toBe("#132257");
    expect(teamPrimary("MNC")).toBe("#6CABDD");
    expect(teamPrimary("LIV")).toBe("#C8102E");
    expect(teamPrimary("MAN")).toBe("#DA291C");
    // the accent stays the readable one
    expect(teamColor("CHE")).toBe("#DBA111");
  });

  it("curated table wins over the ESPN hex, then the ESPN hex, then the neutral colour", () => {
    expect(teamPrimary("CHE", "ff0000")).toBe("#034694");
    expect(teamPrimary("ZZZ", "12ab9f")).toBe("#12AB9F");
    expect(teamPrimary("ZZZ", "#12ab9f")).toBe("#12AB9F");
    expect(teamPrimary("ZZZ")).toBe("#7C8F84");
    expect(teamPrimary("ZZZ", "nothex")).toBe("#7C8F84");
  });

  it("resolves league-qualified collision keys", () => {
    expect(teamPrimary("BRE", undefined, "eng.1")).toBe(TEAM_PRIMARIES["eng.1:BRE"]);
    expect(teamPrimary("BRE", undefined, "fra.1")).toBe(TEAM_PRIMARIES["fra.1:BRE"]);
    expect(teamPrimary("MON", undefined, "ita.1")).toBe(TEAM_PRIMARIES["ita.1:MON"]);
    expect(teamPrimary("MON", undefined, "fra.1")).toBe(TEAM_PRIMARIES["fra.1:MON"]);
    expect(teamPrimary("PAR", undefined, "ita.1")).toBe(TEAM_PRIMARIES["ita.1:PAR"]);
    expect(teamPrimary("PAR", undefined, "fra.1")).toBe(TEAM_PRIMARIES["fra.1:PAR"]);
    expect(teamPrimary("PAR", undefined, "ita.1")).not.toBe(teamPrimary("PAR", undefined, "fra.1"));
    expect(teamPrimary("BRE", "123456")).toBe("#123456");
  });

  it("never reads inherited Object.prototype properties", () => {
    for (const abbr of ["constructor", "__proto__", "toString", "hasOwnProperty", "valueOf"]) {
      expect(teamPrimary(abbr), abbr).toBe("#7C8F84");
      expect(teamPrimary(abbr, "12ab9f"), abbr).toBe("#12AB9F");
      expect(teamPrimary(abbr, undefined, "eng.1"), abbr).toBe("#7C8F84");
    }
    expect(teamPrimary("x", undefined, "__proto__")).toBe("#7C8F84");
  });
});

describe("TEAM_PRIMARIES table", () => {
  it("has exactly the same keys as TEAM_COLORS", () => {
    expect(Object.keys(TEAM_PRIMARIES).sort()).toEqual(Object.keys(TEAM_COLORS).sort());
  });

  it("has only valid #RRGGBB values and never pure white (white clubs use their dark identity colour)", () => {
    for (const [key, hex] of Object.entries(TEAM_PRIMARIES)) {
      expect(hex, key).toMatch(/^#[0-9A-F]{6}$/);
      expect(hex, `${key} must not be white`).not.toBe("#FFFFFF");
    }
  });
});
