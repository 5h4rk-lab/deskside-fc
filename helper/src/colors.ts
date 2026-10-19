// Curated club accent colours, keyed by ESPN team abbreviation.
//
// The UI uses these as accents on a near-black green background (#0A1410), so the
// rule is: the club's recognisable primary colour, unless that is too dark to read
// there (relative luminance below ~0.08, e.g. navy or black), in which case the
// brighter secondary brand colour is used. A few clubs have no bright secondary
// (Fiorentina, Torino, Toulouse, ...) and use a lightened shade of their primary.
// tests/colors.test.ts enforces the luminance floor and the #RRGGBB format.
//
// ESPN abbreviations that two leagues share are keyed "<league>:<abbr>"; they are
// only found when the caller passes the league.

const FALLBACK = "#7C8F84";

export const TEAM_COLORS: Record<string, string> = {
  // Premier League (eng.1)
  BOU: "#DA291C", // AFC Bournemouth
  ARS: "#EF0107", // Arsenal
  AVL: "#95BFE5", // Aston Villa (claret is too dark: sky blue)
  "eng.1:BRE": "#E30613", // Brentford
  BHA: "#0057B8", // Brighton & Hove Albion
  CHE: "#DBA111", // Chelsea (royal blue is too dark: gold)
  COV: "#5CBFEB", // Coventry City
  CRY: "#C4122E", // Crystal Palace
  EVE: "#3F6FD1", // Everton (royal blue lightened)
  FUL: "#FFFFFF", // Fulham
  HUL: "#F5A12D", // Hull City
  IPS: "#3A64A3", // Ipswich Town
  LEE: "#FFFFFF", // Leeds United
  LIV: "#C8102E", // Liverpool
  MNC: "#6CABDD", // Manchester City
  MAN: "#DA291C", // Manchester United
  NEW: "#FFFFFF", // Newcastle United (black is too dark: white)
  NFO: "#E53233", // Nottingham Forest
  SUN: "#EB172B", // Sunderland
  TOT: "#FFFFFF", // Tottenham Hotspur (navy is too dark: white)

  // La Liga (esp.1)
  ALA: "#0761AF", // Alaves
  ATH: "#EE2523", // Athletic Club
  ATM: "#CB3524", // Atletico Madrid
  BAR: "#EDBB00", // Barcelona (blue and garnet are dark: gold)
  CEL: "#8AC3EE", // Celta Vigo
  DEP: "#0F6BB8", // Deportivo
  ELC: "#2D9B1E", // Elche
  ESP: "#007FC8", // Espanyol
  GET: "#0A5FB4", // Getafe
  LEV: "#C8142F", // Levante
  MCF: "#5AB3E8", // Malaga
  OSA: "#D91A21", // Osasuna
  RAC: "#1FA24A", // Racing Santander
  RAY: "#E53027", // Rayo Vallecano
  BET: "#0BB363", // Real Betis
  RMA: "#FFFFFF", // Real Madrid
  RSO: "#0067B1", // Real Sociedad
  SEV: "#D81022", // Sevilla
  VAL: "#F58A07", // Valencia
  VIL: "#FBE122", // Villarreal

  // Serie A (ita.1)
  MIL: "#FB090B", // AC Milan
  ROMA: "#F0BC42", // AS Roma (maroon is too dark: gold)
  ATA: "#1E71B8", // Atalanta
  BOL: "#C8102E", // Bologna
  CAG: "#CE1126", // Cagliari
  COMO: "#4169E1", // Como
  FIO: "#7B4FD0", // Fiorentina (purple lightened)
  FRO: "#FACF08", // Frosinone
  GEN: "#D2232A", // Genoa
  INT: "#0068A8", // Internazionale
  JUV: "#FFFFFF", // Juventus (black is too dark: white)
  LAZ: "#87D8F7", // Lazio
  LEC: "#F7D117", // Lecce
  "ita.1:MON": "#E2231A", // Monza
  NAP: "#12A0D7", // Napoli
  "ita.1:PAR": "#FFD200", // Parma
  SAS: "#00A550", // Sassuolo
  TOR: "#B83A5A", // Torino (granata lightened)
  UDI: "#FFFFFF", // Udinese (black is too dark: white)
  VEN: "#F58220", // Venezia (black is too dark: orange)

  // Bundesliga (ger.1)
  FCU: "#EB1923", // 1. FC Union Berlin
  B04: "#E32221", // Bayer Leverkusen
  MUN: "#DC052D", // Bayern Munich
  DOR: "#FDE100", // Borussia Dortmund
  BMG: "#00973B", // Borussia Moenchengladbach
  SGE: "#E1000F", // Eintracht Frankfurt
  FCA: "#BA3733", // FC Augsburg
  KOE: "#ED1C24", // FC Cologne
  HSV: "#0A6EBD", // Hamburg SV
  M05: "#C3141E", // Mainz
  RBL: "#DD0741", // RB Leipzig
  SCF: "#E2001A", // SC Freiburg
  SCP: "#1E63C8", // SC Paderborn 07
  ELV: "#FFFFFF", // SV Elversberg
  S04: "#0F63C9", // Schalke 04
  TSG: "#1961B5", // TSG Hoffenheim
  VFB: "#E32219", // VfB Stuttgart
  SVW: "#1D9053", // Werder Bremen

  // Ligue 1 (fra.1)
  AUX: "#0066B3", // AJ Auxerre
  "fra.1:MON": "#E51B22", // AS Monaco
  ANG: "#FFFFFF", // Angers (black is too dark: white)
  "fra.1:BRE": "#E2231A", // Brest
  HAC: "#5BA3DC", // Le Havre AC (navy is too dark: sky blue)
  MNS: "#E2331B", // Le Mans
  RCL: "#F4C20D", // Lens
  LILL: "#E01E13", // Lille
  LOR: "#F58220", // Lorient
  LYON: "#D8232A", // Lyon
  OLM: "#2FAEE0", // Marseille
  NICE: "#D20A11", // Nice
  "fra.1:PAR": "#2A62C9", // Paris FC
  PSG: "#DA291C", // Paris Saint-Germain (navy is too dark: red)
  REN: "#E13327", // Stade Rennais
  STR: "#1C7FD6", // Strasbourg
  TOU: "#8651C4", // Toulouse (purple lightened)
  TRY: "#0072CE", // Troyes
};

function normalizeHex(raw: string | undefined): string | null {
  const h = (raw ?? "").replace(/^#/, "");
  return /^[0-9a-fA-F]{6}$/.test(h) ? `#${h.toUpperCase()}` : null;
}

// Curated table first ("<league>:<abbr>", then the bare abbreviation), then the ESPN
// hex as #RRGGBB, then a neutral green-grey.
export function teamColor(abbr: string, espnHex?: string, league?: string): string {
  const curated = (league ? TEAM_COLORS[`${league}:${abbr}`] : undefined) ?? TEAM_COLORS[abbr];
  return curated ?? normalizeHex(espnHex) ?? FALLBACK;
}
