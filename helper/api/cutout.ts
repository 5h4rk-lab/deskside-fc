import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../src/auth.js";
import { fetchImageBuffer, isAllowedImageUrl, parseDimension, resizeImage } from "../src/images.js";
import { findCutoutUrl } from "../src/sportsdb.js";

function first(v: string | string[] | undefined): string | undefined {
  return Array.isArray(v) ? v[0] : v;
}

export default async function handler(req: VercelRequest, res: VercelResponse): Promise<void> {
  if (!requireToken(req, res)) return;

  const name = first(req.query.name)?.trim();
  const w = parseDimension(first(req.query.w));
  const h = parseDimension(first(req.query.h));
  if (!name || name.length > 100) {
    res.status(400).json({ error: "invalid name" });
    return;
  }
  if (w === null || h === null) {
    res.status(400).json({ error: "w and h must be integers from 1 to 480" });
    return;
  }

  try {
    const url = await findCutoutUrl(name);
    if (!url || !isAllowedImageUrl(url)) {
      res.status(404).json({ error: "no cutout" });
      return;
    }
    const src = await fetchImageBuffer(url);
    const out = await resizeImage(src, w, h, "contain");
    res.setHeader("Content-Type", "image/png");
    res.setHeader("Cache-Control", "public, max-age=86400");
    res.status(200).send(out.data);
  } catch {
    res.status(502).json({ error: "upstream error" });
  }
}
