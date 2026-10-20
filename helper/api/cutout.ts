import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../src/auth.js";
import { CUTOUT_DEADLINE_MS, sendError } from "../src/http.js";
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
    sendError(res, 400, "invalid name");
    return;
  }
  if (w === null || h === null) {
    sendError(res, 400, "w and h must be integers from 1 to 480");
    return;
  }

  // One deadline for the lookup and the download: the device gives up after 10 s.
  const signal = AbortSignal.timeout(CUTOUT_DEADLINE_MS);
  try {
    const url = await findCutoutUrl(name, fetch, signal);
    if (!url || !isAllowedImageUrl(url)) {
      sendError(res, 404, "no cutout");
      return;
    }
    const src = await fetchImageBuffer(url, fetch, signal);
    const out = await resizeImage(src, w, h, "contain");
    res.setHeader("Content-Type", "image/png");
    res.setHeader("Cache-Control", "public, max-age=86400, s-maxage=86400");
    res.setHeader("Vary", "x-token");
    res.status(200).send(out.data);
  } catch {
    sendError(res, 502, "upstream error");
  }
}
