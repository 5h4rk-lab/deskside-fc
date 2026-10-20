import type { VercelRequest, VercelResponse } from "@vercel/node";
import { requireToken } from "../src/auth.js";
import { sendError } from "../src/http.js";
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
    sendError(res, 400, "invalid or disallowed url");
    return;
  }
  if (w === null || h === null) {
    sendError(res, 400, "w and h must be integers from 1 to 480");
    return;
  }
  if (fit !== "contain" && fit !== "cover") {
    sendError(res, 400, "invalid fit");
    return;
  }

  try {
    const src = await fetchImageBuffer(u);
    const out = await resizeImage(src, w, h, fit as Fit);
    res.setHeader("Content-Type", out.type);
    res.setHeader("Cache-Control", "public, max-age=86400, s-maxage=86400, immutable");
    res.setHeader("Vary", "x-token");
    res.status(200).send(out.data);
  } catch {
    sendError(res, 502, "upstream error");
  }
}
