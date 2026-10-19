import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../src/auth.js";
import { fetchTeams } from "../src/espn.js";

export default async function handler(req: VercelRequest, res: VercelResponse): Promise<void> {
  if (!requireToken(req, res)) return;

  try {
    const teams = await fetchTeams();
    res.setHeader("Cache-Control", "s-maxage=86400");
    res.status(200).json({ teams });
  } catch {
    res.status(502).json({ error: "upstream error" });
  }
}
