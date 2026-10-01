// AudioWorkletProcessor: plays stereo chunks rendered ahead by the engine worker.
//
// No SharedArrayBuffer (GitHub Pages cannot send COOP/COEP headers): the worker posts
// Float32Array chunks (transferred) through a MessagePort; this processor queues them and
// reports the consumed frame count back so the worker keeps ~0.5 s rendered ahead.
// The engine always runs at 48 kHz; if the AudioContext runs at another rate the chunks are
// resampled here by linear interpolation.
class BabbleForgePlayer extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.queue = [];
    this.head = null;
    this.pos = 0;           // read position (fractional when resampling) inside head
    this.step = 48000 / sampleRate;
    this.consumed = 0;      // engine frames consumed
    this.reported = 0;
    this.underruns = 0;
    this.started = false;
    this.e = [0, 0];        // energy since the last report (played audio, for meters / tests)
    this.n = 0;
    this.peak = [0, 0];
    this.engine = null;     // MessagePort to the worker
    this.port.onmessage = (ev) => {
      const m = ev.data;
      if (m.type === 'port') {
        this.engine = m.port;
        this.engine.onmessage = (e2) => this.onChunk(e2.data);
      } else if (m.type === 'flush') {
        this.queue = [];
        this.head = null;
        this.pos = 0;
        this.started = false;
      }
    };
  }

  onChunk(m) {
    if (m.type === 'chunk') this.queue.push(m);
    else if (m.type === 'reset') {
      this.queue = [];
      this.head = null;
      this.pos = 0;
      this.consumed = m.consumed || 0;
      this.reported = this.consumed;
      this.started = false;
    }
  }

  nextHead() {
    this.head = this.queue.length ? this.queue.shift() : null;
    this.pos = 0;
    return this.head;
  }

  sample(ch, p) {
    // value at fractional position p of the current head (may look one frame ahead)
    const h = this.head;
    const a = ch === 0 ? h.l : h.r;
    const i = Math.floor(p);
    const f = p - i;
    const x0 = a[i];
    let x1 = x0;
    if (f > 0) {
      if (i + 1 < a.length) x1 = a[i + 1];
      else if (this.queue.length) x1 = (ch === 0 ? this.queue[0].l : this.queue[0].r)[0];
    }
    return x0 + (x1 - x0) * f;
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    const n = L.length;
    // Wait for ~100 ms of audio before (re)starting, so that a stall does not crackle.
    if (!this.started) {
      let q = 0;
      for (const c of this.queue) q += c.l.length;
      if (q < 4800) {
        L.fill(0); R.fill(0);
        return true;
      }
      this.started = true;
    }
    for (let i = 0; i < n; i++) {
      if (!this.head || this.pos >= this.head.l.length) {
        const prevLen = this.head ? this.head.l.length : 0;
        const carry = this.head ? this.pos - prevLen : 0;
        if (!this.nextHead()) {
          L.fill(0, i); R.fill(0, i);
          this.underruns++;
          this.started = false;
          break;
        }
        this.pos = Math.max(0, carry);
      }
      let l, r;
      if (this.step === 1) {
        l = this.head.l[this.pos]; r = this.head.r[this.pos];
      } else {
        l = this.sample(0, this.pos); r = this.sample(1, this.pos);
      }
      L[i] = l; if (R !== L) R[i] = r;
      this.e[0] += l * l; this.e[1] += r * r; this.n++;
      const al = Math.abs(l), ar = Math.abs(r);
      if (al > this.peak[0]) this.peak[0] = al;
      if (ar > this.peak[1]) this.peak[1] = ar;
      const before = Math.floor(this.pos);
      this.pos += this.step;
      this.consumed += Math.floor(this.pos) - before;
    }
    if (this.engine && this.consumed - this.reported >= 2048) {
      this.reported = this.consumed;
      this.engine.postMessage({ type: 'consumed', consumed: this.consumed });
    }
    if (this.n >= 4800) {  // ~10 Hz played-signal meter
      this.port.postMessage({ type: 'meter', ms: [this.e[0] / this.n, this.e[1] / this.n], peak: this.peak.slice(),
                              underruns: this.underruns, consumed: this.consumed });
      this.e = [0, 0]; this.n = 0; this.peak = [0, 0];
    }
    return true;
  }
}

registerProcessor('babbleforge-player', BabbleForgePlayer);
