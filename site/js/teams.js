// Demo data only. Club names are plain text, colours are approximate, and
// crests are drawn as generic shields. Player names are invented.
export const CLUBS = {
  ARS: { n: "Arsenal", c: "#EF0107", ink: "#FF3B3F" },
  CHE: { n: "Chelsea", c: "#034694", ink: "#3A82E6" },
  TOT: { n: "Spurs", c: "#132257", c2: "#FFFFFF", ink: "#DDE3F5" },
  BAR: { n: "Barcelona", c: "#A50044", c2: "#004D98", ink: "#E5336F" },
  MCI: { n: "Man City", c: "#6CABDD", ink: "#6CABDD", dark: true },
  BAY: { n: "Bayern", c: "#DC052D", ink: "#FF4562" },
  LIV: { n: "Liverpool", c: "#C8102E", ink: "#E8506A" },
  VAL: { n: "Valencia", c: "#E87A00", ink: "#FFA033" },
  BVB: { n: "Dortmund", c: "#F5D400", ink: "#F5D400", dark: true },
  RMA: { n: "Real Madrid", c: "#DADCE8", ink: "#DADCE8", dark: true },
  PSG: { n: "Paris SG", c: "#10265F", c2: "#DA2C38", ink: "#6F8DE0" },
  OM: { n: "Marseille", c: "#2FA7DC", ink: "#2FA7DC", dark: true },
};

// Teams a visitor can switch between, with the opponent shown in the demo.
export const PICKS = [
  { id: "ARS", opp: "CHE" },
  { id: "CHE", opp: "ARS" },
  { id: "TOT", opp: "LIV" },
  { id: "BAR", opp: "VAL" },
  { id: "MCI", opp: "LIV" },
  { id: "BAY", opp: "BVB" },
];

export const SCORERS = ["M. Okafor", "T. Lindqvist", "J. Duarte", "R. Novak", "S. Haddad"];

// A generic shield in the club colours. Not any real crest.
export function shield(id, size = 24) {
  const k = CLUBS[id];
  const c2 = k.c2 || "rgba(255,255,255,.28)";
  return `<svg class="shield" width="${size}" height="${size}" viewBox="0 0 32 36" aria-hidden="true">
<path d="M3 3h26v16c0 8-6 13-13 15C9 32 3 27 3 19z" fill="${k.c}" stroke="rgba(241,239,228,.85)" stroke-width="1.6"/>
<path d="M16 3h13v16c0 8-6 13-13 15z" fill="${c2}" opacity=".55"/>
<path d="M9 11h14M9 16h14" stroke="rgba(255,255,255,.55)" stroke-width="1.4" stroke-linecap="round"/></svg>`;
}

// Head-and-shoulders silhouette standing in for a player cutout.
export function cutout(cls = "") {
  return `<svg class="cutout ${cls}" viewBox="0 0 120 150" aria-hidden="true">
<path d="M8 150c0-34 18-50 52-54 34 4 52 20 52 54z" fill="var(--kit)"/>
<path d="M44 98l16 18 16-18" fill="none" stroke="rgba(241,239,228,.85)" stroke-width="5" stroke-linejoin="round"/>
<rect x="50" y="76" width="20" height="26" rx="8" fill="#c8967a"/>
<ellipse cx="60" cy="52" rx="25" ry="31" fill="#d9a88b"/>
<path d="M34 50c-2-24 14-34 28-33 16 1 27 12 24 33-6-12-14-16-26-16s-20 5-26 16z" fill="#2a1d17"/></svg>`;
}
