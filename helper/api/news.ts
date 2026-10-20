import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../src/auth.js";
import { sendError } from "../src/http.js";
import { fetchNews } from "../src/espn.js";

function first(v: string | string[] | undefined): string | undefined {
  return Array.isArray(v) ? v[0] : v;
}

export default async function handler(req: VercelRequest, res: VercelResponse): Promise<void> {
  if (!requireToken(req, res)) return;

  const league = first(req.query.league) ?? "eng.1";
  if (!/^[a-z0-9]+(\.[a-z0-9]+)*$/i.test(league)) {
    sendError(res, 400, "invalid league");
    return;
  }

  try {
    const articles = await fetchNews(league);
    res.setHeader("Cache-Control", "s-maxage=300");
    res.setHeader("Vary", "x-token");
    res.status(200).json({ updated: Math.floor(Date.now() / 1000), articles });
  } catch {
    sendError(res, 502, "upstream error");
  }
}
