# BabbleForge V1 — Reliability: State Machine, Failure Handling, Logging, Diagnostics, Privacy

Covers brief sections 43, 44, 45, 50, 51, 52, 53. Labels are defined in `ENGINE.md` §0.1.

---

## 1. Engine state machine

### 1.1 States

| State | Meaning | Audio output |
|---|---|---|
| STOPPED | No device open, no graph | None |
| PREPARING | Opening device, allocating graph, building plan/kernels, initial preload | None |
| READY | Graph prepared, device open, callback running and outputting silence | Silence |
| STARTING | Fade-in (500 ms) from silence to the masker | Ramping |
| RUNNING | Normal operation | Masker |
| DEGRADED | Running with a reported problem (see §2); masking continues | Masker (possibly reduced/fallback) |
| DEVICE_LOST | The output device disappeared or failed; graph retained, callback not running | None |
| STOPPING | Fade-out (300 ms) then device stop/release | Ramping to silence |
| ERROR | Unrecoverable condition (invalid config with no LKG, corrupt install, allocation failure); requires user action | None |

The reserved output-source modes (MASKER / CALIBRATION / MUTE) are orthogonal to the state. CALIBRATION is only legal in READY/RUNNING/DEGRADED (`V2_EXTENSION_POINTS.md` §3).

### 1.2 Diagram

```text
                 start()                      prepared                 play()
   ┌─────────┐ ─────────► ┌───────────┐ ─────────────► ┌───────┐ ─────────► ┌──────────┐
   │ STOPPED │            │ PREPARING │                │ READY │            │ STARTING │
   └─────────┘ ◄───────┐  └─────┬─────┘                └───┬───┘            └────┬─────┘
        ▲              │        │ fatal                    │ stop()              │ fade done
        │              │        ▼                          ▼                     ▼
        │              │   ┌───────┐   user reset     ┌──────────┐  problem  ┌─────────┐
        │              └── │ ERROR │ ◄─────────────── │ STOPPING │ ◄──────── │ RUNNING │ ◄─┐
        │                  └───────┘   (fatal in any  └────┬─────┘  stop()   └──┬───┬──┘   │
        │                              state)              │                    │   │      │ resolved
        └──────────────────────────────────────────────────┘                    │   ▼      │
                                                                  device lost   │ ┌──────────┐
                                ┌──────────────┐ ◄──────────────────────────────┘ │ DEGRADED │
                                │ DEVICE_LOST  │ ◄─────────────────────────────── └──────────┘
                                └──────┬───────┘   device lost
                                       │ reconnect() (user) / device returns + autoReconnect
                                       ▼
                                  PREPARING
```

The one-click "Start Masking" performs start() → PREPARING → READY → play() → STARTING → RUNNING automatically.

### 1.3 Legal transitions

| From → To | Trigger | Notes |
|---|---|---|
| STOPPED → PREPARING | `start()` | — |
| PREPARING → READY | Graph prepared and the first events preloaded (or 3 s preload timeout) | — |
| PREPARING → ERROR | Device open failed with no fallback allowed, invalid config and no LKG, allocation failure | Device-open failure → ERROR `device.openFailed`; the user may choose a device |
| PREPARING → STOPPED | `stop()` during prepare | Abort cleanly |
| READY → STARTING | `play()` (automatic after start unless "prepare only") | — |
| READY → STOPPING | `stop()` | — |
| STARTING → RUNNING | Fade-in complete | — |
| STARTING/RUNNING → DEGRADED | Any DEGRADED condition (§2) | Audio continues |
| DEGRADED → RUNNING | All degraded conditions cleared for ≥ 30 s | Hysteresis |
| RUNNING/DEGRADED/STARTING/READY → DEVICE_LOST | Device removed/error callback | Audio already stopped by the driver; the engine does not attempt other devices |
| DEVICE_LOST → PREPARING | User "Reconnect", or the same device (same type + name + channel count) reappears **and** `autoReconnectSameDevice` = true (default true) | Never to a different device |
| DEVICE_LOST → STOPPED | `stop()` | — |
| RUNNING/DEGRADED/STARTING → STOPPING | `stop()`, preset change requiring a rebuild (rate, layout), sample-rate change | A rebuild continues STOPPING → PREPARING automatically |
| STOPPING → STOPPED | Fade-out complete and resources released | — |
| STOPPING → PREPARING | Rebuild requested | — |
| any → ERROR | Fatal (§2) | — |
| ERROR → STOPPED | User acknowledges / `reset()` | — |

Illegal transitions are rejected: the command returns `IllegalTransition`, is logged at WARN, and the state is unchanged.

### 1.4 Asynchronous status to the UI

```cpp
struct EngineStatusEvent {
    uint64_t    seq; TimePoint wall; uint64_t engineSample;
    EngineState state, previous;
    uint32_t    degradedReasons;        // bitfield
    std::string headline;               // "MASKING ACTIVE", "OUTPUT DEVICE LOST", …
    std::string detailCode;             // "corpus.reduced", "limiter.sustained" …
};
```

- Delivered on the message thread via a Control → UI queue drained by a 30 Hz timer.
- Coalesced: only the latest state plus a list of new reason codes.
- The UI never polls the engine directly for state.

---

## 2. Failure handling

| Failure | Detection | Immediate behavior | Engine state | Log | User message |
|---|---|---|---|---|---|
| Missing source file | Preloader open fails | Event → REPLACE (substitute a different recording of the same speaker, else another speaker); recording marked unhealthy | RUNNING; DEGRADED `corpus.reduced` if > 10 % of pool recordings are unhealthy | WARN (rate-limited) | None, or "Reduced Voice Library" |
| Source decode failure (corrupt FLAC, CRC) | Decoder error | Same as missing: substitute; mark recording after 2 failures | Same | WARN | Same |
| Insufficient corpus (below plan requirement) | Plan build, or after health changes | Build the reduced plan (§3) | DEGRADED `corpus.insufficient` | WARN | "Reduced Voice Library" |
| No corpus at all | Plan build | Babble strategies not runnable; per policy (§4) | Continuous/Safe: RUNNING with stationary + DEGRADED `corpus.none`; Strict: ERROR | ERROR/WARN | "Voice library unavailable — using steady masking" |
| Preload starvation | RT late-start/starvation messages | `TALKER_ENGINE.md` §8.3 | DEGRADED `preload.behind` if > 5/min | WARN | "Audio source is slow" (Advanced) |
| Audio device disappearance | Device callback/error | Stop output (already silent); keep state; **no switch to another device** | DEVICE_LOST | ERROR | "OUTPUT DEVICE LOST" + Reconnect / Choose Device |
| Device open failure | Open returns error | — | ERROR `device.openFailed` | ERROR | Device chooser |
| Buffer underrun (xrun) | §7 of `REALTIME_ARCHITECTURE.md` | Continue | DEGRADED only if sustained | WARN (rate-limited) | Advanced counter |
| Sample-rate change (device-initiated) | `audioDeviceAboutToStart` with a new rate | STOPPING → PREPARING at the new rate; kernels redesigned; correction reset | Transient | INFO | None |
| Invalid preset (load) | Schema/type error | Reject the file; keep the current config; offer LKG | Unchanged | WARN | "Preset could not be loaded" |
| Invalid config at startup | Parse/validation fails | Load `last_known_good.json`; if that fails, the factory default | RUNNING/READY as normal | ERROR | "Previous settings were invalid; restored last working settings" |
| Limiter overload | `ENGINE.md` §6 alarms | Continue audio | DEGRADED `limiter.overload` | ERROR | "Output limiting — reduce Strength" |
| Safety clip | Clip counter > 0 | Continue | DEGRADED `output.clip` | ERROR | Same |
| Filter design failure | Verification fails | Keep the previous kernel; retry with longer taps | DEGRADED `spectrum.design` if persistent | WARN | Advanced |
| Spectral correction saturates (≥ ±4 dB) | Analysis | Continue | RUNNING (advisory) | INFO | Advanced "Rebuild analysis recommended" |
| Analysis thread stalled | Watchdog (no heartbeat 10 s) | Freeze trims/correction (hold values); audio continues | DEGRADED `analysis.stalled` | ERROR | Advanced |
| Planner stalled | Lookahead < 1 s and no heartbeat 5 s | Voices finish current segments; min count may drop; policy fallback | DEGRADED `planner.stalled` | ERROR | "Reduced Voice Library" |
| RT callback stalled (driver) | No callback for 2 s while RUNNING | Treat as device failure | DEVICE_LOST | ERROR | "OUTPUT DEVICE LOST" |
| Disk full (logs/state) | Write error | Stop writing logs (in-memory ring continues); state files skip | RUNNING (advisory) | ERROR once | Advanced |
| Out of memory at prepare | Allocation failure | Reduce the pool to the minimum and retry once | ERROR if the retry fails | ERROR | "Not enough memory" |
| Crash | OS exception handler | Minidump + crash log; next launch offers restore of `session.json` | — | FATAL | Next launch notice |

**Substitution determinism:** substitutions draw from the `selector` stream, so offline renders stay deterministic. In real time, substitutions caused by I/O failure break determinism (logged).

---

## 3. Insufficient corpus: reduced plans

Requirement per plan: speakersNeeded = max(P_plan, maxActive + 4), with LaboratoryMask needing exactly N distinct speakers.

| Available speakers S | Reduced plan |
|---|---|
| S ≥ speakersNeeded | Normal |
| maxActive + 1 ≤ S < speakersNeeded | P = S; recent-speaker rule relaxed (`TALKER_ENGINE.md` §6.2); DEGRADED `corpus.insufficient` |
| 2 ≤ S ≤ maxActive | max = S − 1 (so no speaker is duplicated simultaneously), mean scaled by (S − 1)/max_original, min scaled; **if the fallback policy allows stationary compensation**, the stationary fraction increases by Δ = 0.5 × (1 − (S − 1)/max_original), capped at 0.6 total |
| S ≤ 1 | Babble impossible → policy (§4) |

- The same speaker is never played in two slots at once. That would create a "chorus" artefact.
- Stationary compensation keeps the output level constant because the mixer is energy-preserving.

---

## 4. Fallback policies

| Policy | Major configuration/corpus problem | Individual talker source failure | Babble impossible (no usable corpus) | Intended use |
|---|---|---|---|---|
| **Strict** | Stop: enter ERROR with a detailed reason; no audio | Offline: abort render. Real time: stop → ERROR | ERROR | Research/Laboratory, validation, experiments where silent substitution would invalidate data |
| **Safe** | Switch immediately to `StationarySpeechMask` (same spectrum target, same level) and stay there until the user acts | Substitute as in §2; if > 10 % of the pool fails → switch to stationary | Stationary | Deployments where predictability beats naturalness |
| **Continuous** (default) | Keep babble with a reduced plan (§3), increasing the stationary share per §3; switch to stationary only if babble is impossible; auto-return to the normal plan when the corpus recovers (rescan or re-plug of the drive) | Substitute; continue | Stationary | Unattended operation (default) |

**Default: Continuous.**
- The product's purpose is uninterrupted masking. A masker that silently stops (Strict) or permanently changes character on a minor fault (Safe) creates privacy gaps and support calls.
- Continuous always keeps some masking and changes the character only as much as the fault requires.
- It reports every change through DEGRADED.
- LaboratoryMask forces Strict regardless of the setting, because research validity requires it.

---

## 5. Logging

### 5.1 Architecture

- Structured **JSON Lines** logs written by the Logger thread.
- Sources: non-RT threads log via a bounded MPSC queue (drop-oldest on overflow with a `logDropped` counter). The RT thread logs only POD event codes plus up to 4 numeric args via its SPSC FIFO; the logger formats them.
- Files: `logs/babbleforge-YYYYMMDD.jsonl`, rotated at 10 MB, keep 10 files (≤ 100 MB total), and the oldest are deleted.
- Levels: TRACE (dev builds only), DEBUG, INFO, WARN, ERROR, FATAL. Default INFO.
- Every entry: `ts` (UTC ISO-8601 ms), `mono` (steady-clock µs), `engineSample`, `level`, `code`, `thread`, `msg`, `data` (object).

### 5.2 Required events

| Category | Codes |
|---|---|
| Engine | `engine.start`, `engine.stop`, `engine.state` (every transition, with reason), `engine.degraded`, `engine.recovered` |
| Session | `session.seed` (seed, corpusVersion, dataSetHash, appVersion) at start |
| Presets | `preset.load`, `preset.change` (diff of changed fields), `preset.invalid`, `preset.lkgRestored` |
| Device | `device.open`, `device.close`, `device.lost`, `device.returned`, `device.rateChange`, `device.bufferChange`, `device.openFailed` |
| Corpus | `corpus.loaded` (version, counts), `corpus.fileMissing`, `corpus.decodeFailed`, `corpus.reduced`, `corpus.insufficient`, `corpus.import.*` |
| Audio health | `audio.xrun` (rate-limited), `audio.overrun`, `preload.lateStart`, `preload.starvation` |
| Output | `limiter.sustained`, `limiter.overload`, `output.clip` |
| Config | `config.invalid`, `config.clamped` (validation adjustments) |
| Spectrum | `spectrum.correctionLarge`, `spectrum.designDegraded`, `spectrum.eqClamped`, `spectrum.stationaryOffTarget` |
| Watchdog | `watchdog.stall.<thread>` |

### 5.3 What is never logged

- Raw audio of any kind: corpus samples, output samples, or microphone input (V1 has no microphone).
- Speech content, transcripts or any recognized text.
- Full file paths when **path redaction** is on (§8). By default, paths are logged locally but redacted in exports.

---

## 6. Diagnostics

### 6.1 Diagnostic snapshot (on demand and on every ERROR)

```json
{
  "schema": "babbleforge.diagnostics/1",
  "generated": "2026-09-30T15:40:12.331Z",
  "app": { "version": "1.0.0", "build": "a1b2c3d", "os": "Windows 11 23H2", "cpu": "Intel i5-12400", "ramGB": 16 },
  "engine": { "state": "DEGRADED", "degradedReasons": ["corpus.reduced"], "uptimeS": 86412,
              "sessionSeed": 9138812201, "corpusVersion": "3f9a1c0d77e24b1a", "dataSetHash": "5d1e…",
              "determinismBroken": false, "planEpoch": 7 },
  "audio": { "driver": "ASIO", "device": "Focusrite USB ASIO", "sampleRate": 48000, "bufferFrames": 256,
             "outputs": 8, "latencyMs": 5.3,
             "dspLoad": { "p50": 0.031, "p99": 0.058, "max": 0.121 },
             "xruns": 0, "overruns": 0, "callbackGaps": 0 },
  "cpu": { "processPct": 3.2 },
  "talkers": { "strategy": "balanced", "meanActiveTarget": 6.5, "meanActive60s": 6.41,
               "meanSpeaking60s": 5.02, "activeNow": 7, "slots": 9, "pool": 14,
               "occupancy60s": 0.991, "soloExposurePct": 0.4 },
  "preload": { "minBufferedS": 6.2, "p99ReadLatencyMs": 11, "lateStarts": 0, "starvations": 0,
               "blockPoolUsedPct": 22 },
  "corpus": { "speakers": 48, "usableSpeechH": 11.4, "unhealthyRecordings": 3, "segmentCooldownEffMin": 45 },
  "spectrum": { "target": "ltass_universal", "babbleRmsDevDb": 0.8, "stationaryMaxDevDb": 0.3,
                "correctionMaxDb": 1.9, "correctionState": "tracking" },
  "limiter": { "enabled": true, "ceilingDbtp": -1.0, "grNowDb": 0.0, "grMax60sDb": 0.0,
               "pctTimeActive60s": 0.0, "sustained": false, "clipEvents": 0 },
  "outputs": [ { "index": 0, "label": "Ceiling 1", "enabled": true, "rmsLeq60Dbfs": -26.2,
                 "truePeakMaxDbtp": -9.8, "gainDb": 0.0, "mute": false, "zone": 0 } ],
  "correlation": { "maxAdjacent": 0.12, "median": 0.05 },
  "counters": { "substitutions": 12, "logDropped": 0, "tapOverflow": 0 },
  "recentLog": [ "…last 200 WARN+ entries (redacted per settings)…" ]
}
```

Export: "Save diagnostics" writes a `.zip` with the snapshot, the last 3 log files, `settings.json` and the current preset (all redacted per settings). It never includes audio or corpus files.

### 6.2 Health model

| Indicator | Good | Warning | Bad |
|---|---|---|---|
| DSP load p99 | < 50 % | 50–80 % | > 80 % |
| Preload min buffered | > 3 s | 1–3 s | < 1 s |
| Xruns / min | 0 | 1–10 | > 10 |
| Limiter | inactive | active > 1 % | sustained/overload |
| Corpus | healthy | reduced | insufficient/none |

The GUI status "Output healthy" = no Bad indicator and no ERROR.

### 6.3 Watchdog (1 Hz)

- Checks thread heartbeats:
  - RT: callback counter advancing while RUNNING
  - Planner: lookahead ≥ 1 s
  - Analysis: heartbeat ≤ 10 s
  - Preloader: queue age ≤ 5 s
  - Logger: queue ≤ 90 %
- Checks memory and handle counts (warn on > 20 % growth versus the 1 h baseline).
- Triggers the persistence timers (`PRESETS.md` §9).
- Never touches RT state directly. It acts via Control commands.

---

## 7. Telemetry

- **Default: none.** V1 contains no network code in the engine and no cloud telemetry.
- If telemetry is ever added:
  - it must be opt-in
  - it must live in a separate process or module with no access to audio buffers, corpus files or paths
  - its failure must be unable to affect audio operation (separate thread, bounded queues, no blocking calls from engine threads)
- The engine never requires Internet access, including for licensing, updates, corpus import or presets.

---

## 8. Security and privacy

| Requirement | Implementation |
|---|---|
| Fully local | No network APIs linked into the engine. CI check: the engine binary imports no socket/HTTP symbols (the updater, if any, is a separate optional executable) |
| No corpus audio leaves the machine | No export path writes corpus audio except explicit user render (`bfrender`), which writes only the masker output |
| No microphone in V1 | No input device is opened; the input channel count is 0 in device setup |
| Path redaction | Setting `redactPathsInExports` (default **on**): exported logs/diagnostics replace paths with `<corpus>/…/<hash8>.flac` and user names with `<user>`. Local logs keep full paths unless `redactPathsInLogs` (default off) |
| Speaker identifiers | `external_id` values are exported only as hashes when redaction is on |
| File permissions | Data directories are created user-private (Windows: per-user AppData ACLs) |
| Untrusted inputs | Preset/profile/scenario JSON are parsed with size limits (≤ 10 MB) and schema validation; audio files are decoded by maintained decoders; ingestion runs in the background with a per-file timeout (120 s) and memory cap (2 GB), and failures are isolated per file |
| Integrity | Corpus cache files carry the FLAC MD5; the metadata DB stores PCM hashes; a mismatch marks the recording unhealthy |
