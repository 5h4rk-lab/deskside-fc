import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../../src/auth.js";
import { firmwareAnswer, publicHost } from "../../src/firmware.js";
import manifest from "../../firmware-manifest.json" with { type: "json" };

/** GET /api/firmware/version: the newest published firmware (see src/firmware.ts). */
export default function handler(req: VercelRequest, res: VercelResponse): void {
  if (!requireToken(req, res)) return;
  const answer = firmwareAnswer(manifest, publicHost(req.headers));
  // Never cached: a new release must show at once, and the answer depends on the token.
  res.setHeader("Cache-Control", "no-store");
  res.status(answer.status).json(answer.body);
}
