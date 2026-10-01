# Corpus management, ingestion, VAD, segment selection

## Ingestion, on-disk corpus (docs/CORPUS.md §2-§5)

- `ingest/`: Corpus Analyzer stages (decode WAV/AIFF/FLAC, r8brain resample, channel normalisation, 20 Hz zero-phase
  DC removal, libfvad VAD + post-processing, segmentation via `CorpusSnapshot::build`, P.56-B ASL, LTASS, YIN, syllable
  rate, clipping, quality score, Haitsma-Kalker fingerprints, speaker identity resolution). `analyzeFile()` is the entry point.
- `CorpusDb`: SQLite schema of §5. `CorpusImporter`: parallel import, exact/near duplicate detection, speaker
  aggregation, LTASS PCA, staging directory + swap into `<corpusRoot>` (manifest last). `CorpusLoader`: DB -> `CorpusSnapshot`
  + `FlacCacheAudioSource`. `FlacCache`: 48 kHz mono 24-bit FLAC writer / seekable reader (LRU of 64 decoders).
- CLI: `bfcorpus import <inputDir> <corpusRoot> [--speakers csv] [--threads N]`, `bfcorpus info <corpusRoot>`.
- Notes: `pause_frac_gtX` is the share of total pause *time* in pauses longer than X ms; the effective bandwidth uses
  1/3-octave band levels (power, not density); per-segment ASL is P.56-B over the 10 s window; quality class values:
  0 rejected, 1 usable, 2 good; reason codes `decode.*`, `reject.*`, `warn.*`, `duplicate.exact|near|overlap`.
