import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../src/auth.js";
import { sendError } from "../src/http.js";
import { DateRangeError, expandDates, fetchMatchesDetailed } from "../src/espn.js";

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

export const MAX_LEAGUES = 8;

export default async function handler(req: VercelRequest, res: VercelResponse): Promise<void> {
  if (!requireToken(req, res)) return;

  const requested = (first(req.query.leagues) ?? "eng.1")
    .split(",")
    .map((s) => s.trim().toLowerCase())
    .filter(Boolean);
  const from = first(req.query.from) ?? offsetDay(-1);
  const to = first(req.query.to) ?? offsetDay(7);

  if (requested.length === 0 || !requested.every((l) => /^[a-z0-9.\-_]+$/i.test(l) && !l.includes(".."))) {
    sendError(res, 400, "invalid leagues");
    return;
  }
  const leagues = [...new Set(requested)]; // lower-cased, de-duplicated, first occurrence wins the order
  if (leagues.length > MAX_LEAGUES) {
    sendError(res, 400, "too many leagues");
    return;
  }
  try {
    expandDates(from, to);
  } catch (err) {
    if (err instanceof DateRangeError) {
      sendError(res, 400, err.message);
      return;
    }
    throw err;
  }

  try {
    const { matches, failed, total } = await fetchMatchesDetailed(leagues, from, to);
    if (total > 0 && failed === total) {
      sendError(res, 502, "upstream error");
      return;
    }
    // A partial answer is cached only briefly so a recovered upstream shows up quickly.
    // No stale-while-revalidate: it would stack with the CDN and in-process TTLs and age live scores.
    res.setHeader("Cache-Control", `s-maxage=${failed > 0 ? 5 : 20}`);
    res.setHeader("Vary", "x-token");
    if (failed > 0) res.setHeader("x-helper-partial", "1");
    res.status(200).json({ updated: Math.floor(Date.now() / 1000), matches });
  } catch {
    sendError(res, 502, "upstream error");
  }
}
