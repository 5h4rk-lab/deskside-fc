/**
 * The newest published device firmware, as GET /api/firmware/version serves it.
 *
 * The manifest source is helper/firmware-manifest.json, written by tools/publish-firmware.sh:
 *   {"version": "0.1.1", "file": "firmware/0.1.1.bin", "sha256": "<64 hex>", "size": 1634512, "notes": "..."}
 * or {"version": null} while nothing is published. The binary itself is a static file under
 * helper/public/firmware/ (public: no secret inside; its integrity comes from the SHA-256 in this
 * token-gated manifest and the TLS channel). The device checks everything again (ui/src/update_check.h).
 */

/** Bounds the device applies too: at least 100 KB, at most one 5 MB app slot. */
export const FIRMWARE_MIN_BYTES = 100 * 1024;
export const FIRMWARE_MAX_BYTES = 0x500000;
export const FIRMWARE_NOTES_MAX = 200;

export interface FirmwareManifest {
  version: string;
  url: string;
  sha256: string;
  size: number;
  notes: string;
}

export type FirmwareAnswer =
  | { status: 200; body: FirmwareManifest }
  | { status: 404; body: { error: string } }
  | { status: 500; body: { error: string } };

const VERSION = /^(0|[1-9]\d{0,3})\.(0|[1-9]\d{0,3})\.(0|[1-9]\d{0,3})$/;
const SHA256 = /^[0-9a-f]{64}$/;
const HOST = /^[a-z0-9.-]+(:\d{1,5})?$/;

/** The request's public host ("host" or "host:port", lower case), or null when it is unusable. */
export function publicHost(headers: Record<string, string | string[] | undefined>): string | null {
  const pick = (v: string | string[] | undefined) => (Array.isArray(v) ? v[0] : v);
  const raw = pick(headers["x-forwarded-host"]) ?? pick(headers["host"]);
  if (!raw) return null;
  const host = raw.split(",")[0].trim().toLowerCase();
  return HOST.test(host) ? host : null;
}

/**
 * Checks the manifest source and builds the answer for a request that reached `host`.
 * A source that fails a check is a server error (never served half-valid).
 */
export function firmwareAnswer(source: unknown, host: string | null): FirmwareAnswer {
  if (source === null || typeof source !== "object" || Array.isArray(source)) {
    return { status: 500, body: { error: "bad firmware manifest" } };
  }
  const m = source as Record<string, unknown>;
  if (m.version === null || m.version === undefined) return { status: 404, body: { error: "no firmware published" } };
  const bad = { status: 500 as const, body: { error: "bad firmware manifest" } };
  if (typeof m.version !== "string" || !VERSION.test(m.version)) return bad;
  if (m.file !== `firmware/${m.version}.bin`) return bad;
  if (typeof m.sha256 !== "string" || !SHA256.test(m.sha256)) return bad;
  if (typeof m.size !== "number" || !Number.isInteger(m.size)) return bad;
  if (m.size < FIRMWARE_MIN_BYTES || m.size > FIRMWARE_MAX_BYTES) return bad;
  const notes = m.notes ?? "";
  if (typeof notes !== "string" || notes.length > FIRMWARE_NOTES_MAX) return bad;
  if (!host) return { status: 500, body: { error: "unknown host" } };
  return {
    status: 200,
    body: { version: m.version, url: `https://${host}/${m.file}`, sha256: m.sha256, size: m.size, notes },
  };
}
