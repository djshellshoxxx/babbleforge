# BabbleForge V1 — Talker Engine

Covers brief sections 8, 9, 11, 15, 17, 47. Labels are defined in `ENGINE.md` §0.1.

---

## 1. Concepts and vocabulary

These four quantities are distinct and must never be conflated in code, UI or logs.

| Term | Symbol | Definition |
|---|---|---|
| Configured talker pool | P | Number of distinct *speakers* eligible for selection in the current plan (drawn from the corpus by the diversity mode) |
| Voice slots | V | Number of simultaneous playback voices allocated = maximum active (`maxActive`). Slots are playback channels, not speakers |
| Active talker | k_a(t) | Number of slots whose segment is playing (FADE_IN, ACTIVE or FADE_OUT with gain > −40 dB). Includes short internal pauses |
| Speaking talker | k_s(t) | Number of active slots whose current sample lies inside a VAD speech region (ground truth from metadata) |
| Mean simultaneous talkers | m | Target long-term mean of k_a. Reported both as mean k_a and mean k_s |
| Minimum / maximum active | min, max | Hard bounds on k_a, enforced by the planner (except during starvation) |

Invariant: 0 ≤ min ≤ m ≤ max = V ≤ P ≤ available speakers.

---

## 2. Architecture

```text
  Plan (MaskRenderPlan.talkers)
        │
        ▼
  ┌──────────────┐   request(k)    ┌─────────────────┐
  │ Talker       │ ─────────────►  │ Segment Selector│ ◄── Corpus index, history
  │ Planner      │ ◄───────────── │ (planner thread)│
  │ (planner thr)│   SegmentPick   └─────────────────┘
  └──────┬───────┘
         │ TalkerEvent timeline (sample-timed, ≥ 4 s ahead)
         ├──────────────────────────► Source Preloader (decode pool) ─► slot ring buffers
         ▼                                                         (block pool)
  SPSC event FIFO ──► RT Voice Renderer (V voices): gain · fade · pan → Babble bus [N]
                                   │
                                   └──► RT→planner feedback FIFO (starvation, completion)
```

**Key design decision:** the planner is a deterministic discrete-event simulation that runs **ahead of real time** on its own sample-time clock (engine sample counter). Its decisions depend only on the plan, the seed, the corpus index and its own history. They never depend on wall-clock or thread timing. The same code runs inside `bfrender` synchronously, which gives offline/real-time equivalence.

---

## 3. Virtual talker (voice slot) state

```cpp
struct TalkerSlot {                         // RT-owned mirror; planner owns the plan copy
    uint16_t   slotIndex;
    // Identity
    SpeakerId  speakerId;                   // 32-bit corpus speaker index
    RecordingId recordingId;
    SegmentId  segmentId;                   // selected start anchor / segment record
    // Source
    BlockChainHandle source;                // preloaded, processed audio (ring in block pool)
    uint64_t   readPos;                     // samples consumed within this event's audio
    uint64_t   eventLength;                 // samples (after gap shortening)
    const SpeechMask* vadMask;              // run-length speech flags for k_s ground truth
    // Gain
    float      segGain;                     // ASL normalization × level variation (linear)
    float      targetGain;                  // segGain × dominance cap
    float      currentGain;                 // smoothed (one-pole 20 ms) toward targetGain
    // Activity
    SlotState  state;                       // see §3.1
    FadeState  fade;                        // shape, length, position
    // Spatial
    float      azimuthDeg, elevationDeg, distanceNorm;   // virtual position
    float      motionRateDegPerS;
    GainVector outGains;                    // N gains (from Spatial Renderer), smoothed 50 ms
    uint32_t   zoneMask;                    // output routing: zones this talker feeds
    // Scheduling
    uint64_t   eventId;                     // monotonically increasing
    uint64_t   startSample, fadeOutSample, endSample;
    // History (planner side only)
    uint64_t   lastEndSample;
    SpeakerId  previousSpeaker;
};
```

### 3.1 Slot state machine

States (planner and RT views combined):

```text
            plan: start chosen                 audio ready & t ≥ start
  ┌──────┐ ───────────────► ┌─────────┐ ─────────────────► ┌─────────┐
  │ WAIT │                  │ PRELOAD │                    │ FADE_IN │
  └──────┘ ◄─┐              └────┬────┘                    └────┬────┘
     ▲       │ cooldown elapsed  │ preload failed               │ fade complete
     │       │                   ▼                              ▼
     │   ┌───┴──────┐      ┌──────────┐   source error /  ┌─────────┐
     │   │ COOLDOWN │      │ REPLACE  │ ◄──starvation──── │ ACTIVE  │
     │   └──────────┘      └────┬─────┘                   └────┬────┘
     │        ▲                 │ new segment picked            │ t ≥ fadeOutSample
     │        │                 ▼                               ▼
     │        │            (back to PRELOAD with          ┌──────────┐
     │        └─────────── same start if still future,     │ FADE_OUT │
     │                     else start = now + 200 ms)      └────┬─────┘
     │                                                          │ gain = 0
     └──────────────────────────────────────────────────────────┘ (→ COOLDOWN)
```

| From | To | Condition | Owner |
|---|---|---|---|
| WAIT | PRELOAD | Planner schedules an event for this slot (start time t_s) | planner |
| PRELOAD | FADE_IN | RT reaches t_s **and** `readyFlag` (≥ fadeIn + 1.0 s of audio buffered) | RT |
| PRELOAD | REPLACE | Preloader reports decode/missing-file failure | preloader → planner |
| PRELOAD | (delayed start) | RT reaches t_s but not ready: event is postponed in 100 ms steps up to 1 s, counted as `lateStart`; after 1 s → REPLACE, counted as `starvation` | RT/planner |
| FADE_IN | ACTIVE | fade position = fade length | RT |
| ACTIVE | FADE_OUT | t ≥ fadeOutSample | RT |
| ACTIVE | FADE_OUT (fast 20 ms) → REPLACE | Buffer underflow imminent (< fast-fade length + 1 block buffered) or source error flag | RT |
| FADE_OUT | COOLDOWN | fade complete | RT |
| COOLDOWN | WAIT | t ≥ lastEnd + reEntryCooldown | planner |
| REPLACE | PRELOAD | Selector returns a substitute segment (a different recording; same speaker if possible, else another speaker) | planner |
| any | WAIT (flush) | Plan epoch change and event not yet started, or engine stop | planner |

**Fades:**
- Equal-power sine/cosine shapes: g = sin(π/2 · x) for fade-in and cos(π/2 · x) for fade-out, x ∈ [0, 1].
- Fade lengths come from Character (`MASK_STRATEGIES.md` §5.1) with ±30 % uniform jitter per event [E].
- Fade-outs are placed so that they begin within a VAD pause where possible (§4.4). Otherwise the fade completes over speech, which is acceptable at ≥ 80 ms.

---

## 4. Planner (scheduler) algorithm

### 4.1 Timing model: alternating renewal process per slot

Each of the V slots alternates between ON (a segment playing) and OFF (idle), with independent random durations. For V independent slots with mean ON duration D_on and mean OFF duration D_off, the expected active count is:

```
E[k_a] = V · D_on / (D_on + D_off)            ⇒   D_off = D_on · (V − m) / m
```

This produces starts that are asynchronous across slots. Because OFF durations are exponential (memoryless), the superposed start process approaches Poisson, which avoids synchronized starts and stops. k_a is approximately binomial(V, m/V): for V = 9 and m = 6.5, the standard deviation is 1.34. This gives natural fluctuation, bounded by min/max.

### 4.2 Distributions

| Quantity | Distribution | Parameters | Rationale |
|---|---|---|---|
| ON duration (segment length) | **Log-normal**, truncated to [segMin, segMax] by resampling (max 8 attempts, then clamp) | median μ from Character; σ_ln from Character | Utterance/turn durations are positively skewed; a log-normal matches that and avoids a uniform distribution's hard edges [E, standard modelling practice] |
| OFF duration | **Shifted exponential**: cooldown + Exp(mean = max(D_off − cooldown, 0.05 s)) | D_off from §4.1 | Memoryless starts → Poisson superposition → no synchronization [R: Poisson arrivals are the standard model for independent events] |
| Initial phase (at Start) | Each slot ON with probability m/V; if ON, the remaining duration is drawn from the length-biased residual (uniform fraction of a drawn ON duration) | — | Starts in steady state; no "everyone starts at t = 0" artefact |
| Per-segment level variation | **Bounded Gaussian** N(0, σ²) in dB, truncated to ±2σ (resample) | σ from Character × CVR multiplier | Symmetric small variation; the truncation prevents extreme dominance |
| Fade length jitter | Uniform ±30 % | — | Only affects texture; uniform is adequate |
| Handover overlap | Uniform [overlapMin, overlapMin + 300 ms] | from Character/CVR | Small bounded jitter |
| Speaker choice | **Weighted random without replacement** (see §6) | diversity and age weights | Prevents clustering; controllable composition |
| Start anchor within speaker | Next entry of the per-speaker **persistent shuffle** (§6.3) | — | Guarantees coverage before reuse |
| Motion target | Uniform within spread limits | — | Slow; perceptually uncritical |

Uniform randomness is used only where the outcome is perceptually uncritical (jitters, motion targets).

### 4.3 Count controller (keeps the long-run mean exact)

The open-loop model assumes truncated means, but the realized means drift (truncation, forced min/max events, starvation). A slow controller corrects this:

```
every planned second:
    err  = m − meanActive_planned(last 60 s planned time)
    λ    = clamp(λ + 0.02 · err / m, 0.7, 1.3)        // multiplicative OFF-rate factor
    D_off_eff = D_off / λ
```

- The controller operates on the **planned** timeline, so it is deterministic.
- Its time constant (≈ 50 s) keeps it from shaping short-term dynamics.

### 4.4 Event construction

For each new ON period in slot j:

```text
1. t_start  = t_prev_end(j) + OFF duration           (or forced start, §4.5)
2. pick    = Selector.pick(t_start, activeSpeakers, history)   (§6)
3. L_target = draw ON duration
4. end snapping: let B = pause boundaries of pick's processed audio (pauses ≥ 150 ms);
   choose b ∈ B nearest to L_target within [L_target − 1.0 s, L_target + 1.0 s];
   if found → fadeOut starts at b (inside the pause), else at L_target − fadeOut
5. level:  segGain = 10^((L_ref_talker − ASL_seg)/20) · 10^(N(0,σ)/20)
           then dominance cap (CVR): segGain ≤ 10^(cap/20) · meanActiveGain
6. position: from Spatial Renderer policy (seeded stream spatial.slot.j)
7. emit TalkerEvent{eventId, slot j, t_start, fadeIn, fadeOutStart, end, pick, segGain, position}
```

`L_ref_talker` is the per-talker reference. The babble bus is normalized afterwards (`ENGINE.md` §3.2).

### 4.5 Enforcing min / max and avoiding exposure gaps

- **Max:** V = max slots exist, so k_a ≤ max holds by construction.
- **Min:** when a planned fade-out would make k_a < min at time t, the planner immediately schedules a *forced start* in an idle slot (ignoring that slot's remaining OFF time but not its re-entry cooldown) at t − overlap, with overlap from §4.2. If no slot is out of cooldown, it uses the slot whose cooldown ends soonest, at the moment it ends. Forced-start overlap floor is 120 ms (revised during implementation).
- **End-cluster guard (revised during implementation):** when ≤ V − min ends occur in any cooldown+40 ms window, prevent gap.
- **Onset pairing (CVR):** when a start would occur while k_s = 0, the planner pulls the next scheduled start of another slot forward to within the CVR window (100–300 ms), where permitted by that slot's cooldown.
- **Anti-synchrony guard:** no two events may start within 40 ms of each other, and no two may end within 40 ms of each other. A violating event is shifted by +40…+120 ms (uniform draw).

### 4.6 Lookahead, re-planning and epochs

| Parameter | Value | Label |
|---|---|---|
| Planning horizon (events planned ahead of RT time) | 4.0 s minimum, 6.0 s target | [I] |
| Re-plan freeze window | Events starting before now + 0.5 s are kept on plan change | [I] |
| Plan epoch | Increments on every plan change. Events carry their epoch; RT drops events of stale epochs that have not started | [I] |
| Event FIFO capacity | 1024 events | [I] |

On a plan change:
1. The planner discards unstarted events beyond the freeze window and returns their preload blocks.
2. It resets its RNG streams to a deterministic state derived from (seed, epoch, plan hash).
3. Hand-over: kept events of older epochs that still sound after the freeze window end with their own fade-out at a deterministic per-event point uniform over [freeze + fade-out, freeze + 1.5 s]; the renderer applies the same cut to events already handed to it. The new plan's slots start from its stationary on/off state (on slots fade in over the same span, mid-segment; off slots wait a stationary residual off time from the end of the span). Count normalization (g_bnorm) is per epoch, so old and new talkers each keep their own plan's normalization (revised during implementation, `IMPLEMENTATION_NOTES.md` §12).
4. It continues planning. In offline/deterministic scenarios, plan changes are timestamped in the scenario file, so the result is reproducible.

### 4.7 MultiVoice (fixed-count continuous) planner variant

For `MultiVoiceMask` and `LaboratoryMask.continuousN`:
- V = K slots. Each slot is always ON.
- At the end of each segment, the next segment of a **different speaker** (MultiVoice) or the **same speaker** (Laboratory continuousN) is started with a 150 ms (MultiVoice) or 20 ms (Lab) equal-power overlap.
- Segment lengths use the same log-normal distribution (median 8 s, σ_ln 0.4 [E]).
- k_a = K always, and k_s fluctuates only due to within-segment pauses ≤ maxGap.

---

## 5. Talker-count modes

| Mode (GUI) | Engine interpretation (default strategy) | Pool P | Mean m | Min | Max (V) | Laboratory continuousN |
|---|---|---|---|---|---|---|
| 1 | MultiVoice K = 1 | 4 | 1 | 1 | 1 | N = 1 |
| 2 | MultiVoice K = 2 | 6 | 2 | 2 | 2 | N = 2 |
| 3 | MultiVoice K = 3 | 7 | 3 | 3 | 3 | N = 3 |
| 4 | MultiVoice K = 4 | 8 | 4 | 4 | 4 | N = 4 |
| 5 | MultiVoice K = 5 | 10 | 5 | 5 | 5 | N = 5 |
| 7 | MultiVoice K = 7 ("research-derived starting point", not optimal) | 12 | 7 | 7 | 7 | N = 7 |
| 8 | Stochastic | 18 | 8 | 6 | 10 | N = 8 |
| 12 | Stochastic | 26 | 12 | 9 | 15 | N = 12 |
| 16 | Stochastic | 34 | 16 | 12 | 20 | N = 16 |
| 24 | Stochastic | 48 | 24 | 18 | 30 | N = 24 |
| Custom | Any valid (P, m, min, max) or K | 2–64 | 1–32 | 0–m | ⌈m⌉–48 | 1–64 |

Stochastic rule for mode N ≥ 8 [E]: min = round(0.75 N), max = round(1.25 N), P = max(2N + 2, max + 4), capped at the corpus speaker count.

If the corpus has fewer speakers than required, see `RELIABILITY.md` §3 (insufficient corpus).

---

## 6. Segment selection

### 6.1 Requirements → mechanisms

| Requirement | Mechanism |
|---|---|
| No immediate segment reuse | Global segment cooldown (§6.4) plus persistent shuffle |
| No same-talker immediate repetition | A speaker cannot be selected if it is currently active in any slot, or was among the last R_spk selected speakers (R_spk = max(1, ⌊(P − V)/2⌋)) |
| Persistent shuffle | Per-speaker permutation of start anchors, persisted across sessions (§6.3) |
| Weighted selection | Speaker weights from diversity mode × age boost × CVR solo-risk weight |
| Minimum cooldown | Per-speaker reuse cooldown and global segment cooldown |
| Randomized offsets where safe | Starts only at VAD phrase onsets preceded by ≥ 150 ms pause ("start anchors"). The anchor is randomized via the shuffle; mid-word starts are never used |
| Long-session behavior | Adaptive cooldowns (§6.4) scale to corpus size; texture never repeats as a block because the combination of co-talkers, overlaps, gains and positions is re-drawn every time |

### 6.2 Speaker selection

```text
eligible = { s ∈ Pool : s not active, s ∉ recentSpeakers(R_spk), s.healthy }
if eligible empty: relax recentSpeakers to R_spk/2, then to 0; log `selector.relaxed`
w(s) = w_div(s) · w_age(s) · w_cvr(s)
    w_age(s)  = min(1 + (t − lastUse(s)) / T_age, 3)          T_age = 60 s   [E]
    w_div(s)  = from diversity mode (§7), default 1
    w_cvr(s)  = mean solo-risk weight of s's remaining anchors (CVR §6)
pick s with probability w(s)/Σw   (single draw from stream `selector`)
```

### 6.3 Start-anchor selection (persistent shuffle)

- Each speaker has an ordered list of start anchors A_s (VAD onsets after a pause ≥ 150 ms, at least 1.0 s of speech following, and at least `segMin` of material before the recording end).
- `shuffle_s` is a Fisher–Yates permutation of A_s drawn from stream `selector.shuffle.<speakerId>.<cycle>`.
- The selector consumes `shuffle_s` sequentially, skipping anchors whose covering region is in global cooldown.
- When the list is exhausted, the cycle increments and the speaker is reshuffled, with the constraint that anchors from the last 20 % of the previous cycle may not appear in the first 20 % of the new one (swap repair).
- **Persistence:**
  - In normal (non-deterministic) mode, (cycle, position) per speaker and the global cooldown table are saved to `selector_state.json` every 60 s and on stop. They are restored on start if the corpus version matches. This prevents "the same first minutes every morning".
  - In deterministic mode, persistence is disabled and state derives only from the seed.

### 6.4 Cooldowns

| Cooldown | Default | Adaptive rule | Label |
|---|---|---|---|
| Slot re-entry | Character-dependent (0.3–1.5 s) | — | [E] |
| Per-speaker reuse | Count-based R_spk (above) + time-based ≥ 10 s | — | [E] |
| Global segment (material region) | 45 min | T_seg,eff = min(45 min, 0.8 · M_elig / r_consume), minimum 5 min, where M_elig = usable speech of all speakers eligible under the diversity mode (pool rotation, §7.3, cycles them through the pool) and r_consume = m (seconds of material consumed per second). Warn `corpus.smallForCooldown` if T_seg,eff < 30 min | [E] |

A region is the processed audio span actually played (start anchor → end). Any anchor lying inside a cooled-down region is skipped.

**Material arithmetic (informative):** with m = 6.5, the engine consumes about 6.5 s of speech per second. A 24-speaker × 15 min corpus (6 h), cycled through the pool by rotation, gives T_seg,eff ≈ 44 min. Reuse is then unavoidable, and it is made imperceptible by different start anchors, different co-talkers, different gains and positions. Larger corpora raise T_seg,eff proportionally (`CORPUS.md` §1).

---

## 7. Voice diversity

### 7.1 Voice feature space

Per speaker (from corpus metadata, `CORPUS.md` §5):

| Feature | Unit | Weight in distance | Evidence |
|---|---|---|---|
| F0 median | semitones re 100 Hz | 1.00 | Target/masker F0 similarity strongly affects segregation [R: Brungart 2001; Darwin & colleagues] |
| F0 range (p95 − p5) | semitones | 0.50 | Prosodic variability [E] |
| Speaking rate | syllable nuclei / s | 0.50 | [E] |
| Spectral centroid of speech frames | log2 Hz | 0.50 | Vocal-tract/voice quality proxy [E] |
| LTASS shape PC1..PC3 | corpus-PCA z-scores | 0.25 each | Spectral similarity [E] |
| Language | categorical | +1.0 if different (only when "language-aware" enabled) | Linguistic informational masking [R, magnitude E] |

All continuous features are z-scored over the corpus. Distance: d(a, b) = √(Σ w_i (z_ai − z_bi)²) + languageTerm.

### 7.2 Diversity modes

| Mode | Pool construction | Purpose |
|---|---|---|
| **Low** | P nearest speakers to the corpus medoid (in feature space), then shuffled | Homogeneous voices. Research on similarity; a smoother blend (fewer standout voices) |
| **Balanced** | Stratify speakers into F0-median quartiles and draw ⌈P/4⌉ per quartile at random (weights 1); ensures a 50/50 lower/higher voice balance | General use with predictable coverage of the voice range |
| **High** (GUI default) | Greedy farthest-point sampling: seed = random speaker, then repeatedly add a random choice among the 3 speakers maximizing min-distance to the chosen set. Speakers beyond 2.5 SD from the corpus centroid in F0 median are excluded (outlier voices segregate easily) | Broad variety without extreme outliers. **Not** "maximum diversity" |
| **Research / Matched** | The user supplies a `TargetVoiceProfile` (F0 median distribution as quantiles and an optional LTASS). For each of P target quantiles, pick the nearest unused speaker | Deliberately maximizes target–masker similarity (hypothesized to increase informational masking); validation required [E] |

Voice balance display (GUI): lower voices = fraction of the pool with F0 median below the corpus median. There is no demographic labelling.

### 7.3 Pool rotation

The pool is not static for long runs. Every 20 min [E], the planner replaces ⌈P/4⌉ pool speakers (the least recently used first) with eligible non-pool speakers chosen by the same diversity rule (same F0 quartile for Balanced; the nearest-to-replaced in feature space for Low/High/Matched). Speakers leave the pool only when they are not active. Rotation spreads material consumption over the whole eligible corpus, and it is disabled in LaboratoryMask.

Diversity changes (re)build the pool on the control thread. Active talkers finish their segments, and new selections use the new pool.

```json
// TargetVoiceProfile (Matched mode), file *.bfvoice.json
{ "schema": "babbleforge.voiceprofile/1",
  "name": "Office A staff (estimated)",
  "f0MedianQuantilesHz": [98, 112, 125, 180, 205, 225],
  "ltassThirdOctaveDb": null,
  "notes": "Estimated; V2 may learn this from measurement" }
```

---

## 8. Source preloading

### 8.1 Pipeline (decode pool threads)

```text
event → open cache file (FLAC, seektable) → seek to anchor − 50 ms
      → decode (float) → resample to engine rate if ≠ 48 kHz (r8brain, offline quality)
      → pause shortening (pauses > maxGap reduced to maxGap using 10 ms equal-power splice
        centred in the pause)
      → write into slot's block chain; publish readyFlag / write index (release store)
```

- Pause shortening uses the recording's VAD pause list. The resulting sample-accurate mapping of speech regions (the new VAD mask) is written to the event's `SpeechMask` for k_s ground truth.
- Preloading never modifies the corpus files.

### 8.2 Buffers and quantities

| Item | Value | Label |
|---|---|---|
| Block size | 16 384 samples float mono (64 KiB, 341 ms at 48 kHz) | [I] |
| Block pool | Preallocated at `prepare`: default 128 MiB (2048 blocks); configurable 32–1024 MiB | [I] |
| Per-event initial fill before READY | fadeIn + 1.0 s (at least 1.25 s) | [I] |
| Per-active-slot target buffered ahead | 8.0 s | [I] |
| Low-water mark (priority boost) | 3.0 s | [I] |
| Warning mark | 1.0 s → `preload.low` diagnostic | [I] |
| Critical | < fast-fade (20 ms) + one block of the device buffer → RT fast-fade and REPLACE (§3.1) | [I] |
| Upcoming events | Fully buffered up to 2.0 s beyond fadeIn before their start | [I] |
| Decode threads | 2 default (1–4), below-normal priority; one extra "urgent" lane for events < 1 s from start | [I] |
| Scheduling | Earliest-deadline-first across slots (deadline = time buffered data runs out) | [I] |

**Memory estimate** at 48 kHz: V = 24 slots × (8 s + 2 s upcoming) × 192 KB/s ≈ 46 MB. The default pool of 128 MiB covers V ≤ 60 at 48 kHz or V ≤ 30 at 96 kHz. The pool is sized automatically at `prepare`: pool = max(128 MiB, 1.5 × V × 10 s × fs × 4 B).

### 8.3 Behavior when preloading falls behind

| Condition | Behavior | Determinism impact |
|---|---|---|
| Upcoming event not READY at start time | Postpone start in 100 ms steps (≤ 1 s), `lateStart++` | Breaks bit-exactness of this session (logged) |
| Not READY after 1 s | Event → REPLACE; `starvation++`; min-count may be violated briefly | Same |
| Active slot below warning mark | Priority boost; diagnostic `preload.low` | none |
| Active slot underflow imminent | 20 ms fast fade-out, REPLACE | Same |
| Sustained (> 5 starvations/min) | Engine DEGRADED `preload.behind`; planner reduces the horizon load by lengthening segments (median ×1.5) until recovered | Same |

In `bfrender`, preloading is synchronous (the renderer waits), so starvation cannot occur and output is deterministic.

---

## 9. RT voice renderer (per block)

```text
for each sub-block (≤ 256 frames):
    drain event FIFO (≤ 32 events/sub-block) → arm slots (stale epoch → drop)
    for each non-free slot v:
        compute per-sample gain  g[n] = currentGain(n) · fade(n)          // smoothed
        read samples from block chain (wraps blocks; no locks, acquire load of write index)
        for each channel c with outGains[v][c] > 1e-5:
            babble[c][n] += g[n] · outGains[v][c](n) · x[n]            // gains smoothed 50 ms
        update readPos, state transitions (§3.1), publish per-slot k_a/k_s flags
    babble[c][*] *= g_bnorm · g_btrim  (both smoothed)
```

- Cost: ~(2 + 2·N_nz) flops per sample per slot, where N_nz is the number of nonzero channel gains (typically 2–3).
- Per-slot published data for analysis (lock-free, per sub-block): active flag, speaking flag (from the SpeechMask), current gain (dB). These feed exact occupancy statistics.
