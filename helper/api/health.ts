import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../src/auth";

export default function handler(req: VercelRequest, res: VercelResponse): void {
  if (!requireToken(req, res)) return;
  res.status(200).json({ ok: true });
}
