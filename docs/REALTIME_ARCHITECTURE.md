# BabbleForge V1 — Real-Time Architecture, Threading, Memory and Determinism

Covers brief sections 5, 6, 41, 46, 47 (buffering), 48, 49 (measurement). Labels are defined in `ENGINE.md` §0.1.

---

## 1. Real-time safety rules

The audio callback (and every function it calls) is **RT context**.

### 1.1 Forbidden in RT context

| # | Forbidden | Examples |
|---|---|---|
| 1 | Heap allocation or deallocation | `new`, `delete`, `malloc`, `std::vector::push_back` beyond capacity, `std::string` construction, `std::function` with captures, `shared_ptr` copies that may drop the last reference |
| 2 | Blocking synchronization | `std::mutex::lock`, `juce::CriticalSection`, condition variables, `WaitableEvent::wait`, `join`, semaphores |
| 3 | File system | open/read/write/seek/stat, directory scanning, memory-mapped access to pages that may not be resident |
| 4 | Network | any socket or HTTP operation |
| 5 | Logging to disk or console | `DBG`, `std::cout`, `printf`, `Logger::writeToLog` |
| 6 | Corpus analysis, VAD, F0, fingerprinting | — |
| 7 | Filter design, FFT planning, JSON parsing, DB queries | — |
| 8 | UI interaction | `MessageManager` calls, `AsyncUpdater::triggerAsyncUpdate` (may allocate on some platforms), component repaint |
| 9 | Unbounded loops | Any loop whose bound depends on external data without a compile-time or `prepare()`-time cap |
| 10 | OS calls with unbounded latency | `sleep`, `yield`, thread creation, priority changes, `SetEvent` on Windows is **permitted only** via the notifier described in §5.4 |
| 11 | Exceptions | No `throw` on the RT path. RT code is compiled `noexcept`, and any exception reaching the callback boundary terminates (fail loudly in debug builds) |
| 12 | Denormal processing | FTZ/DAZ set at callback entry |
| 13 | Virtual dispatch into user-replaceable code | Strategies never run in RT (`MASK_STRATEGIES.md` §2) |

### 1.2 Permitted in RT context

- Reading and writing preallocated buffers.
- Lock-free SPSC FIFO push/pop of trivially copyable messages.
- `std::atomic` loads and stores (lock-free types only; `static_assert(is_always_lock_free)`).
- Acquire/release pointer exchange (RCU).
- Arithmetic.
- Fixed-size FFTs with plans built at `prepare`.

### 1.3 Enforcement

| Mechanism | Build |
|---|---|
| RT-sanitizer: allocation hooks (override `operator new`/`malloc` via interposition) and mutex hooks that `abort()` when called on a thread flagged RT | Debug and `RT_CHECK` CI builds |
| Clang `[[clang::nonblocking]]` / `-Wfunction-effects` on RT entry points where the toolchain supports it | CI (clang) |
| RealtimeSanitizer (LLVM `-fsanitize=realtime`) on Linux CI | CI |
| Code review checklist item "RT path touched" | Always |
| Acceptance A-RT-1 (24 h instrumented run, 0 violations) | Release gate |

---

## 2. Threading model

| Thread | Count | Priority | Responsibilities | May block? |
|---|---|---|---|---|
| **Audio RT** (driver-owned) | 1 | Driver/MMCSS "Pro Audio" | Graph processing, event consumption, meter accumulation, tap publication | Never |
| **Message/UI** (JUCE message thread) | 1 | Normal | GUI, user commands → Control | Yes (not on RT) |
| **Engine Control** | 1 | Above normal | Engine state machine, plan building (strategies), validation, graph prepare/rebuild, parameter distribution, garbage reclamation, device-change handling | Yes (bounded) |
| **Planner** | 1 | Above normal | Talker planner + segment selector (`TALKER_ENGINE.md` §4–6); keeps ≥ 4 s of event lookahead | Yes (on preloader backpressure only) |
| **Decode/Preload pool** | 2 (1–4) + 1 urgent lane | Below normal (urgent: normal) | FLAC decode, resampling, pause shortening, block-chain fill | Yes (disk I/O) |
| **Analysis** | 1 | Below normal | Spectrum analysis, correction loop, modulation/gap analysis, correlation, limiter alarms, level trims | Yes |
| **Filter design** (worker, shares the pool with Analysis tasks) | 1 | Below normal | FIR design, kernel partitioning | Yes |
| **Corpus I/O / Ingestion pool** | 0 when idle; N−2 during import | Idle/below normal | Ingestion pipeline, DB writes, rescans | Yes |
| **Logger** | 1 | Low | Drains log queues and writes files | Yes |
| **Watchdog** | 1 | Normal | 1 Hz health check (§6.3), persistence timers | Yes |

Named threads (`juce::Thread` with names `bf.control`, `bf.planner` etc.) appear in crash dumps.

The engine applies the "below normal" / "low" rows itself at thread start (`core/rt/ThreadPriority.h`: Linux per-thread nice +5 / +10, Windows thread priority, macOS QoS class): decode/preload (urgent lane stays normal) and analysis run below normal, the logger low. Raising priority above normal needs privileges and is left to the audio driver / host. Without this, preload bursts (decode + resampling, heaviest at 88.2/96 kHz) and analysis ran at the callback's priority and could preempt it on a machine with few free cores: the occasional 0.5–1.2 × buffer-period callback spikes measured at 96 kHz were involuntary context switches in the middle of the DSP (revised during implementation).

### 2.1 Communication map

```text
UI ──cmd FIFO (MPSC via control mutex-free queue)──► Control
Control ──RCU plan / kernels / gain tables──► RT
Control ──plan──► Planner
Planner ──TalkerEvent SPSC──► RT
Planner ──PreloadRequest queue──► Decode pool
Decode pool ──block-chain write index (atomic)──► RT
RT ──RT→Planner SPSC (starts, ends, starvation, underflow)──► Planner
RT ──tap SPSC rings (T1–T4 audio, envelopes, k_a/k_s)──► Analysis
RT ──meter seqlock snapshots──► Analysis/UI
RT ──RT log FIFO (POD codes)──► Logger
Analysis ──correction C[b] / trims (atomics, RCU kernels via Control)──► RT
Analysis ──MaskStatistics snapshot (seqlock)──► UI, Diagnostics
Control ──status events (async)──► UI
```

---

## 3. Audio Device Layer

- A wrapper around `juce::AudioDeviceManager` owned by Control.
- **Device selection** is explicit: the engine opens only the device named in settings/preset. It **never auto-switches** to another device (brief §44, GUI §58).
- **Channel map:** logical outputs → device channels (`SPATIAL_ENGINE.md` §7). Unmapped device outputs are zeroed.
- **Callback wrapper** (per device callback):
  1. set FTZ/DAZ; mark the thread RT for the sanitizer
  2. record the start timestamp (`std::chrono::steady_clock`, which is RT-safe and non-blocking)
  3. split the buffer into sub-blocks of ≤ 256 frames; process each; write to the device buffers
  4. record the end timestamp; update the DSP-load histogram (lock-free array of 64 buckets)
  5. detect xruns (§7)

**Device events** (JUCE `ChangeListener` on the message thread) are forwarded to Control:
- `deviceAboutToStart`
- `deviceStopped`
- `deviceError`
- `audioDeviceListChanged`
- sample rate or buffer size changed

---

## 4. Buffer handling

| Buffer | Owner | Size | Allocation |
|---|---|---|---|
| Sub-block work buffers (babble[N], stationary[N], mix[N]) | RT | N × 256 floats each | `prepare` |
| Convolver state (per channel × 2 shapers) | RT | (L/256 + 2) × 512 complex per shaper | `prepare`; kernels via RCU |
| Decorrelator delay lines | RT | ≤ 5 × 4.5 ms per channel | `prepare` |
| Output delay lines | RT | 100 ms × N | `prepare` |
| Limiter lookahead + oversampling state | RT | 5 ms × N + FIR state | `prepare` |
| Talker slots | RT | V_max (≤ 96) | `prepare` |
| Block pool (talker audio) | shared | default 128 MiB | `prepare` (Control) |
| Tap rings (RT → Analysis) | shared | 2 s of audio per tap per channel (T1: N ch; T2: N ch; T3: N ch; T4: N ch) — at 48 kHz, 8 ch: 4 × 8 × 96000 × 4 B = 12.3 MB | `prepare` |
| Event FIFO | shared | 1024 × 128 B | `prepare` |

**On overflow of a tap ring:** the RT side drops the newest data and increments `tapOverflow` (the analysis thread is late). Analysis results are marked `incomplete` for that window. Audio is never affected.

---

## 5. Lock-free communication

### 5.1 SPSC FIFO

A bounded ring (capacity a power of two) with `std::atomic<size_t>` head/tail, acquire/release ordering, and trivially copyable messages ≤ 128 B. JUCE `AbstractFifo` semantics are acceptable. All producer/consumer pairs are single-threaded by construction. If multiple producers are needed (UI and Watchdog → Control), they go through Control's own mutex-protected queue, which is **never** touched by RT.

### 5.2 RCU pointer exchange for large immutable objects

Objects: `MaskRenderPlan` (RT subset), FIR kernels, spatial gain tables, output-matrix config, calibration EQ.

```text
Control:  auto* p = new T(...);                 // non-RT allocation
          old = slot.pending.exchange(p, acq_rel);
          if (old) garbage.push(old);            // superseded before RT picked it up
RT (sub-block start):
          if (T* n = slot.pending.exchange(nullptr, acq_rel)) {
              retire = slot.current; slot.current = n;  // begin crossfade if needed
              rtToControl.push(RetireMsg{retire});      // Control deletes later
          }
```

Deletion happens only on Control, after the RT thread has handed the pointer back. No reference counting occurs on RT.

### 5.3 Scalar parameters

- `std::atomic<float>` per scalar (Strength, mix b per zone, output gains, mute, limiter ceiling), written by Control and read once per sub-block by RT.
- RT applies smoothing (one-pole or linear ramps as specified per parameter).

### 5.4 RT → other thread wakeups

RT never signals OS events directly. Consumer threads poll their FIFOs:
- Planner: every 20 ms
- Analysis: every 50 ms
- Logger: every 100 ms
- urgent preload: every 5 ms, using a `std::atomic<bool>` hint

Polling cost is negligible, and it avoids priority inversion.

### 5.5 Meter snapshots

Meter snapshots are published with a seqlock: the RT writer increments a sequence counter, writes a POD struct, and increments it again. Readers retry if the sequence is odd or changed. The writer never blocks.

### 5.6 Synchronization model summary

- The RT thread owns all DSP state.
- Other threads influence RT only via (a) atomics, (b) SPSC messages, and (c) RCU pointers.
- No shared mutable object is accessed from RT and another thread except the block-pool chains, which follow single-writer (decoder) / single-reader (RT) semantics with an atomic write index and a separate atomic "consumed" index for block recycling.

---

## 6. File decoding strategy

1. All decoding happens in the Decode/Preload pool (`TALKER_ENGINE.md` §8).
2. FLAC cache files are opened on demand and kept in an LRU handle cache (64 handles). This avoids repeated open cost while bounding handle count for multi-day runs.
3. Reads are sequential per event. OS read-ahead suffices; no memory mapping is used for FLAC.
4. **Slow-disk tolerance:** the preload targets (8 s ahead) tolerate stalls up to ~6 s without audible effect. Disk-read latency is measured per request and published as `preload.p99LatencyMs`.
5. Network drives are supported but trigger an advisory ("Voice library on a network drive may cause interruptions") when p99 latency > 200 ms.

---

## 7. Underruns (xruns) and recovery

| Detection | Method |
|---|---|
| Engine overrun | Callback duration > 90 % of buffer duration → `rtOverrun++` (a near-miss indicator) |
| Driver-reported xrun | `AudioIODevice::getXRunCount()` delta (ASIO, CoreAudio, some WASAPI) |
| Timestamp gap | Consecutive callback start timestamps differing by > 1.5 × buffer duration → `callbackGap++` |

**Recovery:**
- Processing continues. The engine does not stop or reset on an xrun.
- The event timeline advances by engine sample count, not wall-clock, so an xrun does not desynchronize the plan.
- Diagnostics counter `xruns` is incremented and the event is logged (rate-limited: first 10, then a 1-per-minute summary).
- **Sustained xruns** (> 10 per minute for 2 minutes) → DEGRADED state `audio.xruns`. The GUI advisory suggests a larger buffer size. There is no automatic buffer change (that would restart the device).

---

## 8. Determinism and random number generation

### 8.1 Requirements

Given (preset + data-set hash, corpus version, seed, duration, sample rate, and scenario events), BabbleForge regenerates the same output.

| Level | Guarantee | Scope |
|---|---|---|
| D1 — Event determinism | The talker event timeline (JSON) is identical across platforms and builds | Planner, selector, spatial placement |
| D2 — Bit-exact audio | The output WAV is bit-identical for the same build, platform, sample rate and data set, **independent of block size** | Offline render; real-time when no starvation/late-start occurred |
| D3 — Cross-platform audio | Difference ≤ −100 dB relative to output RMS | Different compilers/OS |

The real-time path is deterministic up to (and including) the first preload starvation event. After it, the session is marked `determinismBroken` (reason logged).

### 8.2 PRNG specification

| Item | Choice |
|---|---|
| Generator | **xoshiro256\*\*** (Blackman & Vigna 2018), 64-bit output, period 2^256 − 1 |
| Seeding | Master seed: 64-bit unsigned (user or generated from `std::random_device` + time at session start, **then logged**). The xoshiro state is filled by 4 outputs of **SplitMix64** |
| Stream derivation | `streamSeed = SplitMix64(masterSeed ^ FNV1a64(streamName) ^ (epoch * 0x9E3779B97F4A7C15))`. Each named stream is an independent generator |
| Named streams | `planner.slot.<j>`, `planner.global`, `selector`, `selector.shuffle.<speakerId>.<cycle>`, `gainvar`, `spatial.slot.<j>`, `spatial.motion`, `noise.ch.<c>`, `decorrelator.ch.<c>`, `lab.speakerset` |
| Uniform double [0, 1) | `(x >> 11) * 0x1.0p-53` |
| Uniform float [0, 1) | `(x >> 40) * 0x1.0p-24` |
| Integers in [0, n) | Lemire's nearly-divisionless method with rejection (unbiased) |
| Exponential | `−mean · detlog(1 − U)` |
| Normal | Box–Muller (polar form avoided because its loop count varies across platforms under FP differences), using `detlog`, `detsqrt`, `detsincos` |
| Log-normal | `detexp(μ + σ·Z)` |
| Truncation | Rejection with a max-attempts cap (8), then clamp (deterministic) |
| Weighted choice | Cumulative sum in double over a stable-ordered candidate list (sorted by id), then one uniform draw |
| Forbidden | `std::rand`, `std::random_device` (except for the initial master seed), `std::*_distribution` (implementation-defined), `std::shuffle` (implementation-defined), hash-map iteration order in any decision |

### 8.3 Deterministic math (`detmath`)

- Planner and selector decisions (D1) use portable software implementations of `log`, `exp`, `sin`, `cos` and `sqrt` (correctly rounded `sqrt` is IEEE-exact; the others are fixed polynomial approximations with ≤ 1 ulp error, identical on every platform).
- These translation units compile with `-ffp-contract=off` / `/fp:precise` and no fast-math.
- Planner time quantities are stored as integer samples (`int64`).
- Decisions compare integers or doubles produced by the same deterministic operations.
- **DSP audio code** may use the platform libm and SIMD (it is covered by D2/D3, not D1).

### 8.4 Block-size independence (D2)

- Events are sample-timed.
- Parameter smoothing is per sample or per fixed 32-sample grid anchored to the absolute sample counter (not to block starts).
- The convolver partition (256 at 44.1/48 kHz, 512 at 88.2/96 kHz) is independent of the device buffer, because the engine re-blocks internally with a fixed partition-sized FIFO.
- RCU updates are applied at the first fixed 256-frame grid boundary after their "effective sample" (Control stamps each update with an effective sample time = now + 2048 samples). In offline renders, the same stamps come from the scenario.
- Analysis-driven updates (spectral correction, trims) are computed from taps and applied at stamped boundaries. In offline renders, the analysis thread runs synchronously at its nominal block cadence (5 s) to keep D2.

### 8.5 Seed reporting

The session seed, corpus version, data-set hash, app version and plan epoch history are included in:
- the log at Start
- the diagnostic snapshot
- every render sidecar

"Reproduce this session" in Diagnostics exports a scenario file (`PRESETS.md` §6.3) with the recorded parameter-change events.

---

## 9. Memory management

| Component | Estimate (stereo, V = 12) | Estimate (16 ch, V = 54 distributed) | Notes |
|---|---|---|---|
| Corpus runtime index | 0.2 MB (50 spk × 15 min) | same | ≤ 100 MB for 10 000 speakers |
| Block pool | 128 MiB (default floor) | 128 MiB → auto 1.5 × 54 × 10 s × 192 KB ≈ 155 MB | Configurable |
| Convolver state + kernels | 2 ch × 2 × ~0.1 MB ≈ 0.4 MB | 16 × 2 × 0.1 ≈ 3.2 MB (+ crossfade duplicates) | — |
| Tap rings | 3.1 MB | 24.6 MB | 2 s per tap |
| Analysis buffers (FFT 8192, averages, histograms) | 2 MB | 6 MB | — |
| Event/log FIFOs | 0.5 MB | 0.5 MB | — |
| FLAC decoders (LRU 64 handles) | ~8 MB | ~8 MB | — |
| SQLite page cache | 16 MB (cap) | 16 MB | Only while querying |
| GUI (separate budget) | ~100 MB | ~100 MB | Not engine |
| **Engine total (excluding GUI)** | **≈ 160 MB** | **≈ 215 MB** | Target ≤ 256 MB default, ≤ 1 GB max configurable |

- All engine memory is allocated at `prepare()` or on non-RT threads.
- Steady-state operation performs **no** net allocation growth (A-RT-3). Log strings, analysis history and diagnostics ring buffers are bounded (fixed capacity).

---

## 10. Graph lifecycle

```text
prepare(sampleRate, maxDeviceBlock, layout):
    compute derived sizes (taps, FFT sizes, oversampling factor)
    allocate every RT buffer, pool, FIFO, slot, delay line
    build initial kernels (stationary, babble static EQ), gain tables
    reset RNG streams (seed, epoch 0)
    planner: pre-plan ≥ 4 s; preloader: fill initial events (≥ 1.25 s each)
    → READY when preload of the first-second events completes (≤ 3 s timeout → proceed; late events start late)
release():
    stop device callback → join planner/preloader after draining → free resources on Control
```

**Sample-rate or layout change:** full `release()` + `prepare()` through the STOPPING → PREPARING path (`RELIABILITY.md` §1). The audio fades out over 50 ms before release and fades in over 500 ms after start.
