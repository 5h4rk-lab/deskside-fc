// Data contract shared with the C++ UI. Times are epoch seconds UTC.

export interface Team {
  id: string;
  abbr: string;
  name: string;
  crest: string;
  color: string; // "#RRGGBB": readable accent for small details on the dark UI
  // "#RRGGBB": the club's real brand colour for large surfaces (may be dark).
  // Devices treat it as optional and fall back to `color` when it is absent or invalid.
  primary: string;
}

export type MatchState = "pre" | "live" | "post" | "postponed";

export interface MatchSummary {
  id: string;
  league: string;
  state: MatchState;
  clock: string | null;
  kickoff: number;
  home: Team;
  away: Team;
  homeScore: number | null;
  awayScore: number | null;
}

export interface MatchEvent {
  minute: string;
  type: "goal" | "yellow" | "red" | "sub";
  team: "home" | "away";
  player: string;
  detail: string | null;
}

export interface LineupPlayer {
  name: string;
  number: number;
  x: number;
  y: number;
  photo: string | null;
}

export interface Lineup {
  formation: string;
  players: LineupPlayer[];
}

export interface StatRow {
  label: string;
  home: string;
  away: string;
}

export interface MatchDetail {
  match: MatchSummary;
  events: MatchEvent[];
  home: Lineup;
  away: Lineup;
  stats: StatRow[];
}

export interface NewsItem {
  id: string;
  headline: string;
  published: number;
  image: string | null;
}
