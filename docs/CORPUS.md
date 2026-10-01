# BabbleForge V1 — Speech Corpus

Covers brief sections 12, 13, 14 (and corpus parts of 47, 48). Labels are defined in `ENGINE.md` §0.1.

---

## 1. Corpus requirements

### 1.1 Size

| Tier | Distinct speakers | Usable speech per speaker | Total usable | Status |
|---|---|---|---|---|
| Minimum (production) | 24 | ≥ 10 min | ≥ 4 h | Engine runs; small-corpus warnings may appear for dense presets |
| Recommended | 50+ | ≥ 15 min | ≥ 12.5 h | Target for shipped/recommended packs |
| Research | any | any | any | Laboratory mode checks per-render requirements only |
| Supported maximum | ≥ 10 000 speakers, ≥ 2 000 h | — | — | Metadata in SQLite; runtime index is compact (§6) |

The engine must never assume the corpus fits in RAM.

**Why these numbers [E]:**
- Speakers: the Large Distributed mode may need 50+ simultaneous speaker identities. The pool must exceed max-active by ≥ 4 to satisfy the no-repeat rules (`TALKER_ENGINE.md` §6).
- Duration: material consumption is ≈ m seconds per second, so T_seg,eff ≥ 30 min at m = 6.5 needs about 4.1 h of eligible speech.

Per-preset requirement checks are in `RELIABILITY.md` §3.

### 1.2 Speaker composition [E]

- Roughly balanced lower/higher F0 (each F0-median half holds ≥ 35 % of speakers).
- Varied speaking rates and prosody.
- Conversational or read continuous speech. Isolated-word corpora are unsuitable (too many pauses and too little co-articulation).
- **No synthetic duplication:** a production multi-talker mode must not derive "different speakers" by pitch-shifting one speaker [R: Brungart — voice characteristics drive segregation; synthetic duplicates share articulation and timing].
- Languages are recorded as metadata. Mixed-language corpora are allowed. Language-aware selection is optional (`TALKER_ENGINE.md` §7).

### 1.3 Audio quality requirements

| Check | Good | Usable (warning) | Reject | Label |
|---|---|---|---|---|
| Sample rate (source) | ≥ 44.1 kHz | 32 kHz | < 32 kHz (the 8 kHz octave band's upper edge is 11.3 kHz; below 32 kHz the Nyquist limit ≤ 16 kHz is marginal, and < 22.05 kHz loses the band) | [I] |
| Effective bandwidth (−30 dB point of the speech LTASS re its peak) | ≥ 11 kHz | 7.5–11 kHz | < 7.5 kHz (telephone/codec band-limited) | [I] |
| Bit depth | ≥ 16-bit PCM or lossless | Lossy (MP3/AAC/Opus ≥ 128 kbps) flagged `lossy` | Lossy < 96 kbps | [I] |
| Clipping | clipped-sample ratio < 1e-5 | 1e-5 … 1e-4 | > 1e-4, or any clipped run > 3 ms | [I] |
| SNR (active speech level − noise floor) | ≥ 35 dB | 25–35 dB | < 25 dB | [E] |
| Speech activity ratio (VAD speech / file duration) | ≥ 60 % | 30–60 % | < 30 % (too much silence or non-speech) | [E] |
| DC offset | \|mean\| < 0.001 FS | ≤ 0.01 (removed at ingest) | — | [I] |
| Reverberation (estimated) | clean/close-mic | moderate (flag) | — (not rejected; flagged `reverberant`) | [E] |
| Music/non-speech content | none | — | > 10 % non-speech events detected (VAD speech prob. mid-range + high spectral flatness) → review | [E] |
| Minimum file duration | ≥ 30 s | 10–30 s | < 10 s | [I] |

Clipping detection (revised during implementation):
- a sample is counted clipped if |x| ≥ 0.999 FS, or if ≥ 3 consecutive samples are within 0.01 dB of the file's max |x| (captures clipping at non-full-scale levels after gain changes)
- flat-top rule: consecutive samples equal within 2^-17 (note: low-frequency (<~20 Hz) sines may still false-trigger)
- clipped runs are merged when separated by < 1 ms

---

## 2. Storage layout

```text
<corpusRoot>/                          (user-selectable; default %LOCALAPPDATA%/BabbleForge/Corpus)
├── corpus.sqlite                      metadata DB (§5)
├── manifest.json                      corpus version, analyzer version, counts, hash
├── cache/
│   └── <speakerId>/<recordingId>.flac processed 48 kHz mono 24-bit FLAC, seektable every 1 s
├── source_links.json                  original file paths (optional; redactable)
└── selector_state.json                persistent shuffle state (TALKER_ENGINE §6.3)
```

- **Cache format:** 48 kHz, mono, 24-bit FLAC, compression level 5, seek points every 48 000 samples. FLAC gives about 50–60 % size and cheap decode (≈ 0.3 % of one core per stream). Seeking cost is < 1 ms with the seektable.
- **Originals are never modified or required after import.** A "keep originals referenced" option records paths for re-analysis.
- **Corpus version:**
  ```
  corpusVersion = SHA-256( analyzerVersion ‖ sorted(recordingId ‖ pcmHash ‖ segmentTableHash) )
  ```
  printed as its first 16 hex chars. It is used by deterministic mode and presets.

Disk estimate: 12.5 h of source (50 × 15 min) → cache ≈ 12.5 h × 144 KB/s × 0.55 ≈ 3.6 GB.

---

## 3. Ingestion pipeline (Corpus Analyzer)

Runs on a background thread pool (default: hardware threads − 2, min 1, below-normal priority). It never runs on or blocks the audio thread. It may run while masking is active; the engine keeps using the previous corpus version until the new one is committed atomically (§3.12).

```text
Decode → Resample → Channel normalize → DC removal → VAD → Speech segmentation
      → Silence/pause analysis → Level (peak, RMS, P.56 ASL) → Spectral analysis (LTASS)
      → F0 statistics → Speaking rate → Quality analysis → Duplicate detection
      → Metadata DB write → Cache write → Commit
```

### 3.1 Decode

- Formats: WAV/BWF, AIFF, FLAC, Ogg Vorbis, Opus, MP3, M4A/AAC (via JUCE `AudioFormatManager` and platform codecs).
- Decode to float32. Files that fail to decode are rejected with the reason code `decode.<error>`.

### 3.2 Resample

To 48 kHz using r8brain-free-src (MIT) at 24-bit quality (≥ 120 dB stopband, transition band starting at 0.45 fs). If the source is already 48 kHz, there is no processing.

**Engine rates ≠ 48 kHz:** resampled at preload time (`TALKER_ENGINE.md` §8), not stored.

### 3.3 Channel normalization

- Mono: as is.
- Stereo/multichannel: compute the inter-channel correlation ρ over speech frames.
  - ρ ≥ 0.9 → average the channels (energy-preserving sum × 1/√2 correction is **not** applied; a plain mean keeps level for correlated content)
  - ρ < 0.9 → select the channel with the higher VAD-estimated SNR, and flag `multichannelSelected`
- Files with > 2 channels: the same process over all channels.

### 3.4 DC removal

2nd-order high-pass at 20 Hz (zero-phase, offline, forward-backward).

### 3.5 VAD

| Option | Engine | Rate | Frame | Use |
|---|---|---|---|---|
| **Default** | WebRTC VAD (BSD-3-Clause, built in) | resampled to 16 kHz internally | 30 ms, aggressiveness 2 | Fast, deterministic, no model files |
| Optional "Accurate" | Silero VAD (MIT) via ONNX Runtime (MIT) | 16 kHz | 32 ms (512 samples) | Better in noise and music; plug-in module |
| Fallback | Energy + spectral-flatness VAD (in-house) | 48 kHz | 10 ms | Only if others are unavailable |

Post-processing (common) [E]:
1. Raw frame decisions → a 10 ms-resolution speech flag.
2. Hangover: extend speech by 150 ms after each speech run and 50 ms before (onset protection).
3. Merge speech runs separated by pauses < 80 ms (phonetic gaps, stop closures) → "speech regions".
4. Drop speech regions < 200 ms that are isolated by ≥ 500 ms pauses (clicks, breaths).

VAD is never run in the audio callback. The V1 runtime uses stored VAD masks only.

### 3.6 Speech segmentation

| Term | Definition | Stored |
|---|---|---|
| Pause | Gap between speech regions ≥ 80 ms | Pause list (start, end) per recording |
| Phrase boundary | Pause ≥ 150 ms | Flag on pause |
| Start anchor | Phrase-boundary end (speech onset) with ≥ 1.0 s of speech in the following 3 s, and ≥ 2 s of remaining audio | Anchor table |
| Utterance | Speech between pauses ≥ 700 ms | For statistics |
| Segment record | An anchor plus derived per-anchor stats: speech fraction in the next 10 s, longest continuous phrase, ASL of the next 10 s | Segment table |

Segments are **start points**, not fixed excerpts. The planner decides the durations. This gives effectively continuous variety in excerpt lengths.

### 3.7 Silence analysis

Per recording:
- pause-duration histogram (bins 80–150, 150–300, 300–600, 600–1000, 1000–2000, > 2000 ms)
- total pause time
- proportion of pauses above each maxGap setting (100/250/600 ms), so the engine can predict the speech fraction after pause shortening for each Character setting

### 3.8 Level measurement

| Metric | Method |
|---|---|
| Peak | max\|x\| (dBFS) |
| True peak | BS.1770 4× oversampled (dBTP) |
| RMS | Whole file (dBFS) |
| Active speech level (ASL) | ITU-T P.56 method B [S] (margin 15.9 dB, time constant 0.03 s, hangover 0.2 s) per recording and per segment record (10 s window after anchor) |
| Activity factor | P.56 activity (%) |
| LUFS-I | BS.1770 (informational; not used for normalization) |
| Noise floor | 10th percentile of 30 ms frame RMS over non-speech frames (dBFS) |
| SNR | ASL − noise floor |

**Normalization basis:** ASL [S], because the engine normalizes talkers by level while speaking, which matches the equal-level-talker construction of babble research [R].

### 3.9 Spectral analysis (LTASS)

- Speech frames only (VAD).
- Welch PSD with 4096-point Hann, 50 % overlap.
- Aggregated into 26 1/3-octave bands (50 Hz–16 kHz), in dB relative to the band power sum.
- Per recording and per speaker (power average over the speaker's recordings, weighted by speech duration).
- Also stored: spectral centroid (speech frames, power-weighted mean frequency) and effective bandwidth (§1.3).
- **Corpus-level PCA:** after commit, a PCA of speaker LTASS vectors (mean-removed dB over the 21 operating bands). The first 3 components' scores are stored per speaker for the diversity features. The PCA basis is stored in `manifest.json` with the analyzer version.

### 3.10 F0 statistics

- YIN (de Cheveigné & Kawahara 2002) at 16 kHz, 10 ms hop, 40 ms window, search range 60–400 Hz, aperiodicity threshold 0.15.
- Voiced frames only (YIN confidence plus VAD).
- Octave-error cleanup: median filter over 5 frames, and removal of values > 1 octave from the running 1 s median.
- Stored: median Hz, mean, p5, p95, range in semitones (p95/p5), voiced ratio, and a 12-bin histogram (semitone bins relative to 100 Hz, 2-semitone width).
- pYIN (probabilistic YIN) is an acceptable drop-in if validated as more accurate.

### 3.11 Speaking rate

Syllable-nuclei estimate (de Jong & Wempe 2009 approach): intensity peaks ≥ 2 dB above the surrounding dips, in voiced regions, ≥ 100 ms apart. Rate = nuclei / speech time (syll/s). Stored per recording and per speaker (weighted mean).

### 3.12 Quality analysis, scoring and commit

`qualityScore` ∈ [0, 100] = 100 − Σ penalties:

| Item | Penalty |
|---|---|
| SNR | 0 at ≥ 35 dB, linear to 40 at 25 dB |
| Bandwidth | 0 at ≥ 11 kHz, linear to 30 at 7.5 kHz |
| Clipping ratio | 0 at < 1e-5, 20 at 1e-4 |
| Speech ratio | 0 at ≥ 60 %, 20 at 30 % |
| Lossy | 10 |
| Reverberant | 10 |
| multichannelSelected | 5 |

Classes: Good ≥ 80, Usable 50–79, Rejected < 50 or any hard reject rule (§1.3).

- Usable recordings are included, but their selection weight is × (qualityScore/100).
- **Commit:** analysis results and cache files are written to a staging DB/directory, then atomically swapped (rename of the DB file plus a manifest version bump).
- The engine is notified via the control thread. It picks the new version at the next plan build. Running events finish on old cache files, which are kept until no reference remains (reference-counted file handles). Previous cache kept as cache.old-<version>, purged next import (revised during implementation).
- **Effective bandwidth (revised during implementation):** measured on 1/3-oct band levels.
- **Not yet implemented:** music/non-speech flag, overlap-based anchor exclusion, ECAPA check (revised during implementation).

### 3.13 Duplicate detection

| Level | Method | Action |
|---|---|---|
| Exact duplicate file | SHA-256 of decoded 16-bit 16 kHz mono PCM | Reject the second copy (`duplicate.exact`) |
| Near duplicate (re-encoded, trimmed, gain-changed) | Audio fingerprint: 32-bit sub-fingerprints from 33 band-energy differences at 11.6 ms hop (Haitsma & Kalker 2002 scheme), compared by bit-error rate over aligned ≥ 10 s windows; duplicate if BER < 0.20 | Reject or flag for review (`duplicate.near`) |
| Overlapping content (the same passage inside a longer file) | Same fingerprint, sub-sequence search (index of 3 s fingerprint blocks) | Flag; the overlapping region is excluded from anchors |
| Possible same speaker under different IDs | Speaker-level feature distance (F0 median ± 1 semitone, F0 range, LTASS PC distance < 0.5 SD, speaking rate) **and** optional speaker-embedding cosine similarity (ECAPA-TDNN via ONNX, optional module) > 0.75 | **Flag only** (`speaker.possibleDuplicate`); never auto-merged. The user confirms or dismisses |

---

## 4. Import review (engine side of GUI §54)

The ingestion job reports per file: class (Good/Usable/Rejected), reason codes, and key metrics. Summary: counts per class, total and usable duration, speakers, languages.

**Speaker identity sources**, in priority order:
1. an explicit `speakers.csv` (file → speakerId, language, optional labels)
2. folder name convention `<root>/<speaker>/<files>`
3. the filename pattern `<speaker>_*.ext` (configurable regex)
4. otherwise, one speaker per file, flagged `speaker.unknown`

---

## 5. Metadata schema (SQLite)

```sql
CREATE TABLE corpus_info (key TEXT PRIMARY KEY, value TEXT);   -- corpusVersion, analyzerVersion, created, pcaBasis(json)

CREATE TABLE speaker (
  speaker_id        INTEGER PRIMARY KEY,
  external_id       TEXT UNIQUE,          -- user/CSV id (may be redacted in exports)
  language          TEXT,                 -- BCP-47, e.g. "en-GB"; NULL = unknown
  n_recordings      INTEGER,
  usable_speech_s   REAL,
  f0_median_hz      REAL, f0_mean_hz REAL, f0_p5_hz REAL, f0_p95_hz REAL,
  f0_range_st       REAL,
  speaking_rate_sps REAL,
  spectral_centroid_hz REAL,
  ltass_third_oct_db BLOB,                -- 26 × float32
  ltass_pc1 REAL, ltass_pc2 REAL, ltass_pc3 REAL,
  asl_mean_dbfs     REAL,
  quality_mean      REAL,
  flags             INTEGER,              -- bitfield (possibleDuplicate, …)
  enabled           INTEGER DEFAULT 1,
  user_labels       TEXT                  -- JSON, free-form (never used by DSP)
);

CREATE TABLE recording (
  recording_id      INTEGER PRIMARY KEY,
  speaker_id        INTEGER REFERENCES speaker,
  source_path       TEXT,                 -- optional; redactable
  source_sha256     TEXT, pcm_sha256 TEXT,
  cache_file        TEXT,                 -- relative path
  source_sample_rate INTEGER, source_channels INTEGER, source_format TEXT, lossy INTEGER,
  duration_s        REAL, speech_s REAL, speech_ratio REAL,
  peak_dbfs REAL, true_peak_dbtp REAL, rms_dbfs REAL, asl_dbfs REAL, activity_pct REAL,
  lufs_i REAL, noise_floor_dbfs REAL, snr_db REAL, bandwidth_hz REAL,
  clip_ratio REAL, dc_offset REAL, reverberant INTEGER,
  f0_median_hz REAL, f0_p5_hz REAL, f0_p95_hz REAL, f0_hist BLOB,
  speaking_rate_sps REAL, spectral_centroid_hz REAL,
  ltass_third_oct_db BLOB,
  pause_hist BLOB, pause_frac_gt100 REAL, pause_frac_gt250 REAL, pause_frac_gt600 REAL,
  quality_score REAL, quality_class INTEGER, reason_codes TEXT,
  fingerprint BLOB,
  vad_engine TEXT, analyzer_version TEXT, analyzed_utc TEXT
);

CREATE TABLE speech_region (             -- run-length VAD, sample-accurate at 48 kHz
  recording_id INTEGER, start_sample INTEGER, end_sample INTEGER,
  PRIMARY KEY (recording_id, start_sample));

CREATE TABLE segment (                   -- start anchors
  segment_id        INTEGER PRIMARY KEY,
  recording_id      INTEGER REFERENCES recording,
  anchor_sample     INTEGER,             -- start (48 kHz)
  max_end_sample    INTEGER,             -- last usable sample for an excerpt from this anchor
  speech_frac_10s   REAL, longest_phrase_s REAL, asl_10s_dbfs REAL,
  solo_risk         INTEGER,             -- 1 if speech_frac > 0.9 and longest phrase > 4 s
  excluded          INTEGER DEFAULT 0    -- overlap duplicate or user exclusion
);
CREATE INDEX segment_by_rec ON segment(recording_id);
```

Fields required by the brief map as follows:

| Brief field | Schema column(s) |
|---|---|
| speaker_id, recording_id, segment_id, language | as named |
| duration | `recording.duration_s` |
| sample_rate | `source_sample_rate`; cache is always 48000 |
| peak, RMS | `peak_dbfs`, `rms_dbfs` |
| LUFS | `lufs_i` (info only) |
| speech_activity_ratio | `speech_ratio`, `activity_pct` |
| average_F0, F0_range | `f0_median_hz`, `f0_mean_hz`, `f0_range_st` |
| spectral_centroid | `spectral_centroid_hz` |
| LTASS vector | `ltass_third_oct_db` |
| quality score | `quality_score` |
| source filename | `source_path` |
| segment start/end | `anchor_sample`, `max_end_sample` |

The fields that exceed the brief (ASL, SNR, bandwidth, pause statistics, fingerprint, solo risk) are needed by the scheduler, CVR and quality control.

---

## 6. Runtime corpus index (Corpus Manager)

At plan build, the Corpus Manager loads a compact, read-only, immutable snapshot:

```cpp
struct SegmentRec  { uint32_t recording; uint32_t anchor48k_lo; uint16_t anchor48k_hi;
                     uint16_t maxLenDs; /* deci-seconds */ int16_t asl_cdB; /* centi-dB */
                     uint8_t speechFrac_pct; uint8_t flags; };                 // 16 bytes
struct RecordingRec{ uint32_t speaker; uint32_t cacheFileIdx; uint32_t pauseListOffset;
                     uint32_t regionListOffset; int16_t asl_cdB; uint16_t quality; };  // 20 bytes
struct SpeakerRec  { float features[8]; uint32_t firstRecording, nRecordings;
                     uint32_t firstAnchor, nAnchors; float weight; uint32_t flags; };  // 56 bytes
```

- **Memory:**
  - segments: 16 B × (≈ 12 anchors per speech minute)
  - 50 speakers × 15 min → 9 000 anchors → 144 KB
  - 10 000 speakers × 12 min → 1.44 M anchors → 23 MB
  - VAD region/pause lists: ~8 B per region → ≈ 2–3× the anchor table
  - Worst supported corpus: < 100 MB. Typical: < 2 MB.
- The snapshot is shared read-only by the planner and preloader (`std::shared_ptr<const CorpusSnapshot>`). It is replaced atomically on commit.
- **Health tracking:** per-recording failure counters (`RELIABILITY.md` §2). A recording is marked unhealthy after 2 decode failures and is excluded until the next re-scan.

---

## 7. Build options and SQLite (revised during implementation)

**SQLite:** the corpus database backend. Fetched from sqlite.org as a single-file amalgamation; option `BF_SQLITE_USE_SYSTEM=ON` uses the system library instead. Compilation with `BF_WITH_CORPUS_DB` toggle.

---

## 8. Licensing and distribution of corpora

- BabbleForge ships **no mandatory corpus**.
- Recommended development/test corpus: MUSAN speech (CC BY 4.0), with attribution kept in `manifest.json`.
- Common Voice (CC0) may be used through the corpus builder by the user. Its terms ask that datasets not be re-mirrored, so it is not bundled.
- Every corpus manifest stores the license and source per recording group (`license`, `attribution`, `sourceUrl`). An export of corpus metadata includes them.

---
