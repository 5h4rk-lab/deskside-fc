import type {
  Lineup,
  LineupPlayer,
  MatchDetail,
  MatchEvent,
  MatchState,
  MatchSummary,
  StatRow,
  Team,
} from "./contract.js";
import { layoutFormation } from "./formation.js";

const ESPN_BASE = "https://site.api.espn.com/apis/site/v2/sports/soccer";
const FALLBACK_COLOR = "#7C8F84";

type Json = Record<string, any>;

export function crestPath(logoUrl: string | undefined, size = 48): string {
  if (!logoUrl) return "";
  return `/api/img?u=${encodeURIComponent(logoUrl)}&w=${size}&h=${size}`;
}

function teamColor(raw: unknown): string {
  return typeof raw === "string" && /^[0-9a-fA-F]{6}$/.test(raw) ? `#${raw.toUpperCase()}` : FALLBACK_COLOR;
}

function toTeam(competitor: Json): Team {
  const t: Json = competitor?.team ?? {};
  return {
    id: String(t.id ?? competitor?.id ?? ""),
    abbr: String(t.abbreviation ?? ""),
    name: String(t.displayName ?? t.name ?? ""),
    crest: crestPath(t.logo ?? t.logos?.[0]?.href),
    color: teamColor(t.color),
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
    home: toTeam(home),
    away: toTeam(away),
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

export async function fetchMatches(
  leagues: string[],
  from: string,
  to: string,
  fetchImpl: typeof fetch = fetch,
): Promise<MatchSummary[]> {
  const perLeague = await Promise.all(
    leagues.map(async (league) => {
      const url = `${ESPN_BASE}/${league}/scoreboard?dates=${from}-${to}`;
      const resp = await fetchImpl(url);
      if (!resp.ok) throw new Error(`ESPN ${league} scoreboard: HTTP ${resp.status}`);
      return normalizeScoreboard(await resp.json(), league);
    }),
  );
  return perLeague.flat().sort((a, b) => a.kickoff - b.kickoff);
}

export function cutoutPath(name: string, size = 40): string {
  return `/api/cutout?name=${encodeURIComponent(name)}&w=${size}&h=${size}`;
}

// ESPN position abbreviation -> [row rank (back to front), lateral rank (left to right)].
// formationPlace is a shirt-number-style slot, not a row index (in a 3-1-4-2 the wide
// midfielders are places 2 and 3), so rows are built from the position instead.
function roleRank(abbr: unknown): number {
  const a = String(abbr ?? "").toUpperCase();
  const base = a.split("-")[0];
  if (base === "G" || base === "GK") return 0;
  if (["CD", "CB", "LB", "RB", "LWB", "RWB", "D"].includes(base)) return 1;
  if (["DM", "SW", "CDM"].includes(base)) return 2;
  if (["AM", "CAM"].includes(base)) return 4;
  if (["F", "CF", "ST", "LF", "RF", "LW", "RW", "S"].includes(base)) return 5;
  return 3; // CM, LM, RM, M and anything unknown
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

function toLineup(roster: Json | undefined): Lineup {
  if (!roster) return { formation: "", players: [] };
  const formation = typeof roster.formation === "string" ? roster.formation : "";
  const starters: Json[] = (Array.isArray(roster.roster) ? roster.roster : []).filter((p: Json) => p?.starter === true);
  const place = (p: Json) => Number(p.formationPlace) || 99;
  const abbr = (p: Json) => p.position?.abbreviation;
  starters.sort((a, b) => {
    const keeperA = place(a) === 1 || roleRank(abbr(a)) === 0 ? 0 : 1;
    const keeperB = place(b) === 1 || roleRank(abbr(b)) === 0 ? 0 : 1;
    if (keeperA !== keeperB) return keeperA - keeperB;
    return (
      roleRank(abbr(a)) - roleRank(abbr(b)) ||
      lateralRank(abbr(a)) - lateralRank(abbr(b)) ||
      place(a) - place(b)
    );
  });
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
  const resp = await fetchImpl(`${ESPN_BASE}/${league}/summary?event=${id}`);
  if (!resp.ok) throw new Error(`ESPN ${league} summary ${id}: HTTP ${resp.status}`);
  return normalizeSummary(await resp.json(), league);
}
