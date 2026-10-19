import type { MatchState, MatchSummary, Team } from "./contract.js";

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
    crest: crestPath(t.logo),
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

export function normalizeScoreboard(raw: unknown, league: string): MatchSummary[] {
  const events = (raw as Json | null)?.events;
  if (!Array.isArray(events)) return [];
  const out: MatchSummary[] = [];
  for (const ev of events) {
    const comp: Json | undefined = ev?.competitions?.[0];
    const competitors: Json[] = comp?.competitors ?? [];
    const home = competitors.find((c) => c.homeAway === "home");
    const away = competitors.find((c) => c.homeAway === "away");
    if (!home || !away) continue;
    const status: Json | undefined = ev.status ?? comp?.status;
    const state = toState(status);
    const kickoffMs = Date.parse(ev.date ?? comp?.date ?? "");
    out.push({
      id: String(ev.id),
      league,
      state,
      clock: state === "live" ? String(status?.displayClock ?? "") || null : null,
      kickoff: Number.isFinite(kickoffMs) ? Math.floor(kickoffMs / 1000) : 0,
      home: toTeam(home),
      away: toTeam(away),
      homeScore: toScore(home.score),
      awayScore: toScore(away.score),
    });
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
