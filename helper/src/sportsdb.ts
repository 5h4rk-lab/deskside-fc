import { FETCH_TIMEOUT_MS } from "./images.js";

const SEARCH_URL = "https://www.thesportsdb.com/api/v1/json/123/searchplayers.php";

/** Looks up a player on TheSportsDB (free key 123) and returns the first result's cutout URL. */
export async function findCutoutUrl(name: string, fetchImpl: typeof fetch = fetch): Promise<string | null> {
  const trimmed = (name ?? "").trim();
  if (!trimmed) return null;
  const q = encodeURIComponent(trimmed.replace(/\s+/g, "_"));
  const resp = await fetchImpl(`${SEARCH_URL}?p=${q}`, { signal: AbortSignal.timeout(FETCH_TIMEOUT_MS) });
  if (!resp.ok) throw new Error(`thesportsdb ${resp.status}`);
  const body: any = await resp.json();
  const first = Array.isArray(body?.player) ? body.player[0] : null;
  const cutout = first?.strCutout;
  return typeof cutout === "string" && cutout !== "" ? cutout : null;
}
