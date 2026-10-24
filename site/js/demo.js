import { CLUBS, PICKS, SCORERS, shield, cutout } from "./teams.js";
import { playHorn } from "./sound.js";

const ICONS = {
  live: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7l4 3-1.5 5h-5L8 10z"/></svg>',
  matches: '<svg viewBox="0 0 24 24"><rect x="4" y="5" width="16" height="15" rx="2"/><path d="M4 10h16M9 3v4M15 3v4"/></svg>',
  news: '<svg viewBox="0 0 24 24"><path d="M5 5h11v14H7a2 2 0 01-2-2zM16 9h3v8a2 2 0 01-2 2M8 9h5M8 13h5"/></svg>',
};
const TABS = [["live", "Live"], ["matches", "Matches"], ["news", "News"]];
const GOAL_MS = 4000;
const reduced = () => window.matchMedia("(prefers-reduced-motion: reduce)").matches;

const HOME_XI = [[50,92,1],[14,76,12],[38,79,2],[62,79,6],[86,76,3],[28,62,41],[50,66,10],[72,62,8],[18,54,7],[50,56,9],[82,54,11]];
const AWAY_XI = [[50,8,1],[14,22,24],[38,20,6],[62,20,5],[86,22,27],[34,34,25],[66,34,8],[50,42,10],[22,41,7],[78,41,11]];

const state = {
  id: "ARS", tab: "live", detail: null, sub: "pitch",
  score: [2, 1], minute: 67, sound: false, goal: false,
  events: [[12, "Goal", 0, "M. Okafor"], [31, "Goal", 1, "T. Lindqvist"], [44, "Yellow card", 1, "S. Haddad"], [58, "Goal", 0, "J. Duarte"]],
  scorer: 0,
};
let root, timers = [], tick;

const pick = () => PICKS.find((p) => p.id === state.id);
const club = (id) => CLUBS[id];
const fav = () => state.id;
const opp = () => pick().opp;

function statusBar(left, right) {
  return `<div class="status"><span>${left}</span><span>${right}</span></div>`;
}
const clock = "09:42 ●";

function tabbar() {
  return `<div class="tabbar" role="tablist" aria-label="Screens">${TABS.map(([k, label]) =>
    `<button type="button" role="tab" id="tab-${k}" aria-selected="${state.tab === k}" aria-controls="scr" tabindex="${state.tab === k ? 0 : -1}" data-tab="${k}">${ICONS[k]}<span>${label}</span></button>`).join("")}</div>`;
}

function matchObj(h, a, hs, as, label, live) { return { h, a, hs, as, label, live }; }
function featured() { return matchObj(fav(), opp(), state.score[0], state.score[1], state.minute + "'", true); }

function viewLive() {
  const [h, a] = [fav(), opp()];
  return `<div class="live-bg"></div><div class="live-shade"></div>
${statusBar("Premier League", clock)}
<span class="crest" style="left:8px;top:26px">${shield(h, 84)}</span>
<span class="crest" style="right:8px;top:26px">${shield(a, 84)}</span>
<div class="min disp">${state.minute}'</div>
<div class="score disp">${state.score[0]}<i>:</i>${state.score[1]}</div>
<div class="names disp"><span>${club(h).n.toUpperCase()}</span><span>${club(a).n.toUpperCase()}</span></div>
<button type="button" class="event" data-open="0" aria-label="Open match detail">
  <span class="face">${cutout()}</span>
  <span><span class="t">${lastEvent()[0]}'  ${lastEvent()[1]}</span><span class="n">${lastEvent()[3]}</span></span>
</button>`;
}
function lastEvent() { return state.events.filter((e) => e[1] === "Goal" && e[2] === 0).slice(-1)[0] || state.events[0]; }

function row(m, i, isFav) {
  const mid = m.live || m.done ? `${m.hs}:${m.as}<small>${m.label}</small>` : `${m.time}<small>${m.label}</small>`;
  return `<button type="button" class="m${m.live ? " lv" : ""}${isFav ? " fav" : ""}" data-open="${i}" aria-label="${club(m.h).n} against ${club(m.a).n}">
<span class="h">${shield(m.h, 22)}${m.h}</span><span class="c">${mid}</span><span class="a">${m.a}${shield(m.a, 22)}</span></button>`;
}
const others = [
  { h: "RMA", a: "BAR", time: "19:00", label: "La Liga", day: "TODAY" },
  { h: "MCI", a: "LIV", time: "17:30", label: "PL", day: "TOMORROW" },
  { h: "BAY", a: "BVB", time: "20:00", label: "Bundesliga", day: "TOMORROW" },
  { h: "PSG", a: "OM", hs: 3, as: 1, label: "FT", done: true, day: "YESTERDAY" },
];
function allMatches() { return [featured(), ...others]; }
function viewMatches() {
  const list = allMatches();
  let html = statusBar("Matches", clock) + '<div class="list"><div class="day disp">LIVE NOW</div>';
  html += row({ ...list[0], h: fav(), a: opp() }, 0, true);
  let day = "";
  list.slice(1).forEach((m, i) => {
    if (m.day !== day) { day = m.day; html += `<div class="day disp">${day}</div>`; }
    html += row(m, i + 1, false);
  });
  return html + "</div>";
}

function viewNews() {
  const f = club(fav()), name = f.n.toUpperCase();
  return `<div class="lead">${cutout("lead-pl")}<div class="status"><span>Last night</span><span>${clock}</span></div>
<div class="h disp">LATE GOAL KEEPS ${name} RUN GOING</div></div>
<div class="ft"><div>${f.n}<b>3 : 1</b></div><div>Next up<b>Sat 17:30</b></div></div>
<div class="n2" style="top:218px"><span class="th"></span><span>Demo headline: squad news ahead of the weekend<small>2h ago</small></span></div>
<div class="n2" style="top:268px"><span class="th"></span><span>Demo headline: what the table looks like now<small>5h ago</small></span></div>`;
}

function viewDetail() {
  const m = allMatches()[state.detail];
  const real = state.detail === 0;
  const live = m.live;
  const hc = club(m.h), ac = club(m.a);
  const subs = ["summary", "pitch", "stats"];
  let body = "";
  if (state.sub === "pitch") {
    const dot = (x, cls, id) => `<span class="p ${cls}" style="left:${x[0]}%;top:${x[1]}%">${x[2]}</span>`;
    body = `<div class="pitch"><span class="box t"></span><span class="box b"></span>
${HOME_XI.map((x) => dot(x, "h")).join("")}${AWAY_XI.map((x) => dot(x, "a")).join("")}</div>`;
  } else if (state.sub === "summary") {
    const ev = real ? state.events : [];
    body = `<ul class="events">${ev.length ? ev.map((e) => `<li><b class="disp">${e[0]}'</b><span class="dot ${e[1] === "Goal" ? "g" : "y"}"></span><span>${e[1]}<small>${e[3]} (${club(e[2] ? m.a : m.h).n})</small></span></li>`).join("") : '<li class="none">Events are only filled in for the featured match in this demo.</li>'}</ul>`;
  } else {
    const rows = [["Possession", 54, 46, "%"], ["Shots", 11, 7], ["On target", 5, 3], ["Corners", 6, 3], ["Fouls", 9, 12]];
    body = `<div class="stats">${rows.map(([k, x, y, s = ""]) => `<div class="st"><span class="disp">${x}${s}</span><i>${k}<em><u style="width:${(x / (x + y)) * 100}%"></u></em></i><span class="disp">${y}${s}</span></div>`).join("")}</div>`;
  }
  const scoreTxt = m.live || m.done ? `${m.h} ${m.hs}:${m.as} ${m.a}` : `${m.h} v ${m.a}`;
  return `${statusBar('<button type="button" class="back" data-back="1">‹ Back</button>', live ? m.label + " ●" : m.label || "")}
<div class="ph"><span class="tt disp">${scoreTxt}</span><span class="chip">4-3-3</span></div>
<div class="sub" role="group" aria-label="Detail view">${subs.map((s) => `<button type="button" data-sub="${s}" aria-pressed="${state.sub === s}" class="${state.sub === s ? "on" : ""}">${s[0].toUpperCase() + s.slice(1)}</button>`).join("")}</div>
${body}`;
}

function viewGoal() {
  const who = SCORERS[state.scorer % SCORERS.length];
  return `<div class="rays"></div><div class="conf"></div>
<div class="big disp">GOAL!</div>
<div class="pl">${cutout()}</div>
<div class="who disp">${who.toUpperCase()}<small>${state.minute}'  ·  ${club(fav()).n} ${state.score[0]} : ${state.score[1]}</small></div>`;
}

export function render() {
  const d = root;
  const f = club(fav()), o = club(opp());
  const s = d.style;
  s.setProperty("--team", f.c);
  s.setProperty("--away", o.c);
  s.setProperty("--ink", f.ink);
  s.setProperty("--kit", f.c);
  document.documentElement.style.setProperty("--accent", f.ink);
  let view;
  if (state.detail !== null) view = viewDetail();
  else view = state.tab === "live" ? viewLive() : state.tab === "matches" ? viewMatches() : viewNews();
  d.innerHTML = `<div class="screen" id="scr" role="tabpanel" aria-labelledby="tab-${state.tab}" data-view="${state.detail !== null ? "detail" : state.tab}">${view}</div>${tabbar()}
<div class="goalov${state.goal ? " on" : ""}" aria-hidden="true">${state.goal ? viewGoal() : ""}</div>`;
  d.classList.toggle("is-goal", state.goal);
  document.querySelectorAll("[data-team]").forEach((b) => b.setAttribute("aria-pressed", String(b.dataset.team === state.id)));
}

function score() {
  if (state.goal) return;
  state.goal = true;
  state.score[0] += 1;
  state.scorer = (state.scorer + 1) % SCORERS.length;
  state.events.push([state.minute, "Goal", 0, SCORERS[state.scorer % SCORERS.length]]);
  const btn = document.getElementById("goal-btn");
  btn.disabled = true;
  document.getElementById("live-msg").textContent = `Goal for ${club(fav()).n}, ${state.score[0]} to ${state.score[1]}.`;
  if (state.sound) playHorn();
  render();
  timers.push(setTimeout(() => {
    state.goal = false;
    state.tab = "live";
    state.detail = null;
    if (state.score[0] >= 9) { state.score = [2, 1]; state.events = state.events.slice(0, 4); }
    btn.disabled = false;
    render();
    btn.focus({ preventScroll: true });
  }, GOAL_MS));
}

export function setTeam(id) {
  if (!CLUBS[id] || state.goal) return;
  state.id = id;
  render();
}

export function initDemo(mount) {
  root = mount;
  root.addEventListener("click", (e) => {
    if (state.goal) return;
    const t = e.target.closest("button");
    if (!t) return;
    if (t.dataset.tab) { state.tab = t.dataset.tab; state.detail = null; render(); document.getElementById("tab-" + state.tab).focus(); }
    else if (t.dataset.open !== undefined) { state.detail = Number(t.dataset.open); state.sub = "pitch"; render(); root.querySelector("[data-back]").focus(); }
    else if (t.dataset.back) { state.detail = null; render(); document.getElementById("tab-" + state.tab).focus(); }
    else if (t.dataset.sub) { state.sub = t.dataset.sub; render(); root.querySelector(`[data-sub="${state.sub}"]`).focus(); }
  });
  root.addEventListener("keydown", (e) => {
    if (e.target.getAttribute("role") !== "tab" || !["ArrowLeft", "ArrowRight", "Home", "End"].includes(e.key)) return;
    const keys = TABS.map((t) => t[0]);
    let i = keys.indexOf(state.tab);
    i = e.key === "Home" ? 0 : e.key === "End" ? keys.length - 1 : (i + (e.key === "ArrowRight" ? 1 : -1) + keys.length) % keys.length;
    state.tab = keys[i]; state.detail = null; render(); document.getElementById("tab-" + state.tab).focus();
    e.preventDefault();
  });
  document.getElementById("goal-btn").addEventListener("click", score);
  const snd = document.getElementById("sound-btn");
  snd.addEventListener("click", () => {
    state.sound = !state.sound;
    snd.setAttribute("aria-pressed", String(state.sound));
    snd.querySelector(".lbl").textContent = state.sound ? "Sound on" : "Sound off";
  });
  const chips = document.getElementById("chips");
  chips.innerHTML = PICKS.map((p) => `<button type="button" class="chip-btn" data-team="${p.id}" aria-pressed="${p.id === state.id}"><i style="background:${club(p.id).c}"></i>${club(p.id).n}</button>`).join("");
  chips.addEventListener("click", (e) => { const b = e.target.closest("[data-team]"); if (b) setTeam(b.dataset.team); });
  if (!reduced()) tick = setInterval(() => { if (!state.goal && state.minute < 89) { state.minute += 1; const m = root.querySelector(".min"); if (m) m.textContent = state.minute + "'"; } }, 6000);
  render();
}
