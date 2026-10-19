import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../src/auth.js";
import { fetchImageBuffer, isAllowedImageUrl, parseDimension, resizeImage, type Fit } from "../src/images.js";

function first(v: string | string[] | undefined): string | undefined {
  return Array.isArray(v) ? v[0] : v;
}

export default async function handler(req: VercelRequest, res: VercelResponse): Promise<void> {
  if (!requireToken(req, res)) return;

  const u = first(req.query.u);
  const w = parseDimension(first(req.query.w));
  const h = parseDimension(first(req.query.h));
  const fit = first(req.query.fit) ?? "contain";
  if (!u || !isAllowedImageUrl(u)) {
    res.status(400).json({ error: "invalid or disallowed url" });
    return;
  }
  if (w === null || h === null) {
    res.status(400).json({ error: "w and h must be integers from 1 to 480" });
    return;
  }
  if (fit !== "contain" && fit !== "cover") {
    res.status(400).json({ error: "invalid fit" });
    return;
  }

  try {
    const src = await fetchImageBuffer(u);
    const out = await resizeImage(src, w, h, fit as Fit);
    res.setHeader("Content-Type", out.type);
    res.setHeader("Cache-Control", "public, max-age=86400, immutable");
    res.status(200).send(out.data);
  } catch {
    res.status(502).json({ error: "upstream error" });
  }
}
