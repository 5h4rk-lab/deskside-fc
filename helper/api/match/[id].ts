import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../../src/auth.js";
import { sendError } from "../../src/http.js";
import { fetchSummary } from "../../src/espn.js";

function first(v: string | string[] | undefined): string | undefined {
  return Array.isArray(v) ? v[0] : v;
}

export default async function handler(req: VercelRequest, res: VercelResponse): Promise<void> {
  if (!requireToken(req, res)) return;

  const id = first(req.query.id);
  const league = first(req.query.league) ?? "eng.1";

  if (!id || !/^\d{1,20}$/.test(id)) {
    sendError(res, 400, "invalid id");
    return;
  }
  if (!/^[a-z0-9]+(\.[a-z0-9]+)*$/i.test(league)) {
    sendError(res, 400, "invalid league");
    return;
  }

  try {
    const detail = await fetchSummary(league, id);
    res.setHeader("Cache-Control", "s-maxage=15");
    res.setHeader("Vary", "x-token");
    res.status(200).json(detail);
  } catch {
    sendError(res, 502, "upstream error");
  }
}
