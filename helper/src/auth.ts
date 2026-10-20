import { timingSafeEqual } from "node:crypto";
import type { VercelRequest, VercelResponse } from "@vercel/node";

export function checkToken(header: string | undefined, expected: string): boolean {
  if (!header || !expected) return false;
  const a = Buffer.from(header);
  const b = Buffer.from(expected);
  if (a.length !== b.length) return false;
  return timingSafeEqual(a, b);
}

let warnedMissingToken = false;

export function requireToken(req: VercelRequest, res: VercelResponse): boolean {
  const raw = req.headers["x-token"];
  const header = Array.isArray(raw) ? raw[0] : raw;
  const expected = process.env.HELPER_TOKEN ?? "";
  if (!expected && !warnedMissingToken) {
    warnedMissingToken = true;
    console.error("HELPER_TOKEN is not set");
  }
  if (!checkToken(header, expected)) {
    res.setHeader("Cache-Control", "no-store");
    res.status(401).json({ error: "unauthorized" });
    return false;
  }
  return true;
}
