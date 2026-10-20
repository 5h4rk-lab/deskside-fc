import type {
  Lineup,
  LineupPlayer,
  MatchDetail,
  MatchEvent,
  MatchState,
  MatchSummary,
  NewsItem,
  StatRow,
  Team,
} from "./contract.js";
import { teamColor } from "./colors.js";
import { layoutFormation, parseFormation } from "./formation.js";
import { fetchJson } from "./http.js";
import { isAllowedImageUrl, type Fit } from "./images.js";

const ESPN_BASE = "https://site.api.espn.com/apis/site/v2/sports/soccer";

type Json = Record<string, any>;

/**
 * Helper-relative /api/img path for an upstream image, or null when the URL is not on the
 * proxy's allow-list (the proxy would answer 400 for it, so never hand it to the device).
 */
export function safeImagePath(url: string, w: number, h: number, fit?: Fit): string | null {
  if (!isAllowedImageUrl(url)) return null;
  return `/api/img?u=${encodeURIComponent(url)}${fit ? `&fit=${fit}` : ""}&w=${w}&h=${h}`;
}

export function crestPath(logoUrl: string | undefined, size = 48): string {
  if (!logoUrl) return "";
  return safeImagePath(logoUrl, size, size) ?? "";
}

function toTeam(competitor: Json, league: string): Team {
  const t: Json = competitor?.team ?? {};
  const abbr = String(t.abbreviation ?? "");
  return {
    id: String(t.id ?? competitor?.id ?? ""),
    abbr,
    name: String(t.displayName ?? t.name ?? ""),
    crest: crestPath(t.logo ?? t.logos?.[0]?.href),
    color: teamColor(abbr, typeof t.color === "string" ? t.color : undefined, league),
  };
}

function toScore(raw: unknown): number | null {
  if (raw === undefined || raw === null || raw === "") return null;
  const n = Number(raw);
  return Number.isFinite(n) ? n : null;
}

function toState(status: Json | undefined): MatchState {
  const type: Json = status?.type ?? {};
  if (type.name === "STATUS_POSTPONED") return "postponed";
  switch (type.state) {
    case "in":
      return "live";
    case "post":
      return "post";
    default:
      return "pre";
  }
}

// Shared by the scoreboard (an event) and the summary (its header): both carry
// competitions[0] with competitors, status and date.
function toMatchSummary(ev: Json, league: string): MatchSummary | null {
  const comp: Json | undefined = ev?.competitions?.[0];
  const competitors: Json[] = comp?.competitors ?? [];
  const home = competitors.find((c) => c.homeAway === "home");
  const away = competitors.find((c) => c.homeAway === "away");
  if (!home || !away) return null;
  const status: Json | undefined = ev.status ?? comp?.status;
  const state = toState(status);
  const hasScore = state === "live" || state === "post";
  const kickoffMs = Date.parse(ev.date ?? comp?.date ?? "");
  return {
    id: String(ev.id),
    league,
    state,
    clock: state === "live" ? String(status?.displayClock ?? "") || null : null,
    kickoff: Number.isFinite(kickoffMs) ? Math.floor(kickoffMs / 1000) : 0,
    home: toTeam(home, league),
    away: toTeam(away, league),
    homeScore: hasScore ? toScore(home.score) : null,
    awayScore: hasScore ? toScore(away.score) : null,
  };
}

export function normalizeScoreboard(raw: unknown, league: string): MatchSummary[] {
  const events = (raw as Json | null)?.events;
  if (!Array.isArray(events)) return [];
  const out: MatchSummary[] = [];
  for (const ev of events) {
    const m = toMatchSummary(ev, league);
    if (m) out.push(m);
  }
  return out;
}

export const MAX_SPAN_DAYS = 14;
export const MAX_IN_FLIGHT = 8;

/** Thrown for an unusable from/to window; the endpoint maps it to HTTP 400. */
export class DateRangeError extends Error {}

function parseYmd(s: string): number {
  if (!/^\d{8}$/.test(s)) throw new DateRangeError("from/to must be YYYYMMDD");
  const y = Number(s.slice(0, 4));
  const m = Number(s.slice(4, 6));
  const d = Number(s.slice(6, 8));
  const ms = Date.UTC(y, m - 1, d);
  const back = new Date(ms);
  if (back.getUTCFullYear() !== y || back.getUTCMonth() !== m - 1 || back.getUTCDate() !== d) {
    throw new DateRangeError("from/to must be real calendar dates");
  }
  return ms;
}

/** Inclusive list of UTC calendar days from..to as YYYYMMDD (ESPN rejects date ranges). */
export function expandDates(from: string, to: string): string[] {
  const start = parseYmd(from);
  const end = parseYmd(to);
  if (start > end) throw new DateRangeError("from must not be after to");
  const days = Math.round((end - start) / 86_400_000) + 1;
  if (days > MAX_SPAN_DAYS) throw new DateRangeError(`span is limited to ${MAX_SPAN_DAYS} days`);
  return Array.from({ length: days }, (_, i) => new Date(start + i * 86_400_000).toISOString().slice(0, 10).replace(/-/g, ""));
}

// Runs the tasks with at most `limit` in flight; results keep the input order.
async function runLimited<T>(tasks: (() => Promise<T>)[], limit: number): Promise<T[]> {
  const results = new Array<T>(tasks.length);
  let next = 0;
  const worker = async (): Promise<void> => {
    while (next < tasks.length) {
      const i = next++;
      results[i] = await tasks[i]();
    }
  };
  await Promise.all(Array.from({ length: Math.min(limit, tasks.length) }, worker));
  return results;
}

export interface MatchesResult {
  matches: MatchSummary[];
  /** requests that failed (non-2xx, network error, timeout or invalid JSON) */
  failed: number;
  /** requests issued (or answered from the cache): leagues x dates */
  total: number;
}

export interface FetchMatchesOptions {
  /** Clock for the scoreboard cache (epoch ms); tests inject one. Defaults to Date.now. */
  now?: () => number;
}

// In-process cache for single-date scoreboard results, keyed "<league>|<YYYYMMDD>".
// Only successes are stored. A day that is "today" (UTC, by the injected clock) changes
// quickly, so it lives 20 s; every other day lives 10 min. Bounded: when full, the
// oldest-inserted entry is evicted first (Map iteration order is insertion order).
export const SCOREBOARD_CACHE_MAX = 256;
const TODAY_TTL_MS = 20_000;
const OTHER_DAY_TTL_MS = 600_000;
const scoreboardCache = new Map<string, { expires: number; matches: MatchSummary[] }>();

/** Empties the scoreboard cache (tests, and a clean slate for a long-lived instance). */
export function clearScoreboardCache(): void {
  scoreboardCache.clear();
}

function utcYmd(ms: number): string {
  return new Date(ms).toISOString().slice(0, 10).replace(/-/g, "");
}

function cacheGet(key: string, now: number): MatchSummary[] | null {
  const hit = scoreboardCache.get(key);
  if (!hit) return null;
  if (hit.expires <= now) {
    scoreboardCache.delete(key);
    return null;
  }
  return hit.matches;
}

function cacheSet(key: string, date: string, matches: MatchSummary[], now: number): void {
  const ttl = date === utcYmd(now) ? TODAY_TTL_MS : OTHER_DAY_TTL_MS;
  scoreboardCache.delete(key); // re-inserting moves the key to the newest position
  while (scoreboardCache.size >= SCOREBOARD_CACHE_MAX) {
    const oldest = scoreboardCache.keys().next();
    if (oldest.done) break;
    scoreboardCache.delete(oldest.value);
  }
  scoreboardCache.set(key, { expires: now + ttl, matches });
}

// One request per league per date: the real ESPN soccer scoreboard answers 400 to any
// dates=YYYYMMDD-YYYYMMDD range. Failures (including timeouts) are tolerated here and
// counted; the caller decides what a failure ratio means.
export async function fetchMatchesDetailed(
  leagues: string[],
  from: string,
  to: string,
  fetchImpl: typeof fetch = fetch,
  options: FetchMatchesOptions = {},
): Promise<MatchesResult> {
  const clock = options.now ?? Date.now;
  const dates = expandDates(from, to);
  const jobs = leagues.flatMap((league) =>
    dates.map((date) => async (): Promise<MatchSummary[] | null> => {
      const key = `${league}|${date}`;
      const cached = cacheGet(key, clock());
      if (cached) return cached;
      try {
        const matches = normalizeScoreboard(await fetchJson(`${ESPN_BASE}/${league}/scoreboard?dates=${date}`, fetchImpl), league);
        cacheSet(key, date, matches, clock());
        return matches;
      } catch (err) {
        console.error(`ESPN ${league} scoreboard ${date} failed: ${err instanceof Error ? err.message : "error"}`);
        return null;
      }
    }),
  );
  const results = await runLimited(jobs, MAX_IN_FLIGHT);
  const byId = new Map<string, MatchSummary>();
  for (const list of results) for (const m of list ?? []) if (!byId.has(m.id)) byId.set(m.id, m);
  const matches = [...byId.values()].sort((a, b) => a.kickoff - b.kickoff || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  return { matches, failed: results.filter((r) => r === null).length, total: jobs.length };
}

export async function fetchMatches(
  leagues: string[],
  from: string,
  to: string,
  fetchImpl: typeof fetch = fetch,
): Promise<MatchSummary[]> {
  const res = await fetchMatchesDetailed(leagues, from, to, fetchImpl);
  if (res.total > 0 && res.failed === res.total) throw new Error("ESPN scoreboard: every request failed");
  return res.matches;
}

export function cutoutPath(name: string, size = 40): string {
  return `/api/cutout?name=${encodeURIComponent(name)}&w=${size}&h=${size}`;
}

// ESPN position abbreviation -> row rank (back to front); -1 when unrecognised.
// formationPlace is a shirt-number-style slot, not a row index (in a 3-1-4-2 the wide
// midfielders are places 2 and 3), so rows are normally built from the position.
// That is only trusted when the per-rank counts agree with the formation (see toLineup).
const UNKNOWN_RANK = -1;
function roleRank(abbr: unknown): number {
  const base = String(abbr ?? "").toUpperCase().split("-")[0];
  if (base === "G" || base === "GK") return 0;
  if (["CD", "CB", "LB", "RB", "LWB", "RWB", "D"].includes(base)) return 1;
  if (["DM", "SW", "CDM"].includes(base)) return 2;
  if (["CM", "LM", "RM", "M", "MF"].includes(base)) return 3;
  if (["AM", "CAM"].includes(base)) return 4;
  if (["F", "CF", "ST", "LF", "RF", "LW", "RW", "S"].includes(base)) return 5;
  return UNKNOWN_RANK;
}

function lateralRank(abbr: unknown): number {
  const a = String(abbr ?? "").toUpperCase();
  const [base, suffix] = a.split("-");
  if (suffix === "L") return -1;
  if (suffix === "R") return 1;
  if (base.startsWith("L")) return -2;
  if (base.startsWith("R")) return 2;
  return 0;
}

// Order starters keeper first, then outfield rows back to front, each row left to right.
// Rule 1 (role rank): group by position rank, trusted only if there is exactly one keeper,
// every abbreviation is recognised, and the non-empty rank groups, back to front, have
// exactly the formation's row sizes. Rule 2 (fallback): outfield players in formationPlace
// order sliced by the formation digits, each row then ordered left to right by position
// suffix. Without usable digits there is nothing to check against, so the role-rank order
// is used and layoutFormation spreads the players evenly.
function orderStarters(starters: Json[], rows: number[] | null): Json[] {
  const place = (p: Json) => Number(p.formationPlace) || 99;
  const abbr = (p: Json) => p.position?.abbreviation;
  const rank = (p: Json) => roleRank(abbr(p));
  const byPlace = [...starters].sort((a, b) => place(a) - place(b));
  const keeper = byPlace.find((p) => place(p) === 1) ?? byPlace.find((p) => rank(p) === 0) ?? byPlace[0];
  if (!keeper) return [];
  const outfield = byPlace.filter((p) => p !== keeper);
  const lateral = (a: Json, b: Json) => lateralRank(abbr(a)) - lateralRank(abbr(b)) || place(a) - place(b);

  const groups = new Map<number, Json[]>();
  for (const p of outfield) groups.set(rank(p), [...(groups.get(rank(p)) ?? []), p]);
  const sizes = [1, 2, 3, 4, 5].filter((r) => groups.has(r)).map((r) => groups.get(r)!.length);
  const trusted =
    rank(keeper) === 0 &&
    !groups.has(0) &&
    !groups.has(UNKNOWN_RANK) &&
    rows !== null &&
    sizes.length === rows.length &&
    sizes.every((n, i) => n === rows[i]);

  if (rows === null || rows.reduce((a, b) => a + b, 0) !== outfield.length) {
    // no usable formation: layoutFormation will spread evenly; keep a stable role order
    return [keeper, ...[...outfield].sort((a, b) => (rank(a) - rank(b) || lateral(a, b)))];
  }
  if (trusted) {
    return [keeper, ...[1, 2, 3, 4, 5].flatMap((r) => (groups.get(r) ?? []).sort(lateral))];
  }
  const out = [keeper];
  let at = 0;
  for (const n of rows) {
    out.push(...outfield.slice(at, at + n).sort(lateral));
    at += n;
  }
  return out;
}

function toLineup(roster: Json | undefined): Lineup {
  if (!roster) return { formation: "", players: [] };
  const formation = typeof roster.formation === "string" ? roster.formation : "";
  const raw: Json[] = (Array.isArray(roster.roster) ? roster.roster : []).filter((p: Json) => p?.starter === true);
  const starters = orderStarters(raw, parseFormation(formation));
  const layout = layoutFormation(formation, starters.length);
  // layoutFormation lists each row left to right, so within a row (same y) lay the
  // players out left to right too, in the order the sort above already gave them.
  const players: LineupPlayer[] = starters.map((p, i) => {
    const name = String(p.athlete?.displayName ?? p.athlete?.fullName ?? "");
    return {
      name,
      number: Number(p.jersey) || 0,
      x: layout[i].x,
      y: layout[i].y,
      photo: cutoutPath(name),
    };
  });
  return { formation, players };
}

const STAT_ROWS: { name: string; label: string; format?: (v: string) => string }[] = [
  { name: "possessionPct", label: "Possession", format: (v) => `${Math.round(Number(v))}%` },
  { name: "totalShots", label: "Shots" },
  { name: "shotsOnTarget", label: "Shots on target" },
  { name: "wonCorners", label: "Corners" },
  { name: "foulsCommitted", label: "Fouls" },
];

function toStats(teams: Json[], homeId: string, awayId: string): StatRow[] {
  const find = (id: string, side: string): Json | undefined =>
    teams.find((t) => String(t?.team?.id) === id) ?? teams.find((t) => t?.homeAway === side);
  const home = find(homeId, "home");
  const away = find(awayId, "away");
  if (!home || !away) return [];
  const value = (t: Json, name: string): string | null => {
    const s = (Array.isArray(t.statistics) ? t.statistics : []).find((x: Json) => x?.name === name);
    const v = s?.displayValue ?? s?.value;
    return v === undefined || v === null || v === "" ? null : String(v);
  };
  const rows: StatRow[] = [];
  for (const def of STAT_ROWS) {
    const h = value(home, def.name);
    const a = value(away, def.name);
    if (h === null || a === null) continue;
    rows.push({ label: def.label, home: def.format ? def.format(h) : h, away: def.format ? def.format(a) : a });
  }
  return rows;
}

function eventType(t: unknown): MatchEvent["type"] | null {
  const s = String(t ?? "");
  if (s === "own-goal" || s === "penalty---scored" || s.startsWith("goal")) return "goal";
  if (s === "yellow-card") return "yellow";
  if (s === "red-card" || s === "yellow-red-card") return "red";
  if (s === "substitution") return "sub";
  return null;
}

function toEvents(keyEvents: unknown, homeId: string, awayId: string): MatchEvent[] {
  if (!Array.isArray(keyEvents)) return [];
  const out: MatchEvent[] = [];
  for (const ev of keyEvents as Json[]) {
    const type = eventType(ev?.type?.type);
    if (!type) continue;
    const tid = String(ev?.team?.id ?? "");
    const team = tid === homeId ? "home" : tid === awayId ? "away" : null;
    if (!team) continue;
    const people: Json[] = Array.isArray(ev.participants) ? ev.participants : [];
    const first = people[0]?.athlete?.displayName;
    const second = people[1]?.athlete?.displayName;
    let detail: string | null = null;
    if (type === "sub") {
      detail = second ? `For: ${second}` : null;
    } else if (type === "goal") {
      const kind = String(ev.type.type);
      if (kind === "own-goal") detail = "Own goal";
      else if (kind === "penalty---scored") detail = "Penalty";
      else if (second) detail = `Assist: ${second}`;
    }
    out.push({
      minute: String(ev.clock?.displayValue ?? ""),
      type,
      team,
      player: String(first ?? ""),
      detail,
    });
  }
  return out;
}

export function normalizeSummary(raw: unknown, league: string): MatchDetail {
  const r = (raw ?? {}) as Json;
  const match = toMatchSummary(r.header ?? {}, league);
  if (!match) throw new Error("summary has no usable header");
  if (match.state === "live" && match.clock === null) {
    const short = r.header?.competitions?.[0]?.status?.type?.shortDetail;
    if (typeof short === "string" && short) match.clock = short;
  }
  const rosters: Json[] = Array.isArray(r.rosters) ? r.rosters : [];
  const rosterFor = (id: string, side: string): Json | undefined =>
    rosters.find((x) => String(x?.team?.id) === id) ?? rosters.find((x) => x?.homeAway === side);
  return {
    match,
    events: toEvents(r.keyEvents, match.home.id, match.away.id),
    home: toLineup(rosterFor(match.home.id, "home")),
    away: toLineup(rosterFor(match.away.id, "away")),
    stats: toStats(Array.isArray(r.boxscore?.teams) ? r.boxscore.teams : [], match.home.id, match.away.id),
  };
}

export async function fetchSummary(
  league: string,
  id: string,
  fetchImpl: typeof fetch = fetch,
): Promise<MatchDetail> {
  return normalizeSummary(await fetchJson(`${ESPN_BASE}/${league}/summary?event=${id}`, fetchImpl), league);
}

function toNewsItem(a: Json): NewsItem {
  const imageUrl = (Array.isArray(a.images) ? a.images : []).map((i: Json) => i?.url).find((u: unknown) => typeof u === "string" && u);
  const publishedMs = Date.parse(String(a.published ?? ""));
  return {
    id: String(a.id ?? ""),
    headline: String(a.headline ?? ""),
    published: Number.isFinite(publishedMs) ? Math.floor(publishedMs / 1000) : 0,
    image: imageUrl ? safeImagePath(imageUrl, 240, 135, "cover") : null,
  };
}

export function normalizeNews(raw: unknown): NewsItem[] {
  const articles = (raw as Json | null)?.articles;
  if (!Array.isArray(articles)) return [];
  return articles.filter((a: unknown) => a && typeof a === "object").map((a: Json) => toNewsItem(a));
}

export async function fetchNews(league: string, fetchImpl: typeof fetch = fetch): Promise<NewsItem[]> {
  return normalizeNews(await fetchJson(`${ESPN_BASE}/${league}/news`, fetchImpl));
}

export function normalizeTeams(raw: unknown, league: string): (Team & { league: string })[] {
  const teams = (raw as Json | null)?.sports?.[0]?.leagues?.[0]?.teams;
  if (!Array.isArray(teams)) return [];
  return teams
    .map((entry: Json) => entry?.team)
    .filter((t: unknown): t is Json => !!t && typeof t === "object")
    .map((t: Json) => ({ ...toTeam({ team: t }, league), league }));
}

export const TEAM_LEAGUES = ["eng.1", "esp.1", "ita.1", "ger.1", "fra.1"];

// One bad league must not empty the team picker: only throw when every league failed.
export async function fetchTeams(
  leagues: string[] = TEAM_LEAGUES,
  fetchImpl: typeof fetch = fetch,
): Promise<(Team & { league: string })[]> {
  const settled = await Promise.allSettled(
    leagues.map(async (league) => normalizeTeams(await fetchJson(`${ESPN_BASE}/${league}/teams`, fetchImpl), league)),
  );
  const teams: (Team & { league: string })[] = [];
  let failed = 0;
  settled.forEach((r, i) => {
    if (r.status === "fulfilled") {
      teams.push(...r.value);
      return;
    }
    failed++;
    console.error(`ESPN ${leagues[i]} teams failed: ${r.reason instanceof Error ? r.reason.message : "error"}`);
  });
  if (leagues.length > 0 && failed === leagues.length) throw new Error("ESPN teams: every league failed");
  return teams;
}
