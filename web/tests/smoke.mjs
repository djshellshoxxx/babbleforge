// Headless smoke test of the built site (web/dist) in Chromium via Playwright.
//
//   node web/tests/smoke.mjs            (after `npm run build`)
// Env: BF_DIST (default web/dist), PLAYWRIGHT_BROWSERS_PATH (default /opt/pw-browsers when it
// exists), BF_CORPUS=synthetic to force the synthetic corpus.
//
// Checks: the page boots, Start (a real click = user gesture) starts audio, after 3 s the played
// output (measured in the AudioWorklet) is non-silent and near the expected digital level
// (L_ref + Strength = -26 dBFS for the default Office preset), and changing Character and Mask
// Type changes the engine statistics.
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const dist = process.env.BF_DIST || path.join(here, '..', 'dist');
if (!process.env.PLAYWRIGHT_BROWSERS_PATH && fs.existsSync('/opt/pw-browsers')) process.env.PLAYWRIGHT_BROWSERS_PATH = '/opt/pw-browsers';

let chromium;
try {
  ({ chromium } = await import('playwright'));
} catch {
  const req = createRequire(import.meta.url);
  const globalRoot = path.join(path.dirname(path.dirname(process.execPath)), 'lib', 'node_modules');
  ({ chromium } = req(path.join(globalRoot, 'playwright')));
}

const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css',
                '.wasm': 'application/wasm', '.json': 'application/json', '.flac': 'audio/flac', '.md': 'text/markdown' };
const server = http.createServer((req, res) => {
  const u = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  let f = path.join(dist, u);
  if (!f.startsWith(dist)) { res.writeHead(403).end(); return; }
  if (fs.existsSync(f) && fs.statSync(f).isDirectory()) f = path.join(f, 'index.html');
  if (!fs.existsSync(f)) { res.writeHead(404).end(); return; }
  res.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${server.address().port}/`;

const results = [];
const check = (name, ok, detail) => { results.push({ name, ok, detail }); console.log(`${ok ? 'PASS' : 'FAIL'} ${name}: ${detail}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
let failed = false;
try {
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  const q = process.env.BF_CORPUS === 'synthetic' ? '?fresh&corpus=synthetic' : '?fresh';
  await page.goto(base + q);
  await page.waitForFunction(() => window.__bf && window.__bf.ready === true, null, { timeout: 120000 });
  const corpus = await page.evaluate(() => window.__bf.corpus);
  check('boot', true, `corpus: ${corpus.kind} (${corpus.label})`);

  await page.click('#startBtn');
  await sleep(3000);
  // Average the worklet meter (played audio) over ~1 s.
  const played = await page.evaluate(async () => {
    const S = window.__bf;
    const acc = [0, 0];
    let n = 0;
    const t0 = performance.now();
    let last = null;
    while (performance.now() - t0 < 1200) {
      await new Promise((r) => setTimeout(r, 100));
      if (S.meter && S.meter !== last) { last = S.meter; acc[0] += S.meter.ms[0]; acc[1] += S.meter.ms[1]; n++; }
    }
    return { db: acc.map((e) => 10 * Math.log10(e / Math.max(n, 1) + 1e-30)), n, underruns: S.meter?.underruns ?? -1,
             rate: S.ctx.sampleRate, state: S.ctx.state };
  });
  const stats0 = await page.evaluate(() => window.__bf.stats);
  const target = -26;
  const lv = played.db;
  check('audio running', played.n > 0 && played.state === 'running', `context ${played.state} @ ${played.rate} Hz, ${played.n} meter blocks`);
  check('non-silent output', lv.every((d) => d > -60), `played RMS L/R ${lv.map((d) => d.toFixed(1)).join(' / ')} dBFS`);
  check('level near target', lv.every((d) => Math.abs(d - target) < 4),
        `expected ${target} dBFS ±4 dB (digital level), engine outputRmsDb ${stats0.level.outputRmsDb.toFixed(2)}`);
  check('no dropouts after start', played.underruns <= 1, `${played.underruns} underruns`);

  // Character -> statistics change (talker counts / plan).
  const before = { plan: stats0.planChanges, ch: await page.evaluate(() => window.__bf.eff.plan.macros.character) };
  await page.evaluate(() => window.__bf.actions.setCharacter(1.0));
  await sleep(3000);
  const stats1 = await page.evaluate(() => window.__bf.stats);
  const ch1 = await page.evaluate(() => window.__bf.eff.plan.macros.character);
  check('character changes engine statistics', stats1.planChanges > before.plan && ch1 !== before.ch &&
        (stats1.talkers.maxActive !== stats0.talkers.maxActive || stats1.mix.configuredBabbleFraction !== stats0.mix.configuredBabbleFraction ||
         Math.abs(stats1.talkers.meanActive - stats0.talkers.meanActive) > 0.05),
        `planChanges ${before.plan}->${stats1.planChanges}, character ${before.ch}->${ch1}, meanActive ${stats0.talkers.meanActive.toFixed(2)}->${stats1.talkers.meanActive.toFixed(2)}, maxActive ${stats0.talkers.maxActive}->${stats1.talkers.maxActive}, b ${stats0.mix.configuredBabbleFraction.toFixed(2)}->${stats1.mix.configuredBabbleFraction.toFixed(2)}`);

  // Mask Type -> Speech Noise (no voices).
  await page.selectOption('#strategySel', 'speech_noise');
  await sleep(3500);
  const stats2 = await page.evaluate(() => window.__bf.stats);
  check('strategy changes engine statistics', stats2.planChanges > stats1.planChanges && stats2.mix.configuredBabbleFraction < 0.01,
        `planChanges ${stats1.planChanges}->${stats2.planChanges}, configured babble fraction ${stats1.mix.configuredBabbleFraction.toFixed(2)}->${stats2.mix.configuredBabbleFraction.toFixed(2)}`);

  // Multichannel layout is rendered and folded to stereo (engine rebuild).
  await page.evaluate(() => window.__bf.actions.setLayout('ring4'));
  await sleep(2500);
  const stats3 = await page.evaluate(() => window.__bf.stats);
  check('4-channel layout (stereo downmix)', stats3.channels === 4, `engine channels ${stats3.channels}`);

  await page.click('#startBtn');  // stop
  await sleep(600);
  check('no page errors', errors.length === 0, errors.length ? errors.join(' | ') : 'none');
} catch (e) {
  check('exception', false, String(e && e.stack ? e.stack : e));
} finally {
  await browser.close();
  server.close();
}
failed = results.some((r) => !r.ok);
console.log(failed ? 'SMOKE TEST FAILED' : 'SMOKE TEST PASSED');
process.exit(failed ? 1 : 0);
