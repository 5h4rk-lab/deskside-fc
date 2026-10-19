import { timingSafeEqual } from "node:crypto";
import type { VercelRequest, VercelResponse } from "@vercel/node";

export function checkToken(header: string | undefined, expected: string): boolean {
  if (!header || !expected) return false;
  const a = Buffer.from(header);
  const b = Buffer.from(expected);
  if (a.length !== b.length) return false;
  return timingSafeEqual(a, b);
}

export function requireToken(req: VercelRequest, res: VercelResponse): boolean {
  const raw = req.headers["x-token"];
  const header = Array.isArray(raw) ? raw[0] : raw;
  if (!checkToken(header, process.env.HELPER_TOKEN ?? "")) {
    res.status(401).json({ error: "unauthorized" });
    return false;
  }
  return true;
}
