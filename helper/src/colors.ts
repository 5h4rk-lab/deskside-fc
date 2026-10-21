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

// Real club brand colours for LARGE surfaces (the Live split background, the goal
// flood, the News fallback gradient, pitch markers). No luminance rule here: the
// UI darkens and lightens these itself where needed, so Chelsea is its royal blue,
// not the gold accent above. Clubs whose identity colour is white (or near-white)
// would flood the screen white, so they use their strongest dark/secondary identity
// colour instead; those entries are marked "white primary". Same keys as TEAM_COLORS
// (tests enforce it).
export const TEAM_PRIMARIES: Record<string, string> = {
  // Premier League (eng.1)
  BOU: "#DA291C", // AFC Bournemouth (red)
  ARS: "#EF0107", // Arsenal (red)
  AVL: "#670E36", // Aston Villa (claret)
  "eng.1:BRE": "#E30613", // Brentford (red)
  BHA: "#0057B8", // Brighton & Hove Albion (blue)
  CHE: "#034694", // Chelsea (royal blue)
  COV: "#5CBFEB", // Coventry City (sky blue)
  CRY: "#1B458F", // Crystal Palace (blue)
  EVE: "#003399", // Everton (royal blue)
  FUL: "#000000", // Fulham (white primary: black)
  HUL: "#F5A12D", // Hull City (amber)
  IPS: "#3A64A3", // Ipswich Town (blue)
  LEE: "#1D428A", // Leeds United (white primary: blue)
  LIV: "#C8102E", // Liverpool (red)
  MNC: "#6CABDD", // Manchester City (sky blue)
  MAN: "#DA291C", // Manchester United (red)
  NEW: "#241F20", // Newcastle United (black)
  NFO: "#E53233", // Nottingham Forest (red)
  SUN: "#EB172B", // Sunderland (red)
  TOT: "#132257", // Tottenham Hotspur (white primary: navy)

  // La Liga (esp.1)
  ALA: "#0761AF", // Alaves (blue)
  ATH: "#EE2523", // Athletic Club (red)
  ATM: "#CB3524", // Atletico Madrid (red)
  BAR: "#A50044", // Barcelona (garnet)
  CEL: "#8AC3EE", // Celta Vigo (sky blue)
  DEP: "#0F6BB8", // Deportivo (blue)
  ELC: "#00843D", // Elche (white primary: green)
  ESP: "#007FC8", // Espanyol (blue)
  GET: "#005999", // Getafe (blue)
  LEV: "#B4053F", // Levante (garnet)
  MCF: "#5AB3E8", // Malaga (sky blue)
  OSA: "#D91A21", // Osasuna (red)
  RAC: "#1FA24A", // Racing Santander (white primary: green)
  RAY: "#E53027", // Rayo Vallecano (white primary: red sash)
  BET: "#0BB363", // Real Betis (green)
  RMA: "#00529F", // Real Madrid (white primary: crest blue)
  RSO: "#0067B1", // Real Sociedad (blue)
  SEV: "#D81022", // Sevilla (white primary: red)
  VAL: "#F58A07", // Valencia (white primary: orange)
  VIL: "#FBE122", // Villarreal (yellow)

  // Serie A (ita.1)
  MIL: "#FB090B", // AC Milan (red)
  ROMA: "#8E1F2F", // AS Roma (maroon)
  ATA: "#1E71B8", // Atalanta (blue)
  BOL: "#A21C26", // Bologna (red)
  CAG: "#CE1126", // Cagliari (red)
  COMO: "#4169E1", // Como (blue)
  FIO: "#482E92", // Fiorentina (purple)
  FRO: "#FACF08", // Frosinone (yellow)
  GEN: "#D2232A", // Genoa (red)
  INT: "#0068A8", // Internazionale (blue)
  JUV: "#000000", // Juventus (white primary: black)
  LAZ: "#87D8F7", // Lazio (sky blue)
  LEC: "#F7D117", // Lecce (yellow)
  "ita.1:MON": "#E2231A", // Monza (red)
  NAP: "#12A0D7", // Napoli (blue)
  "ita.1:PAR": "#FFD200", // Parma (yellow)
  SAS: "#00A550", // Sassuolo (green)
  TOR: "#8A1E03", // Torino (granata)
  UDI: "#000000", // Udinese (white primary: black)
  VEN: "#000000", // Venezia (black)

  // Bundesliga (ger.1)
  FCU: "#EB1923", // 1. FC Union Berlin (red)
  B04: "#E32221", // Bayer Leverkusen (red)
  MUN: "#DC052D", // Bayern Munich (red)
  DOR: "#FDE100", // Borussia Dortmund (yellow)
  BMG: "#00973B", // Borussia Moenchengladbach (green)
  SGE: "#E1000F", // Eintracht Frankfurt (red)
  FCA: "#BA3733", // FC Augsburg (red)
  KOE: "#ED1C24", // FC Cologne (red)
  HSV: "#0A6EBD", // Hamburg SV (blue)
  M05: "#C3141E", // Mainz (red)
  RBL: "#DD0741", // RB Leipzig (red)
  SCF: "#E2001A", // SC Freiburg (red)
  SCP: "#1E63C8", // SC Paderborn 07 (blue)
  ELV: "#000000", // SV Elversberg (white primary: black)
  S04: "#004D9D", // Schalke 04 (royal blue)
  TSG: "#1961B5", // TSG Hoffenheim (blue)
  VFB: "#E32219", // VfB Stuttgart (red)
  SVW: "#1D9053", // Werder Bremen (green)

  // Ligue 1 (fra.1)
  AUX: "#0066B3", // AJ Auxerre (blue)
  "fra.1:MON": "#E51B22", // AS Monaco (red)
  ANG: "#000000", // Angers (white primary: black)
  "fra.1:BRE": "#E2231A", // Brest (red)
  HAC: "#0D2C54", // Le Havre AC (navy)
  MNS: "#E2331B", // Le Mans (red)
  RCL: "#F4C20D", // Lens (yellow)
  LILL: "#E01E13", // Lille (red)
  LOR: "#F58220", // Lorient (orange)
  LYON: "#14387F", // Lyon (white primary: blue)
  OLM: "#2FAEE0", // Marseille (white primary: sky blue)
  NICE: "#D20A11", // Nice (red)
  "fra.1:PAR": "#2A62C9", // Paris FC (blue)
  PSG: "#004170", // Paris Saint-Germain (navy)
  REN: "#E13327", // Stade Rennais (red)
  STR: "#1C7FD6", // Strasbourg (blue)
  TOU: "#4E2A84", // Toulouse (violet)
  TRY: "#0072CE", // Troyes (blue)
};

function normalizeHex(raw: string | undefined): string | null {
  const h = (raw ?? "").replace(/^#/, "");
  return /^[0-9a-fA-F]{6}$/.test(h) ? `#${h.toUpperCase()}` : null;
}

// Own properties only: an abbreviation like "constructor" must not hit Object.prototype.
function curated(table: Record<string, string>, key: string): string | undefined {
  return Object.hasOwn(table, key) ? table[key] : undefined;
}

function lookup(table: Record<string, string>, abbr: string, espnHex?: string, league?: string): string {
  const hit = (league ? curated(table, `${league}:${abbr}`) : undefined) ?? curated(table, abbr);
  return hit ?? normalizeHex(espnHex) ?? FALLBACK;
}

// Curated table first ("<league>:<abbr>", then the bare abbreviation), then the ESPN
// hex as #RRGGBB, then a neutral green-grey.
export function teamColor(abbr: string, espnHex?: string, league?: string): string {
  return lookup(TEAM_COLORS, abbr, espnHex, league);
}

// The real brand colour for large surfaces; same fallbacks as teamColor.
export function teamPrimary(abbr: string, espnHex?: string, league?: string): string {
  return lookup(TEAM_PRIMARIES, abbr, espnHex, league);
}
