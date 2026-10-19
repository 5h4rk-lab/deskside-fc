export interface Point {
  x: number;
  y: number;
}

const Y_TOP = 0.08;
const Y_BOTTOM = 0.92;
const MAX_ROW = 4;

const r3 = (n: number): number => Math.round(n * 1000) / 1000;

function rowOf(n: number, y: number): Point[] {
  const out: Point[] = [];
  for (let i = 0; i < n; i++) out.push({ x: r3((i + 1) / (n + 1)), y: r3(y) });
  return out;
}

function stack(rowSizes: number[]): Point[] {
  // keeper line plus one line per row, evenly from Y_TOP to Y_BOTTOM
  const lines = rowSizes.length;
  const pts: Point[] = [{ x: 0.5, y: Y_TOP }];
  rowSizes.forEach((n, i) => {
    pts.push(...rowOf(n, Y_TOP + ((i + 1) * (Y_BOTTOM - Y_TOP)) / lines));
  });
  return pts;
}

function parse(formation: string): number[] | null {
  if (!/^\d(-\d)+$/.test(formation)) return null;
  const rows = formation.split("-").map(Number);
  return rows.every((n) => n > 0) ? rows : null;
}

// Index 0 is the keeper at the top (own goal end); outfield rows follow in increasing y,
// each listed left to right. Anything unparsable, or a count that does not match the
// formation, falls back to even rows of at most four.
export function layoutFormation(formation: string, count: number): Point[] {
  if (!Number.isFinite(count) || count <= 0) return [];
  const rows = parse(formation);
  if (rows && rows.reduce((a, b) => a + b, 0) === count - 1) return stack(rows);

  const outfield = count - 1;
  const sizes: number[] = [];
  const rowCount = Math.ceil(outfield / MAX_ROW);
  for (let i = 0; i < rowCount; i++) {
    sizes.push(Math.floor(outfield / rowCount) + (i < outfield % rowCount ? 1 : 0));
  }
  return count === 1 ? [{ x: 0.5, y: Y_TOP }] : stack(sizes);
}
