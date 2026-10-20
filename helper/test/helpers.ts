import { vi } from "vitest";

export function stubRes() {
  const res: any = { statusCode: 0, headers: {} as Record<string, string>, body: undefined };
  res.status = (c: number) => ((res.statusCode = c), res);
  res.setHeader = (k: string, v: string) => ((res.headers[k] = v), res);
  res.json = (b: unknown) => ((res.body = b), res);
  res.send = (b: unknown) => ((res.body = b), res);
  return res;
}

/** A fetch that never answers; it only rejects when the request's signal aborts. */
export function hangingFetch(): typeof fetch {
  return ((_url: unknown, init?: RequestInit) =>
    new Promise((_resolve, reject) => {
      const signal = init?.signal;
      if (!signal) return; // would hang forever: the test would time out, which is the point
      if (signal.aborted) return reject(signal.reason);
      signal.addEventListener("abort", () => reject(signal.reason), { once: true });
    })) as unknown as typeof fetch;
}

/**
 * Replaces AbortSignal.timeout with a 10 ms real timer and records the requested
 * durations, so deadline tests stay fast and can assert the configured values.
 */
export function fastTimeouts() {
  const real = AbortSignal.timeout.bind(AbortSignal);
  const requested: number[] = [];
  const spy = vi.spyOn(AbortSignal, "timeout").mockImplementation((ms: number) => {
    requested.push(ms);
    return real(10);
  });
  return { requested, restore: () => spy.mockRestore() };
}

/** A PNG header (signature + IHDR) claiming w x h pixels, 1-bit grayscale, followed by IDAT and IEND. */
export async function bigPng(w: number, h: number, withData: boolean): Promise<Buffer> {
  const { crc32, deflateSync } = await import("node:zlib");
  const chunk = (type: string, data: Buffer): Buffer => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body));
    return Buffer.concat([len, body, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 1; // bit depth
  ihdr[9] = 0; // grayscale
  const rowBytes = Math.ceil(w / 8) + 1; // + filter byte
  const raw = withData ? Buffer.alloc(rowBytes * h) : Buffer.alloc(rowBytes); // zeros compress to ~50 KB
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw)),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}
