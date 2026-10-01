"""Signal metrics, defined as in docs/SPECTRUM_ENGINE.md 5 and 8 and docs/VALIDATION.md 2.2/5.

All functions take audio as an array shaped (channels, frames) (1-D input is treated as one channel).
The structure of analyze() mirrors the JSON written by bfanalyze so compare.py can diff them.

Conventions shared with the C++ analyzer (so numbers are comparable):
  * level = mean square power in dB (a full-scale sine is -3.01 dB);
  * overall RMS is the energy mean over all channels;
  * band levels are band POWER, per-channel powers are ADDED across channels;
  * the 10 ms envelope is the frame mean square averaged over channels;
  * percentiles use linear interpolation (numpy default).
"""
import numpy as np
from scipy import signal

THIRD_OCT_NOMINAL = [50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
                     2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000]
OCTAVE_NOMINAL = [125, 250, 500, 1000, 2000, 4000, 8000]
MOD_BAND_NOMINAL = [0.5, 0.63, 0.8, 1.0, 1.25, 1.6, 2.0, 2.5, 3.15, 4.0, 5.0, 6.3, 8.0, 10.0, 12.5, 16.0]
OPERATING = slice(3, 24)  # 100 Hz .. 10 kHz in THIRD_OCT_NOMINAL
FRAME_RATE = 100.0
GAP_BELOW_LEQ_DB = 12.0


def as2d(x):
    x = np.asarray(x, dtype=np.float64)
    return x[None, :] if x.ndim == 1 else x


def pdb(p, floor=1e-30):
    return 10.0 * np.log10(np.maximum(p, floor))


# ------------------------------------------------------------------------------------------ level
def rms_db(x):
    x = as2d(x)
    return float(pdb(np.mean(x * x)))


def rms_db_per_channel(x):
    x = as2d(x)
    return [float(pdb(np.mean(c * c))) for c in x]


def sample_peak_db(x):
    return float(pdb(np.max(np.abs(as2d(x))) ** 2))


def true_peak_db(x, fs, oversample=4):
    """Approximate true peak (4x polyphase oversampling); BS.1770 style, not bit-exact."""
    x = as2d(x)
    if fs >= 96000:
        oversample = 2
    pk = 0.0
    for c in x:
        pk = max(pk, float(np.max(np.abs(signal.resample_poly(c, oversample, 1)))))
    return float(pdb(pk * pk))


# ----------------------------------------------------------------------------- IEC 61260 filterbank
def third_oct_centres_exact():
    """Base-10 exact centres 1000*10^(i/10), i = -13..12 (the 26 nominal bands 50 Hz..16 kHz)."""
    return 1000.0 * 10.0 ** (np.arange(-13, 13) / 10.0)


def band_sos(fc, fs, fraction=3, order=3):
    """Butterworth band-pass SOS for one base-10 band: edges fc*10^(+-3/(20*fraction)). order=3 gives a
    6th-order band-pass (class-1 style; rolls off at least 18 dB/oct outside the band)."""
    g = 10.0 ** (3.0 / (10.0 * fraction * 2.0))  # 1/3-oct: 10^(1/20); octave: 10^(3/20)
    lo, hi = fc / g, fc * g
    if hi >= 0.5 * fs * 0.98:
        return None
    return signal.butter(order, [lo, hi], btype="bandpass", fs=fs, output="sos")


def third_octave_levels(x, fs, order=3):
    """Per-band mean-square level in dB (26 bands, NaN above Nyquist). Channel powers added."""
    x = as2d(x)
    out = np.full(len(THIRD_OCT_NOMINAL), np.nan)
    for i, fc in enumerate(third_oct_centres_exact()):
        sos = band_sos(fc, fs, 3, order)
        if sos is None:
            continue
        p = 0.0
        for c in x:
            y = signal.sosfilt(sos, c)
            p += float(np.mean(y * y))
        out[i] = pdb(p)
    return out


def octave_from_third(third_db):
    """Octave 125..8k by power-summing the three constituent 1/3-octave bands (SPECTRUM_ENGINE 2.1)."""
    out = []
    for j in range(7):
        i0 = 3 + 3 * j  # 125 Hz octave = bands 100,125,160 (indices 3,4,5)
        p = np.sum(10.0 ** (np.asarray(third_db[i0:i0 + 3]) / 10.0))
        out.append(float(pdb(p)))
    return out


def octave_levels_filterbank(x, fs, order=3):
    """Octave levels with a true octave filterbank (independent of the 1/3-oct power sum)."""
    x = as2d(x)
    out = []
    for j in range(-3, 4):
        fc = 1000.0 * 10.0 ** (0.3 * j)
        sos = band_sos(fc, fs, 1, order)
        if sos is None:
            out.append(float("nan"))
            continue
        p = sum(float(np.mean(signal.sosfilt(sos, c) ** 2)) for c in x)
        out.append(float(pdb(p)))
    return out


# ------------------------------------------------------------------------------------------- LTASS
def ltass_deviation(third_db, target_db):
    """Shape deviation per SPECTRUM_ENGINE 5.1 over the 21 operating bands (100 Hz-10 kHz):
    d_b = L_b - T_b - mu, mu = target-power-weighted mean offset. target_db: 26 values (or the 21 slice).
    Returns dict(d=[21], rms125to8k, maxAbs, maxAbsBand)."""
    L = np.asarray(third_db, dtype=float)[OPERATING]
    T = np.asarray(target_db, dtype=float)
    T = T[OPERATING] if len(T) == 26 else T
    w = 10.0 ** (T / 10.0)
    mu = float(np.sum(w * (L - T)) / np.sum(w))
    d = L - T - mu
    sub = d[1:-1]  # 125 Hz .. 8 kHz
    return {"d": d.tolist(), "rms125to8kDb": float(np.sqrt(np.mean(sub ** 2))),
            "maxAbsDb": float(np.max(np.abs(d))), "maxAbsBandHz": THIRD_OCT_NOMINAL[3 + int(np.argmax(np.abs(d)))],
            "offsetDb": mu}


def apply_target_rolloff(target_db, lf_hz=80.0, hf_hz=12500.0, fs=48000.0):
    """Add the 2nd-order Butterworth LF/HF limit roll-off (SPECTRUM_ENGINE 2.1, band-centre approximation) to a
    26-band shape target so it can be compared with a rendered stationary masker."""
    f = np.array(THIRD_OCT_NOMINAL, dtype=float)
    hf = min(hf_hz, 0.45 * fs)
    h2 = 1.0 / (1.0 + (lf_hz / f) ** 4) / (1.0 + (f / hf) ** 4)
    return (np.asarray(target_db, dtype=float) + 10.0 * np.log10(h2)).tolist()


def slope_db_per_oct(third_db, lo=250, hi=4000):
    f = np.array(THIRD_OCT_NOMINAL, dtype=float)
    m = (f >= lo) & (f <= hi)
    return float(np.polyfit(np.log2(f[m]), np.asarray(third_db)[m], 1)[0])


# --------------------------------------------------------------------------------------- envelope
def envelope_power_10ms(x, fs):
    """10 ms frame mean-square power, averaged over channels. Shape (frames,)."""
    x = as2d(x)
    fl = int(round(fs * 0.01))
    n = x.shape[1] // fl
    p = np.mean(x[:, : n * fl].reshape(x.shape[0], n, fl) ** 2, axis=(0, 2))
    return p


def level_db_frames(p):
    return 10.0 * np.log10(np.maximum(p, 1e-20))


def l10_l90(p):
    """L10 - L90 of the 10 ms frame levels (dB). Returns (L10, L90, L10-L90)."""
    lv = level_db_frames(p)
    l10, l90 = np.percentile(lv, 90), np.percentile(lv, 10)
    return float(l10), float(l90), float(l10 - l90)


def gap_durations(p, rate=FRAME_RATE, leq_window_s=10.0):
    """Gaps = runs of 10 ms frames whose level is < Leq(last 10 s, incl. the frame) - 12 dB.
    A gap is counted when the run ends (a trailing open run is ignored, as in bfanalyze). Seconds."""
    p = np.maximum(np.asarray(p, dtype=float), 0.0)
    w = int(round(leq_window_s * rate))
    cs = np.concatenate([[0.0], np.cumsum(p)])
    idx = np.arange(1, len(p) + 1)
    lo = np.maximum(idx - w, 0)
    leq = level_db_frames((cs[idx] - cs[lo]) / (idx - lo))
    below = level_db_frames(p) < leq - GAP_BELOW_LEQ_DB
    gaps, run = [], 0
    for b in below:
        if b:
            run += 1
        elif run:
            gaps.append(run / rate)
            run = 0
    return np.array(gaps)


def gap_stats(p, rate=FRAME_RATE):
    g = gap_durations(p, rate)
    dur = len(p) / rate
    edges = [0.0, 0.05, 0.1, 0.2, 0.5, 1.0, np.inf]
    hist = [int(np.sum((g >= edges[i]) & (g < edges[i + 1]))) for i in range(6)]
    return {"count": int(len(g)), "medianS": float(np.percentile(g, 50)) if len(g) else 0.0,
            "p95S": float(np.percentile(g, 95)) if len(g) else 0.0, "maxS": float(g.max()) if len(g) else 0.0,
            "ratePerS": len(g) / dur if dur > 0 else 0.0, "histogram": hist}


def modulation_depth_broadband(p, rate=FRAME_RATE, window_s=10.0):
    """m = std/mean of the 10 ms power envelope low-passed at 16 Hz, last 10 s (SPECTRUM_ENGINE 8.2)."""
    sos = signal.butter(2, min(16.0, 0.45 * rate), btype="low", fs=rate, output="sos")
    y = signal.sosfilt(sos, p, zi=signal.sosfilt_zi(sos) * p[0])[0]
    y = y[-int(window_s * rate):]
    return float(np.std(y) / np.mean(y)) if np.mean(y) > 0 else 0.0


# ------------------------------------------------------------------------------ modulation spectrum
def modulation_spectrum(p, rate=FRAME_RATE, block_s=20.0, avg_s=60.0, report_s=60.0):
    """Modulation spectrum m(f) in 16 one-third-octave bands 0.5-16 Hz (SPECTRUM_ENGINE 8.3).

    20 s blocks (2000 frames), periodic Hann, 50 % overlap, zero-padded FFT, m = 2|E|/E(0); band value =
    sqrt(sum m^2 over the band bins / ENBW) so a sinusoidal modulation of index m reads m. Block values are
    averaged over the blocks that ended in the last 60 s; for a whole file, that 60 s average is taken at every
    60 s boundary and those snapshots are averaged (bfanalyze convention). Files < 60 s use the final average.
    Returns an array of 16 values (NaN if the file is shorter than one block)."""
    p = np.asarray(p, dtype=float)
    blen = int(round(block_s * rate))
    hop = blen // 2
    nfft = 1
    while nfft < blen:
        nfft *= 2
    nfft = max(nfft, 4)
    win = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(blen) / blen)
    enbw = 1.5 * nfft / blen
    df = rate / nfft
    centres = 10.0 ** ((np.arange(16) - 3.0) / 10.0)
    ranges = [(int(np.ceil(c * 10 ** -0.05 / df)), int(np.ceil(c * 10 ** 0.05 / df))) for c in centres]
    ends, vals = [], []
    for end in range(blen, len(p) + 1, hop):
        seg = np.zeros(nfft)
        seg[:blen] = win * p[end - blen:end]
        spec = np.fft.rfft(seg)
        e0 = spec[0].real
        m = np.zeros(16)
        if e0 > 0:
            mm = 2.0 * np.abs(spec) / e0
            for b, (a, z) in enumerate(ranges):
                m[b] = np.sqrt(np.sum(mm[a:z] ** 2) / enbw)
        ends.append(end)
        vals.append(m)
    if not ends:
        return np.full(16, np.nan)
    ends, vals = np.array(ends), np.array(vals)
    horizon = int(round(avg_s * rate))

    def avg_at(total):
        sel = (total - ends >= 0) & (total - ends < horizon)
        return vals[sel].mean(axis=0) if sel.any() else None

    step = int(round(report_s * rate))
    snaps = [avg_at(t) for t in range(step, len(p) + 1, step)]
    snaps = [s for s in snaps if s is not None]
    if snaps:
        return np.mean(snaps, axis=0)
    return avg_at(len(p))


def modulation_at(mod, hz):
    """m(f) of the band whose centre is nearest `hz`."""
    return float(mod[int(np.argmin(np.abs(np.array(MOD_BAND_NOMINAL) - hz)))])


# ------------------------------------------------------------------------------------ correlation
def channel_correlation(x, window_s=10.0, fs=48000.0):
    """Pearson correlation matrix over the file and max |r| over consecutive 10 s windows."""
    x = as2d(x)
    nch, n = x.shape
    if nch < 2:
        return None
    full = np.corrcoef(x)
    w = int(round(window_s * fs))
    mw = np.ones((nch, nch))
    if n >= w:
        mw = np.zeros((nch, nch))
        for off in range(0, n - w + 1, w):
            c = np.corrcoef(x[:, off:off + w])
            mw = np.maximum(mw, np.abs(np.nan_to_num(c)))
        np.fill_diagonal(mw, 1.0)
    adj = max(mw[a, (a + 1) % nch] for a in range(nch))
    return {"matrix": np.nan_to_num(full).tolist(), "max10sAbs": mw.tolist(), "maxAdjacent10sAbs": float(adj)}


# --------------------------------------------------------------------------------------------- all
def analyze(x, fs, target_db=None):
    """Full metric set in a structure mirroring bfanalyze's JSON ("level", "spectrum", "temporal",
    "correlation"). target_db: optional 26-value band-level target for the LTASS deviation."""
    x = as2d(x)
    nch, n = x.shape
    rms = rms_db(x)
    tp = true_peak_db(x, fs)
    third = third_octave_levels(x, fs)
    octv = octave_from_third(third)
    p = envelope_power_10ms(x, fs)
    l10, l90, l1090 = l10_l90(p)
    mod = modulation_spectrum(p)
    out = {
        "sampleRate": fs, "channels": nch, "frames": n, "durationS": n / fs,
        "level": {"rmsDb": rms, "rmsChDb": rms_db_per_channel(x), "samplePeakDb": sample_peak_db(x),
                  "truePeakDbtp": tp, "crestDb": tp - rms},
        "spectrum": {"thirdOctCentresHz": THIRD_OCT_NOMINAL, "thirdOctDb": third.tolist(),
                     "octaveCentresHz": OCTAVE_NOMINAL, "octaveDb": octv,
                     "octaveFilterbankDb": octave_levels_filterbank(x, fs)},
        "temporal": {"envelope": {"L10Db": l10, "L90Db": l90, "L10minusL90Db": l1090,
                                  "modulationDepth10s": modulation_depth_broadband(p)},
                     "gaps": gap_stats(p),
                     "modulationSpectrum": {str(f): float(v) for f, v in zip(MOD_BAND_NOMINAL, mod)}},
    }
    if target_db is not None:
        out["spectrum"]["deviation"] = ltass_deviation(third, target_db)
    corr = channel_correlation(x, fs=fs)
    if corr:
        out["correlation"] = corr
    return out
