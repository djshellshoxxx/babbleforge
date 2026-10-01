// Native vs WebAssembly engine parity (deterministic offline render, synthetic corpus).
//
//   node web/tests/parity.mjs [--bfrender build/tools/bfrender/bfrender]
// Env: BF_BFRENDER (native bfrender path), BF_WASM_DIR (default build-web).
//
// Renders the same scenarios with the native `bfrender --synthetic-corpus 12` and with the WASM
// module (bf_render_scenario_sha: renderScenario + audioSha256, the same code path) and compares
// the SHA-256 of the float32 audio. Also checks block-size independence inside WASM. Without a
// native bfrender only the WASM checks run (and the test says so).
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const wasmDir = process.env.BF_WASM_DIR || path.join(root, 'build-web');
const argi = process.argv.indexOf('--bfrender');
const candidates = [argi > 0 ? process.argv[argi + 1] : null, process.env.BF_BFRENDER,
  path.join(root, 'build-native/tools/bfrender/bfrender'), path.join(root, 'build/tools/bfrender/bfrender')].filter(Boolean);
const bfrender = candidates.find((p) => fs.existsSync(p));

const { default: createBabbleForge } = await import(pathToFileURL(path.join(wasmDir, 'bfweb.mjs')).href);
const M = await createBabbleForge();
const call = (f, ...a) => JSON.parse(M.ccall(f, 'string', a.map((x) => (typeof x === 'number' ? 'number' : 'string')), a));
const init = call('bf_init');
if (!init.ok) throw new Error(init.error);
const synth = call('bf_corpus_synthetic', 12);  // == bfrender --synthetic-corpus 12

const scenarios = {
  smoke: JSON.parse(fs.readFileSync(path.join(root, 'tests/data/render/smoke_scenario.json'), 'utf8')),
  hybridRing4: {
    schema: 'babbleforge.scenario', schemaVersion: '1.0',
    preset: { name: 'web-parity', area: 'open_office', strategy: 'hybrid', macros: { mix: { babbleFraction: 0.5 } },
              outputs: { layout: 'ring4' } },
    seed: 4242, durationS: 6, sampleRate: 48000, outputFormat: { container: 'wav', sampleFormat: 'float32' },
    events: [{ atS: 3.0, set: { strategy: 'dense', 'macros.character': 0.8 } }],
  },
  speechNoiseMono: {
    schema: 'babbleforge.scenario', schemaVersion: '1.0',
    preset: { name: 'web-parity-ssn', area: 'small_room', strategy: 'speech_noise', outputs: { layout: 'mono' } },
    seed: 7, durationS: 4, sampleRate: 48000, outputFormat: { container: 'wav', sampleFormat: 'float32' },
  },
};

let failed = false;
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'bfparity-'));
console.log(`WASM engine ${init.version}, data set ${init.dataSetHash.slice(0, 12)}, corpus ${synth.info.version}`);
console.log(bfrender ? `native bfrender: ${bfrender}` : 'native bfrender: not found (WASM-only checks)');
for (const [name, sc] of Object.entries(scenarios)) {
  const t0 = performance.now();
  const w = call('bf_render_scenario_sha', JSON.stringify(sc), 0);
  const ms = performance.now() - t0;
  if (!w.ok) { console.log(`FAIL ${name}: wasm render: ${w.error}`); failed = true; continue; }
  const w2 = call('bf_render_scenario_sha', JSON.stringify(sc), 128);
  const blockOk = w2.ok && w2.sha256 === w.sha256;
  console.log(`${blockOk ? 'PASS' : 'FAIL'} ${name}: wasm block 480 vs 128 ${blockOk ? 'identical' : 'DIFFER'} (${w.channels} ch, ${sc.durationS} s, rms ${w.rmsDb.toFixed(3)} dBFS, ${ms.toFixed(0)} ms)`);
  failed ||= !blockOk;
  if (!bfrender) continue;
  const f = path.join(tmp, `${name}.json`);
  fs.writeFileSync(f, JSON.stringify(sc));
  const out = path.join(tmp, `${name}.wav`);
  execFileSync(bfrender, ['--scenario', f, '--synthetic-corpus', '12', '--out', out], { stdio: ['ignore', 'ignore', 'inherit'] });
  const side = JSON.parse(fs.readFileSync(path.join(tmp, `${name}.sidecar.json`), 'utf8'));
  const same = side.audioSha256 === w.sha256;
  const nRms = side.metrics?.level?.rmsDb;
  console.log(`${same ? 'PASS' : 'FAIL'} ${name}: native vs wasm ${same ? 'bit-exact' : 'DIFFER'} (sha256 ${w.sha256.slice(0, 16)}${same ? '' : ` vs native ${side.audioSha256.slice(0, 16)}, rms wasm ${w.rmsDb.toFixed(4)} native ${nRms}`})`);
  failed ||= !same;
}
fs.rmSync(tmp, { recursive: true, force: true });
console.log(failed ? 'PARITY TEST FAILED' : 'PARITY TEST PASSED');
process.exit(failed ? 1 : 0);
