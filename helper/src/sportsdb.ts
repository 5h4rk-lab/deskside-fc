import { fetchJson, UPSTREAM_TIMEOUT_MS } from "./http.js";

const SEARCH_URL = "https://www.thesportsdb.com/api/v1/json/123/searchplayers.php";

/**
 * Looks up a player on TheSportsDB (free key 123) and returns the first result's cutout URL.
 * The request is aborted by `signal` (default: 5 s); /api/cutout passes its shared deadline.
 */
export async function findCutoutUrl(
  name: string,
  fetchImpl: typeof fetch = fetch,
  signal: AbortSignal = AbortSignal.timeout(UPSTREAM_TIMEOUT_MS),
): Promise<string | null> {
  const trimmed = (name ?? "").trim();
  if (!trimmed) return null;
  const q = encodeURIComponent(trimmed.replace(/\s+/g, "_"));
  const body: any = await fetchJson(`${SEARCH_URL}?p=${q}`, fetchImpl, signal);
  const first = Array.isArray(body?.player) ? body.player[0] : null;
  const cutout = first?.strCutout;
  return typeof cutout === "string" && cutout !== "" ? cutout : null;
}
