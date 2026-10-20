import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../src/auth.js";
import { sendError } from "../src/http.js";
import { fetchTeams } from "../src/espn.js";

export default async function handler(req: VercelRequest, res: VercelResponse): Promise<void> {
  if (!requireToken(req, res)) return;

  try {
    const teams = await fetchTeams();
    res.setHeader("Cache-Control", "s-maxage=86400");
    res.setHeader("Vary", "x-token");
    res.status(200).json({ teams });
  } catch {
    sendError(res, 502, "upstream error");
  }
}
