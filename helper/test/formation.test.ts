import { describe, expect, it } from "vitest";
import { layoutFormation } from "../src/formation.js";

function rows(points: { x: number; y: number }[]): { x: number; y: number }[][] {
  const out: { x: number; y: number }[][] = [];
  for (const p of points) {
    const last = out[out.length - 1];
    if (last && Math.abs(last[0].y - p.y) < 1e-9) last.push(p);
    else out.push([p]);
  }
  return out;
}

describe("layoutFormation", () => {
  it("lays out 4-3-3 as keeper, 4, 3, 3 on increasing y", () => {
    const pts = layoutFormation("4-3-3", 11);
    expect(pts).toHaveLength(11);
    expect(pts[0].x).toBeCloseTo(0.5, 5);
    expect(pts[0].y).toBeLessThan(0.15);
    const r = rows(pts);
    expect(r.map((row) => row.length)).toEqual([1, 4, 3, 3]);
    for (let i = 1; i < r.length; i++) expect(r[i][0].y).toBeGreaterThan(r[i - 1][0].y);
    for (const row of r) {
      const xs = row.map((p) => p.x);
      expect(xs).toEqual([...xs].sort((a, b) => a - b));
    }
  });

  it("lays out 3-1-4-2 as rows 3, 1, 4, 2", () => {
    const r = rows(layoutFormation("3-1-4-2", 11));
    expect(r.map((row) => row.length)).toEqual([1, 3, 1, 4, 2]);
  });

  it("keeps every point inside y 0.08..0.92 and x 0..1", () => {
    const pts = layoutFormation("4-2-3-1", 11);
    expect(pts[0].y).toBeCloseTo(0.08, 5);
    expect(pts[pts.length - 1].y).toBeCloseTo(0.92, 5);
    for (const p of pts) {
      expect(p.y).toBeGreaterThanOrEqual(0.08 - 1e-9);
      expect(p.y).toBeLessThanOrEqual(0.92 + 1e-9);
      expect(p.x).toBeGreaterThan(0);
      expect(p.x).toBeLessThan(1);
    }
  });

  it("falls back to an even spread for unparsable formations without throwing", () => {
    for (const f of ["", "garbage", "4-4-", "0-0-0", "4--3"]) {
      const pts = layoutFormation(f, 11);
      expect(pts).toHaveLength(11);
      expect(pts[0].x).toBeCloseTo(0.5, 5);
      expect(pts[0].y).toBeLessThan(0.15);
      for (const p of pts) {
        expect(Number.isFinite(p.x) && Number.isFinite(p.y)).toBe(true);
        expect(p.y).toBeGreaterThanOrEqual(0.08 - 1e-9);
        expect(p.y).toBeLessThanOrEqual(0.92 + 1e-9);
      }
    }
  });

  it("falls back when the count does not match the formation", () => {
    expect(layoutFormation("4-3-3", 9)).toHaveLength(9);
    expect(layoutFormation("4-3-3", 14)).toHaveLength(14);
    expect(layoutFormation("4-3-3", 0)).toEqual([]);
    expect(layoutFormation("4-3-3", 1)).toHaveLength(1);
  });
});
