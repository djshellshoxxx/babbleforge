// Engine worker: runs the synchronous (offline) BabbleForge MaskEngine compiled to WASM.
//
// Renders ahead of playback in 2048-frame chunks (16 x 128) and posts stereo Float32Array
// chunks (transferred) straight to the AudioWorklet through a MessagePort; the worklet reports
// what it consumed and the worker keeps AHEAD frames buffered. Settings changes re-compose the
// plan (bf_set_preset -> buildScenarioPlan -> MaskEngine::setPlan at the current position), so
// the engine's own plan crossfades apply. Statistics are posted to the page at 10 Hz.
import createBabbleForge from './wasm/bfweb.mjs';

const FS = 48000;
const CHUNK = 2048;
const AHEAD = 24000;  // 0.5 s rendered ahead of playback

let M = null;
let port = null;          // to the worklet
let running = false;
let rendered = 0, consumed = 0;
let layout = null;        // {id, outputs:[{label, azimuthDeg}]}
let downmix = null;       // per channel [gL, gR]
let statsTimer = null;
let renderMs = 0, renderFrames = 0;
let lastPreset = null, seed = 1;

const call = (f, ...args) => {
  const s = M.ccall(f, 'string', args.map((a) => (typeof a === 'number' ? 'number' : 'string')), args);
  return JSON.parse(s);
};

function makeDownmix(lay) {
  const n = lay.outputs.length;
  if (n === 1) return [[Math.SQRT1_2, Math.SQRT1_2]];
  if (n === 2) return [[1, 0], [0, 1]];
  // Multichannel layouts are rendered by the engine and folded to browser stereo:
  // constant-power pan from the azimuth (+ = left; rear speakers fold onto the front arc),
  // scaled so that each browser channel keeps the per-output level.
  const k = Math.sqrt(2 / n);
  return lay.outputs.map((o) => {
    const p = Math.sin((o.azimuthDeg * Math.PI) / 180);  // +1 left .. -1 right
    const a = ((1 - p) * Math.PI) / 4;
    return [Math.cos(a) * k, Math.sin(a) * k];
  });
}

function renderChunk() {
  const t0 = performance.now();
  const ptr = M._bf_render(CHUNK);
  renderMs += performance.now() - t0;
  renderFrames += CHUNK;
  if (!ptr) return false;
  const n = downmix.length;
  const heap = new Float32Array(M.HEAPF32.buffer, ptr, n * CHUNK);
  const l = new Float32Array(CHUNK), r = new Float32Array(CHUNK);
  for (let c = 0; c < n; c++) {
    const [gl, gr] = downmix[c];
    const off = c * CHUNK;
    for (let i = 0; i < CHUNK; i++) {
      const x = heap[off + i];
      l[i] += gl * x;
      r[i] += gr * x;
    }
  }
  port.postMessage({ type: 'chunk', l, r, at: rendered }, [l.buffer, r.buffer]);
  rendered += CHUNK;
  return true;
}

function pump() {
  if (!running || !port) return;
  let guard = 0;
  while (rendered - consumed < AHEAD && guard++ < 64) if (!renderChunk()) break;
}

function postStats() {
  if (!running) return;
  const st = call('bf_stats');
  st.render = { cpu: renderFrames ? (renderMs / 1000) / (renderFrames / FS) : 0, aheadFrames: rendered - consumed,
                rendered, consumed };
  renderMs = 0; renderFrames = 0;
  st.layout = layout;
  postMessage({ type: 'stats', stats: st });
}

function start(preset, s) {
  seed = s;
  const r = call('bf_start', JSON.stringify(preset), seed, CHUNK);
  if (!r.ok) return r;
  lastPreset = preset;
  layout = r.layout;
  downmix = makeDownmix(layout);
  rendered = 0; consumed = 0;
  if (port) port.postMessage({ type: 'reset', consumed: 0 });
  running = true;
  pump();
  clearInterval(statsTimer);
  statsTimer = setInterval(postStats, 100);
  return r;
}

onmessage = async (ev) => {
  const m = ev.data;
  const reply = (data) => postMessage({ type: 'reply', id: m.id, data });
  try {
    switch (m.type) {
      case 'init': {
        M = await createBabbleForge({ locateFile: (p) => new URL('./wasm/' + p, import.meta.url).href });
        const init = call('bf_init');
        if (!init.ok) return reply(init);
        reply({ ok: true, init, catalog: call('bf_catalog') });
        break;
      }
      case 'synthetic':
        reply(call('bf_corpus_synthetic', m.speakers || 24));
        break;
      case 'corpus': {
        const b = call('bf_corpus_begin', JSON.stringify(m.corpus));
        if (!b.ok) return reply(b);
        m.audio.forEach((a, i) => {
          const p = M._malloc(a.length * 4);
          M.HEAPF32.set(a, p >> 2);
          M._bf_corpus_set_audio(i, p, a.length);
          M._free(p);
        });
        reply(call('bf_corpus_finish'));
        break;
      }
      case 'effective':
        reply(call('bf_effective', JSON.stringify(m.preset)));
        break;
      case 'port':
        port = m.port;
        port.onmessage = (e) => {
          if (e.data.type === 'consumed') {
            consumed = e.data.consumed;
            pump();
          }
        };
        reply({ ok: true });
        break;
      case 'start':
        reply(start(m.preset, m.seed));
        break;
      case 'preset': {
        if (!running) return reply({ ok: true, idle: true });
        const r = call('bf_set_preset', JSON.stringify(m.preset));
        if (r.ok && r.rebuild) {
          // Layout / correction configuration changed: new engine (the page fades around it).
          const s = start(m.preset, seed);
          return reply({ ...s, rebuilt: true });
        }
        if (r.ok) lastPreset = m.preset;
        reply(r);
        break;
      }
      case 'stop':
        running = false;
        clearInterval(statsTimer);
        M && M._bf_stop();
        reply({ ok: true });
        break;
      case 'scenarioSha':
        reply(call('bf_render_scenario_sha', m.scenario, m.block || 0));
        break;
      default:
        reply({ ok: false, error: 'unknown message ' + m.type });
    }
  } catch (e) {
    reply({ ok: false, error: String(e && e.message ? e.message : e) });
  }
};
