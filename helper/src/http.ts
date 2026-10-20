import type { VercelResponse } from "@vercel/node";

/** Per-request deadline for every ESPN / TheSportsDB call. */
export const UPSTREAM_TIMEOUT_MS = 5000;

/** Shared deadline for /api/cutout (lookup plus download); the device gives up after 10 s. */
export const CUTOUT_DEADLINE_MS = 7000;

/** Sends a JSON error that no cache may store. */
export function sendError(res: VercelResponse, status: number, error: string): void {
  res.setHeader("Cache-Control", "no-store");
  res.status(status).json({ error });
}

/**
 * GET + JSON with a deadline. Throws on a non-2xx status, a network error, a timeout
 * or an unparseable body. The parse error is replaced by a fixed message because Node's
 * SyntaxError text includes a snippet of the upstream body.
 */
export async function fetchJson(
  url: string,
  fetchImpl: typeof fetch = fetch,
  signal: AbortSignal = AbortSignal.timeout(UPSTREAM_TIMEOUT_MS),
): Promise<unknown> {
  const resp = await fetchImpl(url, { signal });
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  try {
    return await resp.json();
  } catch (err) {
    if (signal.aborted) throw err; // a timeout while reading the body is not a parse failure
    throw new Error("upstream returned invalid JSON");
  }
}
