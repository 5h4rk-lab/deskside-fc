import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { normalizeScoreboard, normalizeSummary } from "../src/espn.js";

export const fixtureDir = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "contract", "fixtures");

const load = (name: string): unknown => JSON.parse(readFileSync(join(fixtureDir, name), "utf8"));

// Fixed so regenerating is deterministic.
export const FIXTURE_UPDATED = 1760900000;

export function buildMatchesFixture(): unknown {
  return { updated: FIXTURE_UPDATED, matches: normalizeScoreboard(load("scoreboard-post.json"), "eng.1") };
}

export function buildDetailFixture(): unknown {
  return normalizeSummary(load("summary-post.json"), "eng.1");
}
