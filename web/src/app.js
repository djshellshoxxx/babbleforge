// BabbleForge web demo: UI (pages mirror docs/GUI.md), preset model, audio graph.
//
// The UI edits a preset document exactly like the desktop AppState (area -> factory preset,
// strategy -> strategy macros reset, macros / talkers / stationary / spectrum / spatial /
// outputs fields). Every edit is composed in the engine worker (bf_effective: compose +
// validate + buildPlan) to display effective values, and pushed to the running engine after a
// 150 ms debounce (bf_set_preset -> MaskEngine::setPlan at the current position).

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const fmt = (v, d = 1) => (v === undefined || v === null || !isFinite(v) ? '—' : Number(v).toFixed(d));
const dbStr = (v, unit = 'dBFS') => (v === undefined || v === null || v <= -199 ? '—' : `${fmt(v)} ${unit}`);

const SIMPLE_STRATEGIES = ['balanced', 'natural', 'dense', 'speech_noise', 'multi_voice', 'hybrid'];
const SIMPLE_NAMES = { dense: 'Maximum Density', speech_noise: 'Speech Noise' };
const STRATEGY_DESC = {
  balanced: 'General-purpose speech masking with a mix of voices and stable background masking.',
  natural: 'Sounds more like distant conversation.',
  dense: 'Denser masking with fewer quiet gaps.',
  speech_noise: 'Steady speech-shaped noise without recognizable voices.',
  multi_voice: 'Uses several distinct voices as the main masker.',
  hybrid: 'Combines multi-speaker babble and steady masking.',
  research: 'Laboratory / research masker (Advanced).',
};
const TARGET_NAMES = {
  ltass_universal: 'Universal LTASS', 'slope_-5': 'Privacy -5 dB/oct', 'slope_-7': 'Privacy -7 dB/oct',
  'slope_-9': 'Privacy -9 dB/oct', pink: 'Pink noise', flat: 'White (flat)', custom: 'Custom',
};
const EQ_HZ = [125, 250, 500, 1000, 2000, 4000, 8000];
const EQ_LABELS = ['125', '250', '500', '1k', '2k', '4k', '8k'];
const VARIETY = ['low', 'balanced', 'high'];
const VARIATION = ['low', 'medium', 'high'];

// ------------------------------------------------------------------ worker RPC
const worker = new Worker(new URL('./engine-worker.js', import.meta.url), { type: 'module' });
let rpcId = 0;
const pending = new Map();
const rpc = (type, data = {}, transfer = []) =>
  new Promise((resolve) => {
    const id = ++rpcId;
    pending.set(id, resolve);
    worker.postMessage({ type, id, ...data }, transfer);
  });
worker.onmessage = (ev) => {
  const m = ev.data;
  if (m.type === 'reply') {
    const r = pending.get(m.id);
    pending.delete(m.id);
    r && r(m.data);
  } else if (m.type === 'stats') onStats(m.stats);
};

// ------------------------------------------------------------------ state
const S = {
  catalog: null,
  areas: {}, strategies: {}, targets: {},
  preset: null,
  eff: null,           // bf_effective result
  advanced: false,
  running: false,
  startedAt: 0,
  corpus: null,        // {kind, label}
  stats: null,
  meter: null,
  ctx: null, node: null, gain: null,
  seed: Math.floor(Math.random() * 2 ** 31) + 1,
  specView: 'third',
  voiceHistory: [],
};
window.__bf = S;  // for the smoke test / console

function factoryPreset(area, strategy) {
  const a = S.areas[area];
  return {
    schema: 'babbleforge.preset', schemaVersion: '1.0', name: 'Web demo',
    basedOn: { area, strategy, factoryPresetVersion: '1.0.0' },
    area, strategy,
    macros: { strengthDb: a?.level?.defaultStrengthDb ?? 0 },
    outputs: { layout: 'stereo', limiter: { enabled: true, ceilingDbtp: S.catalog.engineDefaults.limiter.ceilingDbtp } },
  };
}

const P = () => S.preset;
const sub = (o, k) => (o[k] ??= {});

// ------------------------------------------------------------------ effective values
let effSeq = 0;
async function recompute() {
  const seq = ++effSeq;
  const r = await rpc('effective', { preset: S.preset });
  if (seq !== effSeq) return;
  if (r.ok) S.eff = r;
  render();
}
const plan = () => S.eff?.plan ?? {};
const eff = () => S.eff?.effective ?? {};

function edit(fn) {
  fn(S.preset);
  recompute();
  schedulePush();
}

let pushTimer = 0;
function schedulePush() {
  clearTimeout(pushTimer);
  pushTimer = setTimeout(pushPreset, 150);
}
async function pushPreset() {
  if (!S.running) return;
  const r = await rpc('preset', { preset: structuredClone(S.preset) });
  if (!r.ok) console.warn('preset push failed', r.error);
}

// ------------------------------------------------------------------ getters (AppState equivalents)
const G = {
  strength: () => P().macros?.strengthDb ?? eff().strengthDb ?? 0,
  character: () => P().macros?.character ?? plan().macros?.character ?? eff().character ?? 0.5,
  voiceAmount: () => P().macros?.voiceAmount ?? eff().voiceAmount ?? 0.5,
  voiceVariety: () => P().macros?.voiceDiversity ?? eff().voiceDiversity ?? 'high',
  cvr: () => P().macros?.clearVoiceReduction ?? eff().clearVoiceReduction ?? 1,
  mix: () => P().macros?.mix?.babbleFraction ?? plan().mix?.babbleFraction ?? eff().babbleFraction ?? 0.7,
  talkers: () => {
    const t = S.eff?.talkers ?? { pool: 14, average: 6.5, minimum: 4, maximum: 9 };
    const pt = P().talkers ?? {};
    return { pool: pt.pool ?? t.pool, average: pt.meanActive ?? t.average, minimum: pt.minActive ?? t.minimum,
             maximum: pt.maxActive ?? t.maximum };
  },
  timing: () => {
    const t = S.eff?.talkers ?? {};
    const pt = P().talkers ?? {};
    return { maxGap: pt.maxInternalGapMs ?? t.maxInternalGapMs ?? 250, segMin: pt.segmentMinS ?? t.segmentMinS ?? 1.5,
             segMax: pt.segmentMaxS ?? t.segmentMaxS ?? 20, gainVar: pt.gainVariationDb ?? t.gainVariationDb ?? 2,
             fade: pt.fadeInMs ?? t.fadeMs ?? 150 };
  },
  statEnabled: () => P().stationary?.enabled ?? eff().stationaryEnabled ?? true,
  seedMode: () => P().stationary?.seedMode ?? eff().seedMode ?? 'session',
  target: () => P().spectrum?.target ?? eff().spectrumTarget ?? 'ltass_universal',
  corrEnabled: () => P().spectrum?.correction?.enabled ?? eff().correctionEnabled ?? true,
  corrSpeed: () => P().spectrum?.correction?.speed ?? eff().correctionSpeed ?? 'normal',
  layout: () => P().outputs?.layout ?? 'stereo',
  spread: () => P().spatial?.spread ?? eff().spread ?? 0.5,
  variation: () => P().spatial?.speakerVariation ?? eff().speakerVariation ?? 'medium',
  limEnabled: () => P().outputs?.limiter?.enabled ?? true,
  ceiling: () => P().outputs?.limiter?.ceilingDbtp ?? -1,
};

function ltassAt(hz) {
  const t = S.catalog.ltassUniversal;
  if (!t) return 0;
  const i = t.bandCentersHz.findIndex((c) => Math.abs(c - hz) < 0.03 * hz);
  return i >= 0 ? t.levelsDb[i] : 0;
}
function customEq() {
  const c = P().spectrum?.custom;
  return EQ_HZ.map((hz) => {
    if (!c) return 0;
    const i = c.bandCentersHz.findIndex((x) => Math.abs(x - hz) < 1);
    return i >= 0 ? c.levelsDb[i] - ltassAt(hz) : 0;
  });
}
function setCustomEq(eq) {
  const s = sub(P(), 'spectrum');
  s.custom = { id: 'custom', displayName: 'Custom', bandCentersHz: EQ_HZ.slice(), levelsDb: EQ_HZ.map((hz, b) => ltassAt(hz) + eq[b]) };
  s.target = 'custom';
}

// enforceTalkerCounts (app/gui/model/AppState.cpp)
function enforceTalkers(c, changed) {
  c.pool = clamp(Math.round(c.pool), 2, 64);
  c.average = clamp(c.average, 1, 32);
  c.minimum = clamp(Math.round(c.minimum), 0, 32);
  c.maximum = clamp(Math.round(c.maximum), 1, 32);
  if (changed === 'minimum') {
    if (c.average < c.minimum) c.average = c.minimum;
    if (c.maximum < Math.ceil(c.average)) c.maximum = Math.ceil(c.average);
  } else if (changed === 'maximum') {
    if (c.average > c.maximum) c.average = c.maximum;
    if (c.minimum > Math.floor(c.average)) c.minimum = Math.floor(c.average);
  } else {
    if (c.minimum > c.average) c.minimum = Math.floor(c.average);
    if (c.maximum < c.average) c.maximum = Math.ceil(c.average);
  }
  if (changed === 'pool') {
    if (c.maximum > c.pool) c.maximum = c.pool;
    if (c.average > c.maximum) c.average = c.maximum;
    if (c.minimum > c.average) c.minimum = Math.floor(c.average);
  } else if (c.pool < c.maximum) c.pool = Math.min(64, c.maximum);
  return c;
}

// ------------------------------------------------------------------ actions
const A = {
  setArea(id) {
    if (!S.areas[id] || id === P().area) return;
    const p = factoryPreset(id, P().strategy);
    p.macros.strengthDb = P().macros?.strengthDb ?? p.macros.strengthDb;
    p.outputs = P().outputs;
    S.preset = p;
    recompute();
    schedulePush();
  },
  setStrategy(id) {
    if (!S.strategies[id] || id === P().strategy) return;
    edit((p) => {
      p.strategy = id;
      if (p.basedOn) p.basedOn.strategy = id;
      if (p.macros) { delete p.macros.character; delete p.macros.mix; }
      if (p.talkers) delete p.talkers.multiVoiceK;
    });
  },
  setStrength: (db) => edit((p) => (sub(p, 'macros').strengthDb = clamp(db, -40, 12))),
  setCharacter: (c) => edit((p) => (sub(p, 'macros').character = clamp(c, 0, 1))),
  setVoiceAmount: (v) => edit((p) => (sub(p, 'macros').voiceAmount = clamp(v, 0, 1))),
  setVoiceVariety: (v) => edit((p) => (sub(p, 'macros').voiceDiversity = v)),
  setCvr: (r) => edit((p) => (sub(p, 'macros').clearVoiceReduction = clamp(r, 0, 1))),
  setMix: (b) => edit((p) => (sub(sub(p, 'macros'), 'mix').babbleFraction = clamp(b, 0, 1))),
  setTalker(field, v) {
    const c = G.talkers();
    c[field] = field === 'average' ? Math.round(v * 10) / 10 : Math.round(v);
    enforceTalkers(c, field);
    edit((p) => Object.assign(sub(p, 'talkers'), { pool: c.pool, meanActive: c.average, minActive: c.minimum, maxActive: c.maximum }));
  },
  setTiming(field, v) {
    const t = G.timing();
    edit((p) => {
      const pt = sub(p, 'talkers');
      if (field === 'maxGap') pt.maxInternalGapMs = clamp(v, 20, 2000);
      if (field === 'segMin') { pt.segmentMinS = clamp(v, 0.5, 30); pt.segmentMaxS = Math.max(pt.segmentMinS, pt.segmentMaxS ?? t.segMax); }
      if (field === 'segMax') { pt.segmentMaxS = clamp(v, 1, 60); pt.segmentMinS = Math.min(pt.segmentMaxS, pt.segmentMinS ?? t.segMin); }
      if (field === 'gainVar') pt.gainVariationDb = clamp(v, 0, 6);
      if (field === 'fade') { pt.fadeInMs = clamp(v, 10, 1000); pt.fadeOutMs = pt.fadeInMs; }
    });
  },
  setStatEnabled: (on) => edit((p) => (sub(p, 'stationary').enabled = on)),
  setSeedMode: (m) => edit((p) => (sub(p, 'stationary').seedMode = m)),
  setTarget(id) {
    const eq = customEq();
    edit((p) => {
      if (id === 'custom') setCustomEq(eq);
      else sub(p, 'spectrum').target = id;
    });
  },
  setEqBand(b, db) {
    const eq = customEq();
    eq[b] = clamp(db, -12, 12);
    edit(() => setCustomEq(eq));
  },
  setCorrEnabled: (on) => edit((p) => (sub(sub(p, 'spectrum'), 'correction').enabled = on)),
  setCorrSpeed: (s) => edit((p) => (sub(sub(p, 'spectrum'), 'correction').speed = s)),
  setLayout: (l) => edit((p) => { sub(p, 'outputs').layout = l; delete p.outputs.channels; }),
  setCoverage: (c) => A.setLayout(c === 'stereo' ? 'stereo' : 'ring4'),
  setSpread: (v) => edit((p) => (sub(p, 'spatial').spread = clamp(v, 0, 1))),
  setVariation: (v) => edit((p) => (sub(p, 'spatial').speakerVariation = v)),
  setLimEnabled: (on) => edit((p) => (sub(sub(p, 'outputs'), 'limiter').enabled = on)),
  setCeiling: (db) => edit((p) => (sub(sub(p, 'outputs'), 'limiter').ceilingDbtp = clamp(db, -6, -0.1))),
  quick(id) { A.setStrategy(id); },
};
S.actions = A;

// ------------------------------------------------------------------ sliders
const sliders = {};
function strengthLabel(db) {
  const labels = S.catalog.engineDefaults.strength.simple.labels;
  let best = 'Normal', d = 1e9;
  for (const [k, v] of Object.entries(labels)) if (Math.abs(v - db) < d) { d = Math.abs(v - db); best = k === 'VeryStrong' ? 'Very Strong' : k; }
  return best;
}
const SL = {
  strength: { name: 'Masking Strength', ends: ['Quiet', 'Strong'], step: 0.5,
    range: () => (S.advanced ? [S.catalog.engineDefaults.strength.advanced.minDb, S.catalog.engineDefaults.strength.advanced.maxDb]
                             : [S.catalog.engineDefaults.strength.simple.minDb, S.catalog.engineDefaults.strength.simple.maxDb]),
    get: () => G.strength(), set: (v) => A.setStrength(v),
    show: (v) => (S.advanced ? `${v > 0 ? '+' : ''}${fmt(v)} dB` : strengthLabel(v)) },
  character: { name: 'Character', ends: ['Natural', 'Dense'], range: () => [0, 1], step: 0.01, get: () => G.character(), set: (v) => A.setCharacter(v),
    show: (v) => (S.advanced ? fmt(v, 2) : '') },
  voiceAmount: { name: 'Voice Amount', ends: ['Few', 'Many'], range: () => [0, 1], step: 0.01, get: () => G.voiceAmount(), set: (v) => A.setVoiceAmount(v),
    show: (v) => (S.advanced ? fmt(v, 2) : '') },
  voiceVariety: { name: 'Voice Variety', ends: ['Low', 'High'], range: () => [0, 2], step: 1, get: () => Math.max(0, VARIETY.indexOf(G.voiceVariety())),
    set: (v) => A.setVoiceVariety(VARIETY[v]), show: (v) => ['Low', 'Balanced', 'High'][v] },
  cvr: { name: 'Clear Voice Reduction', ends: ['Low', 'High'], range: () => [0, 1], step: 0.01, get: () => G.cvr(), set: (v) => A.setCvr(v),
    show: (v) => (S.advanced ? fmt(v, 2) : '') },
  mix: { name: 'Mask Mix', ends: ['Steady', 'Voices'], range: () => [0, 1], step: 0.01, get: () => G.mix(), set: (v) => A.setMix(v),
    show: (v) => (S.advanced ? `${Math.round(v * 100)}% voices` : '') },
  pool: { name: 'Talker Pool', range: () => [2, 64], step: 1, get: () => G.talkers().pool, set: (v) => A.setTalker('pool', v), show: (v) => fmt(v, 0) },
  average: { name: 'Average Active', range: () => [1, 32], step: 0.1, get: () => G.talkers().average, set: (v) => A.setTalker('average', v), show: (v) => fmt(v, 1) },
  minimum: { name: 'Minimum Active', range: () => [0, 32], step: 1, get: () => G.talkers().minimum, set: (v) => A.setTalker('minimum', v), show: (v) => fmt(v, 0) },
  maximum: { name: 'Maximum Active', range: () => [1, 32], step: 1, get: () => G.talkers().maximum, set: (v) => A.setTalker('maximum', v), show: (v) => fmt(v, 0) },
  maxGap: { name: 'Maximum Internal Gap', range: () => [20, 2000], step: 10, get: () => G.timing().maxGap, set: (v) => A.setTiming('maxGap', v), show: (v) => `${fmt(v, 0)} ms` },
  segMin: { name: 'Minimum Segment Length', range: () => [0.5, 30], step: 0.5, get: () => G.timing().segMin, set: (v) => A.setTiming('segMin', v), show: (v) => `${fmt(v)} s` },
  segMax: { name: 'Maximum Segment Length', range: () => [1, 60], step: 0.5, get: () => G.timing().segMax, set: (v) => A.setTiming('segMax', v), show: (v) => `${fmt(v)} s` },
  gainVar: { name: 'Talker Gain Variation', range: () => [0, 6], step: 0.1, get: () => G.timing().gainVar, set: (v) => A.setTiming('gainVar', v), show: (v) => `±${fmt(v)} dB` },
  fade: { name: 'Fade Time', range: () => [10, 1000], step: 10, get: () => G.timing().fade, set: (v) => A.setTiming('fade', v), show: (v) => `${fmt(v, 0)} ms` },
  statContribution: { name: 'Energy Contribution', ends: ['0%', '100%'], range: () => [0, 1], step: 0.01, get: () => 1 - G.mix(), set: (v) => A.setMix(1 - v),
    show: (v) => `${Math.round(v * 100)}%` },
  spread: { name: 'Spatial Spread', ends: ['Narrow', 'Wide'], range: () => [0, 1], step: 0.01, get: () => G.spread(), set: (v) => A.setSpread(v), show: (v) => fmt(v, 2) },
  variation: { name: 'Speaker Variation', ends: ['Low', 'High'], range: () => [0, 2], step: 1, get: () => Math.max(0, VARIATION.indexOf(G.variation())),
    set: (v) => A.setVariation(VARIATION[v]), show: (v) => ['Low', 'Medium', 'High'][v] },
  ceiling: { name: 'Ceiling', range: () => [-6, -0.1], step: 0.1, get: () => G.ceiling(), set: (v) => A.setCeiling(v), show: (v) => `${fmt(v)} dBTP` },
};
SL.strength2 = { ...SL.strength, name: 'Master Level (Strength)' };

function buildSliders() {
  for (const el of $$('.slider[data-ctl]')) {
    const key = el.dataset.ctl;
    const def = SL[key];
    if (!def) continue;
    el.innerHTML = `<div class="head"><span class="name">${def.name}</span><span class="val"></span></div>
      <input type="range" aria-label="${def.name}">${def.ends ? `<div class="ends"><span>${def.ends[0]}</span><span>${def.ends[1]}</span></div>` : ''}`;
    const input = $('input', el);
    input.step = def.step;
    input.addEventListener('input', () => {
      const v = Number(input.value);
      $('.val', el).textContent = def.show(v);
      def.set(v);
    });
    (sliders[key] ??= []).push({ el, input, def });
  }
}
function renderSliders() {
  for (const list of Object.values(sliders))
    for (const { el, input, def } of list) {
      if (document.activeElement === input) continue;
      const [lo, hi] = def.range();
      input.min = lo; input.max = hi;
      const v = clamp(def.get(), lo, hi);
      input.value = v;
      $('.val', el).textContent = def.show(v);
    }
}

// ------------------------------------------------------------------ render
function fillSelect(sel, items, value) {
  const html = items.map(([v, t]) => `<option value="${v}">${t}</option>`).join('');
  if (sel.dataset.html !== html) { sel.innerHTML = html; sel.dataset.html = html; }
  sel.value = value;
}

function render() {
  const p = P();
  const area = S.areas[p.area];
  const strat = S.strategies[p.strategy];
  document.body.classList.toggle('advanced', S.advanced);
  $('#modeSimple').classList.toggle('on', !S.advanced);
  $('#modeAdvanced').classList.toggle('on', S.advanced);
  $('#runArea').textContent = (area?.displayName ?? p.area).toUpperCase();
  const sname = SIMPLE_NAMES[p.strategy] ?? strat?.displayName ?? p.strategy;
  $('#runStrategy').textContent = `${sname} Masking`;

  const areaItems = Object.values(S.areas).map((a) => [a.id, a.displayName]);
  fillSelect($('#areaSel'), areaItems, p.area);
  fillSelect($('#areaSel2'), areaItems, p.area);
  const ids = S.advanced ? Object.keys(S.strategies) : SIMPLE_STRATEGIES.filter((id) => S.strategies[id]);
  if (!ids.includes(p.strategy)) ids.push(p.strategy);
  const stratItems = ids.map((id) => [id, SIMPLE_NAMES[id] ?? S.strategies[id].displayName]);
  fillSelect($('#strategySel'), stratItems, p.strategy);
  fillSelect($('#strategySel2'), stratItems, p.strategy);
  const desc = STRATEGY_DESC[p.strategy] ?? strat?.descriptionSimple ?? '';
  $('#strategyDesc').textContent = desc;
  $('#strategyDesc2').textContent = desc;
  $('#areaDesc').textContent = area?.description ?? '';
  $$('.qcard').forEach((b) => b.classList.toggle('on', b.dataset.quick === p.strategy));

  // Mask Mix only for Hybrid in Simple mode (GUI section 13); Advanced shows it for blends.
  const hybridish = p.strategy === 'hybrid' || (S.advanced && plan().babbleEnabled !== false);
  $$('.mix-ctl').forEach((e) => e.classList.toggle('hidden', !hybridish));

  // coverage / layout
  const lay = G.layout();
  const multi = !['stereo', 'mono'].includes(lay);
  for (const n of ['coverage', 'coverage2']) $$(`input[name=${n}]`).forEach((r) => (r.checked = r.value === (multi ? 'multi' : 'stereo')));
  $$('input[name=layout]').forEach((r) => (r.checked = r.value === lay));
  $$('input[name=speakers]').forEach((r) => (r.checked = r.value === (lay === 'ring6' || lay === 'ring8' ? 'ring8' : multi ? 'ring4' : 'stereo')));
  $$('.multi-note').forEach((e) => e.classList.toggle('hidden', !multi));

  // advisories (area data: "when": "outputs < N")
  const nOut = S.eff?.layout?.outputs?.length ?? 2;
  const adv = (area?.advisories ?? []).filter((a) => {
    const m = /outputs\s*<\s*(\d+)/.exec(a.when ?? '');
    return m && nOut < Number(m[1]);
  }).map((a) => a.text);
  if (area?.outputs?.minRecommended && nOut < area.outputs.minRecommended && !adv.length)
    adv.push(`${area.displayName} usually needs ${area.outputs.minRecommended}+ speakers for even coverage.`);
  $('#advisories').textContent = adv.join(' ');

  // stationary / spectrum
  $('#statEnabled').checked = G.statEnabled();
  $$('.stat-dep').forEach((e) => e.classList.toggle('disabled', !G.statEnabled()));
  $('#seedMode').value = G.seedMode();
  const tItems = ['ltass_universal', 'slope_-5', 'slope_-7', 'slope_-9', 'pink', 'flat']
    .filter((id) => S.targets[id]).map((id) => [id, TARGET_NAMES[id] ?? S.targets[id].displayName]);
  tItems.push(['custom', 'Custom']);
  fillSelect($('#spectrumSel'), tItems, G.target());
  $('#eqBands').classList.toggle('hidden', G.target() !== 'custom');
  const eq = customEq();
  $$('#eqBands input').forEach((inp, b) => { if (document.activeElement !== inp) inp.value = eq[b]; $$('#eqBands .bv')[b].textContent = `${eq[b] > 0 ? '+' : ''}${fmt(eq[b])}`; });
  $('#corrEnabled').checked = G.corrEnabled();
  $('#corrSpeed').value = G.corrSpeed();
  $('#limEnabled').checked = G.limEnabled();
  $('#stLimiter').classList.toggle('hidden', G.limEnabled());

  const t = G.talkers();
  $('#densityText').textContent = S.eff ? `Plan: ${fmt(t.average)} average simultaneous talkers (${t.minimum}–${t.maximum}), pool ${t.pool}.` : '';
  renderSliders();
  drawTargetPlot();
}

// ------------------------------------------------------------------ plots
function targetLevels(id) {
  if (id === 'custom') {
    const c = P().spectrum?.custom;
    return c ? { hz: c.bandCentersHz, db: c.levelsDb } : null;
  }
  const t = S.targets[id];
  return t ? { hz: t.bandCentersHz, db: t.levelsDb } : null;
}
function plotAxes(ctx, w, h, fLo, fHi, dbLo, dbHi) {
  const x = (f) => 36 + ((Math.log10(f) - Math.log10(fLo)) / (Math.log10(fHi) - Math.log10(fLo))) * (w - 46);
  const y = (d) => 10 + ((dbHi - d) / (dbHi - dbLo)) * (h - 30);
  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = '#2a323c'; ctx.fillStyle = '#8b949e'; ctx.font = '11px system-ui'; ctx.lineWidth = 1;
  for (const f of [63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]) {
    if (f < fLo || f > fHi) continue;
    ctx.beginPath(); ctx.moveTo(x(f), 10); ctx.lineTo(x(f), h - 20); ctx.stroke();
    ctx.fillText(f >= 1000 ? `${f / 1000}k` : `${f}`, x(f) - 8, h - 6);
  }
  for (let d = Math.ceil(dbLo / 10) * 10; d <= dbHi; d += 10) {
    ctx.beginPath(); ctx.moveTo(36, y(d)); ctx.lineTo(w - 10, y(d)); ctx.stroke();
    ctx.fillText(`${d}`, 4, y(d) + 4);
  }
  return { x, y };
}
function line(ctx, pts, color, width = 2) {
  ctx.strokeStyle = color; ctx.lineWidth = width; ctx.beginPath();
  pts.forEach(([px, py], i) => (i ? ctx.lineTo(px, py) : ctx.moveTo(px, py)));
  ctx.stroke();
}
function drawTargetPlot() {
  const c = $('#spectrumCanvas');
  if (!c || !c.offsetParent) return;
  const ctx = c.getContext('2d');
  const { x, y } = plotAxes(ctx, c.width, c.height, 50, 16000, -40, 15);
  const ref = targetLevels('ltass_universal');
  if (ref && G.target() !== 'ltass_universal') line(ctx, ref.hz.map((f, i) => [x(f), y(ref.db[i])]), '#3b4450', 1.5);
  const t = targetLevels(G.target());
  if (t) line(ctx, t.hz.map((f, i) => [x(f), y(t.db[i])]), '#d2a8ff', 2.5);
  ctx.fillStyle = '#d2a8ff'; ctx.fillText('target (dB re 1 kHz, 1/3 octave)', 44, 22);
}

function drawAnalysisSpectrum(v) {
  const c = $('#anSpecCanvas');
  if (!c.offsetParent) return;
  const ctx = c.getContext('2d');
  const hz = S.catalog.thirdOctHz;
  if (S.specView === 'fft' && v?.haveFft) {
    const fh = S.catalog.fftViewHz;
    const db = v.fftDb;
    const mx = Math.max(...db);
    const { x, y } = plotAxes(ctx, c.width, c.height, 31.25, 16000, mx - 60, mx + 5);
    if (v.haveSpectrum) {
      const rmx = Math.max(...v.referenceDb);
      line(ctx, hz.map((f, i) => [x(f), y(v.referenceDb[i] - rmx + mx)]), '#d2a8ff', 1.5);
    }
    line(ctx, fh.map((f, i) => [x(f), y(db[i])]), '#58a6ff', 1.5);
    return;
  }
  if (!v?.haveSpectrum) { plotAxes(ctx, c.width, c.height, 50, 16000, -40, 10); ctx.fillText('collecting spectrum…', 50, 30); return; }
  let rf = hz, ref = v.referenceDb, mea = v.measuredDb;
  if (S.specView === 'oct') {
    const oc = [63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
    const comb = (a) => oc.map((f) => {
      const idx = hz.map((h, i) => [h, i]).filter(([h]) => h >= f / 1.42 && h < f * 1.42).map(([, i]) => i);
      const e = idx.reduce((s, i) => s + 10 ** (a[i] / 10), 0);
      return 10 * Math.log10(e || 1e-20);
    });
    rf = oc; ref = comb(ref); mea = comb(mea);
  }
  // Shape comparison: the measured long-term spectrum is aligned to the target by the mean
  // difference over 125 Hz - 8 kHz (the target is relative, 1 kHz = 0 dB).
  const band = rf.map((f, i) => i).filter((i) => rf[i] >= 125 && rf[i] <= 8000);
  const off = band.reduce((a, i) => a + (mea[i] - ref[i]), 0) / Math.max(1, band.length);
  mea = mea.map((d) => d - off);
  const all = [...ref, ...mea].filter((d) => d > -150);
  const mx = Math.max(...all);
  const { x, y } = plotAxes(ctx, c.width, c.height, 50, 16000, mx - 45, mx + 5);
  line(ctx, rf.map((f, i) => [x(f), y(ref[i])]), '#d2a8ff', 2);
  line(ctx, rf.map((f, i) => [x(f), y(mea[i])]), '#58a6ff', 2);
  ctx.fillStyle = '#d2a8ff'; ctx.fillText('target', 44, 22); ctx.fillStyle = '#58a6ff'; ctx.fillText('actual', 90, 22);
}

function drawMap(v, lay) {
  const c = $('#mapCanvas');
  if (!c.offsetParent) return;
  const ctx = c.getContext('2d');
  const w = c.width, h = c.height, cx = w / 2, cy = h / 2, R = w * 0.38;
  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = '#2a323c'; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.arc(cx, cy, R, 0, Math.PI * 2); ctx.stroke();
  ctx.fillStyle = '#8b949e'; ctx.font = '11px system-ui'; ctx.fillText('front', cx - 14, 14);
  const pos = (az, r = R) => [cx - Math.sin((az * Math.PI) / 180) * r, cy - Math.cos((az * Math.PI) / 180) * r];
  for (const o of lay?.outputs ?? []) {
    const [px, py] = pos(o.azimuthDeg);
    ctx.fillStyle = '#d29922'; ctx.fillRect(px - 6, py - 6, 12, 12);
  }
  ctx.beginPath(); ctx.arc(cx, cy, 5, 0, Math.PI * 2); ctx.fillStyle = '#8b949e'; ctx.fill();
  for (const d of v?.talkerDots ?? []) {
    const [active, az, pan, gain, home] = d;
    let px, py;
    if (lay?.outputs?.length === 2) { px = cx + pan * R * 0.5; py = cy - R * 0.55; }
    else if (home >= 0 && lay?.outputs?.[home]) { [px, py] = pos(lay.outputs[home].azimuthDeg, R * 0.75); }
    else[px, py] = pos(az, R * 0.7);
    ctx.beginPath(); ctx.arc(px, py, 4 + 6 * gain, 0, Math.PI * 2);
    ctx.fillStyle = active ? 'rgba(88,166,255,.9)' : 'rgba(88,166,255,.2)'; ctx.fill();
  }
}

function drawVoices() {
  const c = $('#voiceCanvas');
  if (!c.offsetParent) return;
  const ctx = c.getContext('2d');
  const hist = S.voiceHistory;
  ctx.clearRect(0, 0, c.width, c.height);
  // Slots that have carried a talker (the engine reports all 64 voice slots).
  let used = G.talkers().maximum || 1;
  for (const h of hist) h.forEach((on, i) => { if (on && i + 1 > used) used = i + 1; });
  const nSlots = Math.max(1, used);
  const rowH = (c.height - 4) / nSlots;
  const colW = c.width / 150;
  hist.forEach((slots, t) => slots.forEach((on, s) => {
    if (!on) return;
    ctx.fillStyle = '#58a6ff';
    ctx.fillRect(t * colW, 2 + s * rowH, Math.ceil(colW), rowH - 2);
  }));
}

// ------------------------------------------------------------------ stats -> UI
function kv(el, rows) {
  el.innerHTML = rows.map(([k, v]) => `<span class="k">${k}</span><span class="v">${v}</span>`).join('');
}
// Fraction of recent stats ticks (last 15 s) with at least one talker placed.
function occupancy() {
  const h = S.voiceHistory;
  return h.length ? h.filter((x) => x.some(Boolean)).length / h.length : 0;
}
function onStats(st) {
  S.stats = st;
  const v = st.view ?? {};
  const t = st.talkers ?? {};
  const activeNow = (v.slotActive ?? []).filter(Boolean).length;
  const target = G.talkers().maximum || 1;
  $('#activityBar').style.width = `${clamp((st.talkers?.babbleActive ? activeNow / target : (1 - G.mix())) * 100, 0, 100)}%`;
  $('#voicesActive').textContent = t.babbleActive ? `${activeNow} voices active` : 'Steady masking (no voices)';
  const healthy = !(st.reliability?.sourceErrors) && !(st.reliability?.babbleUnavailable);
  $('#healthText').textContent = healthy ? 'Output healthy' : 'Degraded';

  // meters (per browser channel; engine channels when stereo/mono)
  const rms = v.rmsFastChDb ?? [], tp = v.truePeakChDb ?? [];
  const lay = st.layout;
  const names = (lay?.outputs ?? []).map((o, i) => o.label || `Speaker ${i + 1}`);
  const mEl = $('#chMeters');
  if (mEl.childElementCount !== rms.length) {
    mEl.innerHTML = rms.map((_, i) => `<div class="meter"><span>${names[i] ?? `Ch ${i + 1}`}</span><div class="m"><i></i><b></b></div><span class="v"></span></div>`).join('');
  }
  $$('.meter', mEl).forEach((m, i) => {
    const pct = clamp((rms[i] + 60) / 60, 0, 1) * 100;
    $('i', m).style.width = `${pct}%`;
    $('b', m).style.left = `${clamp((tp[i] + 60) / 60, 0, 1) * 100}%`;
    $('.v', m).textContent = S.advanced ? `${fmt(rms[i])} dB` : '';
  });
  const all = v.rmsFastAllDb ?? -200;
  const tgt = -26 + G.strength();
  const word = all < -150 ? '—' : all < tgt - 10 ? 'LOW' : all > Math.max(tgt + 6, -8) ? 'HIGH' : 'GOOD';
  $('#levelWord').textContent = word;
  $('#levelWord').style.color = word === 'GOOD' ? 'var(--accent2)' : word === '—' ? '' : 'var(--warn)';
  $('#outStatus').textContent = `Status: ${healthy ? 'Healthy' : 'Degraded'}`;

  const lim = st.limiter ?? {};
  kv($('#techMeters'), [
    ['RMS (300 ms)', dbStr(all)], ['RMS 10 s', dbStr(v.rms10sDb)], ['LUFS-S', dbStr(st.level?.lufsS, 'LUFS')],
    ['LUFS-I', dbStr(st.level?.lufsI, 'LUFS')], ['True Peak (10 s)', dbStr(v.truePeak10sDb, 'dBTP')],
    ['Limiter GR max', lim.enabled ? `${fmt(lim.grMaxDb)} dB` : 'off'], ['Target RMS', `${fmt(tgt)} dBFS`],
  ]);
  const meter = S.meter;
  const ctxRate = S.ctx?.sampleRate ?? 0;
  kv($('#audioInfo'), [
    ['Engine sample rate', '48000 Hz'], ['Browser output rate', ctxRate ? `${ctxRate} Hz${ctxRate !== 48000 ? ' (resampled)' : ''}` : '—'],
    ['Engine channels', `${st.channels}${st.channels > 2 ? ' (folded to stereo)' : ''}`],
    ['Render ahead', `${fmt((st.render?.aheadFrames ?? 0) / 48, 0)} ms`],
    ['Engine CPU (worker)', `${fmt((st.render?.cpu ?? 0) * 100, 0)} %`],
    ['Output latency', S.ctx ? `${fmt(((S.ctx.baseLatency || 0) + (S.ctx.outputLatency || 0)) * 1000, 0)} ms + render-ahead` : '—'],
    ['Audio dropouts', `${meter?.underruns ?? 0}`], ['Plan changes', `${st.planChanges ?? 0}`],
  ]);

  // ANALYSIS
  S.voiceHistory.push((v.slotActive ?? []).slice());
  if (S.voiceHistory.length > 150) S.voiceHistory.shift();
  const dens = ['Low', 'Medium', 'High'][v.temporalDensity] ?? '—';
  kv($('#anVoice'), [
    ['Active Talkers', t.babbleActive ? `${activeNow}` : '—'], ['Average Talkers', t.babbleActive ? fmt(t.meanActive) : '—'],
    ['Speech Occupancy', t.babbleActive ? `${fmt(occupancy() * 100, 0)}%` : '—'],
    ['Average Gap', `${fmt((v.gapMean60s ?? 0) * 1000, 0)} ms`], ['Longest Recent Gap', `${fmt((v.gapMax60s ?? 0) * 1000, 0)} ms`],
    ['Temporal density', dens], ['Envelope L10–L90', `${fmt(st.temporal?.envelopeL10L90Db)} dB`],
  ]);
  let avgErr = '—', maxDev = '—';
  if (v.haveSpectrum) {
    const hz = S.catalog.thirdOctHz;
    const idx = hz.map((h, i) => i).filter((i) => hz[i] >= 125 && hz[i] <= 8000);
    const dif = idx.map((i) => v.measuredDb[i] - v.referenceDb[i]);
    const mean = dif.reduce((a, b) => a + b, 0) / dif.length;
    const dev = dif.map((d) => d - mean);
    avgErr = `${fmt(dev.reduce((a, b) => a + Math.abs(b), 0) / dev.length)} dB`;
    let k = 0;
    dev.forEach((d, j) => { if (Math.abs(d) > Math.abs(dev[k])) k = j; });
    maxDev = `${fmt(Math.abs(dev[k]))} dB at ${hz[idx[k]] >= 1000 ? `${hz[idx[k]] / 1000} kHz` : `${hz[idx[k]]} Hz`}`;
  }
  kv($('#anSpec'), [['Average target error', avgErr], ['Largest deviation', maxDev],
    ['Correction (max)', st.spectrum?.correctionDb ? `${fmt(Math.max(...st.spectrum.correctionDb.map(Math.abs)))} dB${st.spectrum.eqClamped ? ' (clamped)' : ''}` : '—']]);
  kv($('#anOut'), [
    ['RMS', dbStr(all)], ['LUFS-S', dbStr(st.level?.lufsS, 'LUFS')], ['True Peak', dbStr(v.truePeak10sDb, 'dBTP')],
    ['Crest (TP − RMS)', `${fmt(st.level?.crestDb)} dB`], ['Limiter', lim.enabled ? `${fmt(lim.grMaxDb)} dB GR max` : 'disabled'],
    ['Babble share (measured)', `${fmt((st.mix?.measuredBabbleFraction ?? 0) * 100, 0)}%`],
    ['Babble share (configured)', `${fmt((st.mix?.configuredBabbleFraction ?? 0) * 100, 0)}%`],
  ]);
  const sp = [];
  (v.outputActiveFraction ?? []).forEach((f, i) => sp.push([`${names[i] ?? `Out ${i + 1}`} active`, `${fmt(f * 100, 0)}%`]));
  (v.adjacentCorrelation ?? []).forEach((r, i) => sp.push([`Correlation ${i + 1}–${i + 2}`, fmt(r, 2)]));
  (t.channelBalanceDb ?? []).forEach((b, i) => sp.push([`Balance ${names[i] ?? i + 1}`, `${fmt(b)} dB`]));
  if (!sp.length) sp.push(['Spatial', 'collecting…']);
  kv($('#anSpatial'), sp);
  drawAnalysisSpectrum(v);
  drawVoices();
  drawMap(v, lay);
}

// ------------------------------------------------------------------ corpus
async function loadCorpus() {
  const want = new URLSearchParams(location.search).get('corpus');
  const status = (s) => ($('#stVoices').textContent = `Voices: ${s}`);
  if (want !== 'synthetic') {
    try {
      const res = await fetch('./corpus/corpus.json');
      if (!res.ok) throw new Error(`corpus.json: HTTP ${res.status}`);
      const corpus = await res.json();
      const dec = new OfflineAudioContext(1, 1, 48000);
      let done = 0;
      const audio = new Array(corpus.recordings.length);
      const queue = corpus.recordings.map((r, i) => [r, i]);
      const runOne = async () => {
        for (let item; (item = queue.shift());) {
          const [r, i] = item;
          const buf = await (await fetch(`./corpus/${r.file}`)).arrayBuffer();
          const ab = await dec.decodeAudioData(buf);
          audio[i] = ab.getChannelData(0).slice();
          status(`loading speech ${++done}/${corpus.recordings.length}`);
          $('#startBtn').textContent = `LOADING VOICES ${done}/${corpus.recordings.length}`;
        }
      };
      await Promise.all([runOne(), runOne(), runOne(), runOne()]);
      const r = await rpc('corpus', { corpus, audio }, audio.map((a) => a.buffer));
      if (!r.ok) throw new Error(r.error);
      S.corpus = { kind: 'speech', label: `LibriSpeech (${corpus.speakers.length} speakers, CC BY 4.0)` };
      status(S.corpus.label);
      $('#corpusAbout').innerHTML = `<b>Voices:</b> ${corpus.speakers.length} real speakers from the LibriSpeech ASR corpus (dev-clean, CC BY 4.0; readers are LibriVox volunteers), analysed by the BabbleForge corpus importer. LibriSpeech is 16 kHz audio, so the voices carry no energy above ~8 kHz; the stationary component and spectral correction cover the rest.`;
      return;
    } catch (e) {
      console.warn('speech corpus unavailable, using synthetic voices:', e);
    }
  }
  const r = await rpc('synthetic', { speakers: 24 });
  S.corpus = { kind: 'synthetic', label: 'synthetic voice placeholders' };
  status('synthetic voice placeholders (pseudo-speech)');
  $('#corpusAbout').innerHTML = '<b>Voices:</b> synthetic voice placeholders — procedurally generated pseudo-speech (tones + shaped noise with speech-like timing), not real recordings.';
  return r;
}

// ------------------------------------------------------------------ audio
async function ensureAudio() {
  if (S.ctx) return;
  const ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'playback' });
  await ctx.audioWorklet.addModule(new URL('./worklet.js', import.meta.url));
  const node = new AudioWorkletNode(ctx, 'babbleforge-player', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
  const gain = ctx.createGain();
  gain.gain.value = 0;
  node.connect(gain).connect(ctx.destination);
  node.port.onmessage = (e) => { if (e.data.type === 'meter') S.meter = e.data; };
  const ch = new MessageChannel();
  node.port.postMessage({ type: 'port', port: ch.port1 }, [ch.port1]);
  await rpc('port', { port: ch.port2 }, [ch.port2]);
  Object.assign(S, { ctx, node, gain });
}

async function start() {
  const btn = $('#startBtn');
  btn.disabled = true;
  stopTest();
  await ensureAudio();
  await S.ctx.resume();
  const r = await rpc('start', { preset: structuredClone(S.preset), seed: S.seed });
  btn.disabled = false;
  if (!r.ok) { alert(`Could not start: ${r.error}`); return; }
  S.running = true;
  S.startedAt = performance.now();
  const g = S.gain.gain, t = S.ctx.currentTime;
  g.cancelScheduledValues(t); g.setValueAtTime(g.value, t); g.linearRampToValueAtTime(1, t + 0.6);
  updateRunUi();
}
async function stop() {
  S.running = false;
  updateRunUi();
  const g = S.gain.gain, t = S.ctx.currentTime;
  g.cancelScheduledValues(t); g.setValueAtTime(g.value, t); g.linearRampToValueAtTime(0, t + 0.3);
  await new Promise((res) => setTimeout(res, 350));
  if (S.running) return;
  await rpc('stop');
  S.node.port.postMessage({ type: 'flush' });
}
function updateRunUi() {
  const btn = $('#startBtn');
  btn.textContent = S.running ? 'STOP MASKING' : 'START MASKING';
  btn.classList.toggle('running', S.running);
  $('#runState').textContent = S.running ? 'MASKING ACTIVE' : 'READY';
  $('#runState').classList.toggle('active', S.running);
  $('#stEngine').textContent = `Engine: ${S.running ? 'MASKING ACTIVE' : 'READY'}`;
}

// TEST SPEAKERS (GUI section 23): short noise bursts on Left then Right, engine stopped.
let testing = null;
function stopTest() {
  if (!testing) return;
  testing.forEach((s) => { try { s.stop(); } catch { /* ended */ } });
  testing = null;
  $('#testBtn').textContent = 'TEST SPEAKERS';
  $('#testBtn').classList.remove('active');
}
async function testSpeakers() {
  if (testing) return stopTest();
  if (S.running) await stop();
  await ensureAudio();
  await S.ctx.resume();
  const ctx = S.ctx;
  const len = ctx.sampleRate;
  const buf = ctx.createBuffer(1, len, ctx.sampleRate);
  const d = buf.getChannelData(0);
  let b0 = 0, b1 = 0, b2 = 0;
  for (let i = 0; i < len; i++) {  // pink-ish noise, 20 ms ramps, ~-26 dBFS
    const w = Math.random() * 2 - 1;
    b0 = 0.99765 * b0 + w * 0.099046; b1 = 0.963 * b1 + w * 0.2965164; b2 = 0.57 * b2 + w * 1.0526913;
    const env = Math.min(1, i / 960, (len - i) / 960);
    d[i] = (b0 + b1 + b2 + w * 0.1848) * 0.05 * 0.6 * env;
  }
  testing = [];
  [-1, 1].forEach((pan, k) => {
    const s = ctx.createBufferSource();
    s.buffer = buf;
    const p = ctx.createStereoPanner();
    p.pan.value = pan;
    s.connect(p).connect(ctx.destination);
    s.start(ctx.currentTime + 0.1 + k * 1.3);
    testing.push(s);
    if (k === 1) s.onended = () => stopTest();
  });
  $('#testBtn').textContent = 'STOP TEST (Left, then Right)';
  $('#testBtn').classList.add('active');
}

// ------------------------------------------------------------------ wiring
function wire() {
  $$('.nav button').forEach((b) => b.addEventListener('click', () => showPage(b.dataset.page)));
  $('#modeSimple').addEventListener('click', () => { S.advanced = false; save(); render(); if (current === 'analysis') showPage('run'); });
  $('#modeAdvanced').addEventListener('click', () => {
    let seen = false;
    try { seen = localStorage.getItem('bf.advSeen') === '1'; } catch { /* storage blocked */ }
    if (seen || !$('#advDialog').showModal) { S.advanced = true; save(); render(); return; }
    $('#advDialog').showModal();
  });
  $('#advOk').addEventListener('click', () => {
    try { localStorage.setItem('bf.advSeen', '1'); } catch { /* storage blocked */ }
    $('#advDialog').close(); S.advanced = true; save(); render();
  });
  $('#advCancel').addEventListener('click', () => $('#advDialog').close());
  for (const id of ['#areaSel', '#areaSel2']) $(id).addEventListener('change', (e) => A.setArea(e.target.value));
  for (const id of ['#strategySel', '#strategySel2']) $(id).addEventListener('change', (e) => A.setStrategy(e.target.value));
  $$('.qcard').forEach((b) => b.addEventListener('click', () => A.quick(b.dataset.quick)));
  for (const n of ['coverage', 'coverage2']) $$(`input[name=${n}]`).forEach((r) => r.addEventListener('change', () => A.setCoverage(r.value)));
  $$('input[name=layout]').forEach((r) => r.addEventListener('change', () => A.setLayout(r.value)));
  $$('input[name=speakers]').forEach((r) => r.addEventListener('change', () => A.setLayout(r.value)));
  $('#statEnabled').addEventListener('change', (e) => A.setStatEnabled(e.target.checked));
  $('#seedMode').addEventListener('change', (e) => A.setSeedMode(e.target.value));
  $('#spectrumSel').addEventListener('change', (e) => A.setTarget(e.target.value));
  $('#corrEnabled').addEventListener('change', (e) => A.setCorrEnabled(e.target.checked));
  $('#corrSpeed').addEventListener('change', (e) => A.setCorrSpeed(e.target.value));
  $('#limEnabled').addEventListener('change', (e) => A.setLimEnabled(e.target.checked));
  $('#startBtn').addEventListener('click', () => (S.running ? stop() : start()));
  $('#testBtn').addEventListener('click', testSpeakers);
  $$('[data-spec]').forEach((b) => b.addEventListener('click', () => {
    S.specView = b.dataset.spec;
    $$('[data-spec]').forEach((x) => x.classList.toggle('on', x === b));
    if (S.stats) drawAnalysisSpectrum(S.stats.view);
  }));
  $('#eqBands').innerHTML = EQ_HZ.map((hz, b) => `<div class="band"><span class="bv">0</span><input type="range" min="-12" max="12" step="0.5" value="0" aria-label="${EQ_LABELS[b]} Hz"><span>${EQ_LABELS[b]}</span></div>`).join('');
  $$('#eqBands input').forEach((inp, b) => inp.addEventListener('input', () => A.setEqBand(b, Number(inp.value))));
  document.addEventListener('keydown', (e) => {
    if (e.code === 'Space' && !['INPUT', 'SELECT', 'BUTTON'].includes(document.activeElement?.tagName) && !$('#startBtn').disabled) {
      e.preventDefault(); $('#startBtn').click();
    }
  });
  setInterval(() => {
    if (!S.running) return;
    const s = Math.floor((performance.now() - S.startedAt) / 1000);
    $('#elapsed').textContent = [s / 3600, (s / 60) % 60, s % 60].map((x) => String(Math.floor(x)).padStart(2, '0')).join(':');
  }, 500);
}
let current = 'run';
function showPage(id) {
  current = id;
  $$('.nav button').forEach((b) => b.classList.toggle('on', b.dataset.page === id));
  $$('.page').forEach((p) => p.classList.toggle('on', p.id === `page-${id}`));
  render();
  if (S.stats) onStats(S.stats);
}
function save() {
  try { localStorage.setItem('bf.web.ui', JSON.stringify({ advanced: S.advanced, preset: S.preset })); } catch { /* storage blocked */ }
}
setInterval(save, 2000);

// ------------------------------------------------------------------ boot
async function boot() {
  $('#stEngine').textContent = 'Engine: LOADING';
  const r = await rpc('init');
  if (!r.ok) { $('#stEngine').textContent = `Engine: ERROR ${r.error}`; return; }
  const cat = r.catalog;
  S.catalog = cat;
  const order = ['small_room', 'office', 'conference', 'open_office', 'large_room', 'common_area', 'reception', 'free_field', 'custom'];
  for (const id of order) { const a = cat.areas.find((x) => x.id === id); if (a) S.areas[id] = a; }
  for (const a of cat.areas) S.areas[a.id] ??= a;
  const sorder = ['balanced', 'natural', 'dense', 'speech_noise', 'multi_voice', 'hybrid', 'research'];
  for (const id of sorder) { const s = cat.strategies.find((x) => x.id === id); if (s) S.strategies[id] = s; }
  for (const s of cat.strategies) S.strategies[s.id] ??= s;
  for (const t of cat.targets) S.targets[t.id] = t;
  let saved = null;
  try { saved = JSON.parse(localStorage.getItem('bf.web.ui') || 'null'); } catch { /* storage blocked */ }
  const fresh = new URLSearchParams(location.search).has('fresh');
  S.preset = !fresh && saved?.preset?.area && S.areas[saved.preset.area] ? saved.preset : factoryPreset('office', 'balanced');
  S.advanced = !fresh && !!saved?.advanced;
  buildSliders();
  wire();
  await recompute();
  $('#stEngine').textContent = 'Engine: LOADING VOICES';
  await loadCorpus();
  await recompute();
  $('#startBtn').disabled = false;
  updateRunUi();
  S.ready = true;
}
boot();
