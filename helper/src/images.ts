import sharp from "sharp";

export const ALLOWED_IMAGE_HOSTS = [
  "a.espncdn.com",
  "r2.thesportsdb.com",
  "crests.football-data.org",
  "espnmedia-cdn.akamaized.net",
] as const;

export const MAX_DIMENSION = 480;
export const MAX_SOURCE_BYTES = 5 * 1024 * 1024;
export const FETCH_TIMEOUT_MS = 8000;
export const MAX_REDIRECTS = 3;
/** Decoded-pixel ceiling (4096 x 4096): a small PNG can claim a huge canvas. */
export const MAX_INPUT_PIXELS = 4096 * 4096;

export type Fit = "contain" | "cover";
export type ResizedImage = { data: Buffer; type: "image/png" | "image/jpeg" };

/** SSRF guard: https only, exact allow-listed hostname, default port, no userinfo. */
export function isAllowedImageUrl(u: string): boolean {
  let url: URL;
  try {
    url = new URL(u);
  } catch {
    return false;
  }
  if (url.protocol !== "https:") return false;
  if (url.username !== "" || url.password !== "") return false;
  if (url.port !== "") return false;
  return (ALLOWED_IMAGE_HOSTS as readonly string[]).includes(url.hostname);
}

/** Parses a pixel dimension: a plain integer within 1..MAX_DIMENSION, else null. */
export function parseDimension(v: string | undefined): number | null {
  if (v === undefined || !/^\d{1,4}$/.test(v)) return null;
  const n = Number(v);
  return n >= 1 && n <= MAX_DIMENSION ? n : null;
}

export class ImageError extends Error {}

async function readCapped(resp: Response): Promise<Buffer> {
  const declared = Number(resp.headers.get("content-length"));
  if (Number.isFinite(declared) && declared > MAX_SOURCE_BYTES) {
    await resp.body?.cancel();
    throw new ImageError("source too large");
  }
  if (!resp.body) return Buffer.alloc(0);
  const reader = resp.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > MAX_SOURCE_BYTES) {
      await reader.cancel();
      throw new ImageError("source too large");
    }
    chunks.push(value);
  }
  return Buffer.concat(chunks);
}

/**
 * Fetches an allow-listed image. Redirects are followed manually (max MAX_REDIRECTS)
 * and every hop is re-validated. Enforces a timeout and a source size cap.
 */
export async function fetchImageBuffer(
  url: string,
  fetchImpl: typeof fetch = fetch,
  signal: AbortSignal = AbortSignal.timeout(FETCH_TIMEOUT_MS),
): Promise<Buffer> {
  let current = url;
  for (let hop = 0; hop <= MAX_REDIRECTS; hop++) {
    if (!isAllowedImageUrl(current)) throw new ImageError("host not allowed");
    const resp = await fetchImpl(current, { redirect: "manual", signal });
    if (resp.status >= 300 && resp.status < 400) {
      const loc = resp.headers.get("location");
      await resp.body?.cancel();
      if (!loc) throw new ImageError("redirect without location");
      try {
        current = new URL(loc, current).toString();
      } catch {
        throw new ImageError("bad redirect");
      }
      continue;
    }
    if (!resp.ok) {
      await resp.body?.cancel();
      throw new ImageError(`upstream status ${resp.status}`);
    }
    return readCapped(resp);
  }
  throw new ImageError("too many redirects");
}

const RASTER_FORMATS = new Set(["png", "jpeg", "webp", "gif"]);

/**
 * Resizes to exactly w x h. contain: letterboxed with transparent padding (PNG).
 * cover: fills the box; JPEG when the source is JPEG, otherwise PNG.
 */
export async function resizeImage(buf: Buffer, w: number, h: number, fit: Fit): Promise<ResizedImage> {
  const meta = await sharp(buf, { limitInputPixels: MAX_INPUT_PIXELS }).metadata();
  if (!meta.format || !RASTER_FORMATS.has(meta.format)) {
    throw new ImageError(`unsupported image format: ${meta.format ?? "unknown"}`);
  }
  const pipeline = sharp(buf, { limitInputPixels: MAX_INPUT_PIXELS }).rotate().resize(w, h, {
    fit,
    position: "centre",
    background: { r: 0, g: 0, b: 0, alpha: 0 },
  });
  if (fit === "cover" && meta.format === "jpeg") {
    return { data: await pipeline.jpeg({ quality: 85 }).toBuffer(), type: "image/jpeg" };
  }
  return { data: await pipeline.ensureAlpha().png().toBuffer(), type: "image/png" };
}
