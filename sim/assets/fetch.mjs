// Downloads sample artwork for the simulator into sim/assets/.
// Run from the repo root: node sim/assets/fetch.mjs
// File names must match mock_asset_name() in sim/mock_image_provider.cpp.
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const outDir = path.join(root, 'sim', 'assets');
const sharp = createRequire(path.join(root, 'helper', 'package.json'))('sharp');

const DELAY_MS = 250;
const TIMEOUT_MS = 20000;

const readJson = (rel) => JSON.parse(fs.readFileSync(path.join(root, rel), 'utf8'));
// The `u` query parameter of a helper path such as /api/img?u=...&w=48.
const imageUrl = (helperPath) => new URL(helperPath, 'http://helper.invalid').searchParams.get('u');

const jobs = new Map(); // file name -> { url, kind }
const add = (name, url, kind) => {
  if (name && url && !jobs.has(name)) jobs.set(name, { url, kind });
};

// Crests from football-data.org.
for (const id of [57, 61, 86, 81, 65, 64, 5, 4, 524, 516]) {
  add(`crest-${id}.png`, `https://crests.football-data.org/${id}.png`, 'logo');
}

// ESPN logos for every team in the fixture, plus two extras.
const espn = (id) => `https://a.espncdn.com/i/teamlogos/soccer/500/${id}.png`;
const teams = readJson('contract/fixtures/teams-normalised.json').teams;
if (!Array.isArray(teams)) throw new Error('teams-normalised.json has no teams array');
for (const team of teams) {
  const url = team.crest ? imageUrl(team.crest) : null;
  const id = url && /\/teamlogos\/soccer\/500\/(\d+)\.png$/.exec(url)?.[1];
  if (id) add(`espn-${id}.png`, url, 'logo');
}
for (const id of [367, 388]) add(`espn-${id}.png`, espn(id), 'logo');

// Player cutouts used in docs/superpowers/specs/assets/ui-mockup-v2.html.
const cutoutBase = 'https://r2.thesportsdb.com/images/media/player/cutout/';
const mockup = fs.readFileSync(path.join(root, 'docs/superpowers/specs/assets/ui-mockup-v2.html'), 'utf8');
const cutouts = {
  Kai_Havertz: 'irkeiw1788606984.png',
  Bukayo_Saka: '7np7651788606735.png',
  Declan_Rice: 'v1ijg61788607059.png',
  Cole_Palmer: 'q6nnho1787689956.png',
};
for (const [name, file] of Object.entries(cutouts)) {
  if (!mockup.includes(cutoutBase + file)) console.warn(`note: ${file} (${name}) is no longer in the mockup`);
  add(`cutout-${name}.png`, cutoutBase + file, 'logo');
}

// News photos. Naming mirrors the mock: img-<basename of the URL path>.
const articles = readJson('contract/fixtures/news-normalised.json').articles;
if (!Array.isArray(articles)) throw new Error('news-normalised.json has no articles array');
for (const item of articles) {
  const url = item.image ? imageUrl(item.image) : null;
  if (!url) continue;
  const base = new URL(url).pathname.split('/').pop();
  add(`img-${base.replace(/[^A-Za-z0-9._-]/g, '_')}`, url, 'photo');
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function download(url) {
  const res = await fetch(url, {
    headers: { 'user-agent': 'Mozilla/5.0 (soccer-display sim asset fetch)' },
    signal: AbortSignal.timeout(TIMEOUT_MS),
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return Buffer.from(await res.arrayBuffer());
}

async function shrink(buf, kind) {
  if (kind === 'photo') {
    return sharp(buf).resize(240, 135, { fit: 'cover' }).jpeg({ quality: 80, progressive: false }).toBuffer();
  }
  return sharp(buf)
    .resize(96, 96, { fit: 'contain', background: { r: 0, g: 0, b: 0, alpha: 0 }, withoutEnlargement: true })
    .png()
    .toBuffer();
}

fs.mkdirSync(outDir, { recursive: true });
let fetched = 0;
let skipped = 0;
const failed = [];
for (const [name, { url, kind }] of jobs) {
  const dest = path.join(outDir, name);
  if (fs.existsSync(dest)) {
    skipped++;
    console.log(`skip   ${name}`);
    continue;
  }
  try {
    const out = await shrink(await download(url), kind);
    fs.writeFileSync(dest, out);
    fetched++;
    console.log(`fetch  ${name} (${out.length} bytes)`);
  } catch (err) {
    failed.push(name);
    console.log(`FAIL   ${name}: ${err.message} <- ${url}`);
  }
  await sleep(DELAY_MS);
}

console.log(`\n${fetched} fetched, ${skipped} already present, ${failed.length} failed (of ${jobs.size})`);
if (fetched + skipped === 0) {
  console.error('nothing could be fetched');
  process.exit(1);
}
