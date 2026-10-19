import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../src/auth.js";
import { fetchMatches } from "../src/espn.js";

function first(v: string | string[] | undefined): string | undefined {
  return Array.isArray(v) ? v[0] : v;
}

function ymd(d: Date): string {
  return d.toISOString().slice(0, 10).replace(/-/g, "");
}

function offsetDay(days: number): string {
  const d = new Date();
  d.setUTCDate(d.getUTCDate() + days);
  return ymd(d);
}

export default async function handler(req: VercelRequest, res: VercelResponse): Promise<void> {
  if (!requireToken(req, res)) return;

  const leagues = (first(req.query.leagues) ?? "eng.1")
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean);
  const from = first(req.query.from) ?? offsetDay(-1);
  const to = first(req.query.to) ?? offsetDay(3);

  if (leagues.length === 0 || !leagues.every((l) => /^[a-z0-9.\-_]+$/i.test(l) && !l.includes(".."))) {
    res.status(400).json({ error: "invalid leagues" });
    return;
  }
  if (!/^\d{8}$/.test(from) || !/^\d{8}$/.test(to)) {
    res.status(400).json({ error: "from/to must be YYYYMMDD" });
    return;
  }

  try {
    const matches = await fetchMatches(leagues, from, to);
    res.setHeader("Cache-Control", "s-maxage=20, stale-while-revalidate=60");
    res.status(200).json({ updated: Math.floor(Date.now() / 1000), matches });
  } catch {
    res.status(502).json({ error: "upstream error" });
  }
}
