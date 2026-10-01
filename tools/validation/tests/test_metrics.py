import numpy as np
import pytest

from bfval import events, metrics as M, wavio

FS = 48000.0


def pink(n, seed=0, fs=FS, lo=20.0):
    rng = np.random.default_rng(seed)
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / fs)
    spec[1:] /= np.sqrt(np.maximum(f[1:], lo))
    spec[0] = 0
    x = np.fft.irfft(spec, n)
    return x / np.sqrt(np.mean(x * x))


def test_rms_sine_and_scaling():
    t = np.arange(int(FS)) / FS
    x = 0.5 * np.sin(2 * np.pi * 1000 * t)
    assert M.rms_db(x) == pytest.approx(20 * np.log10(0.5 / np.sqrt(2)), abs=1e-3)
    assert M.rms_db(0.1 * x) - M.rms_db(x) == pytest.approx(-20.0, abs=1e-9)
    assert M.sample_peak_db(x) == pytest.approx(20 * np.log10(0.5), abs=1e-3)


def test_third_octave_sine_in_correct_band():
    t = np.arange(int(2 * FS)) / FS
    x = np.sin(2 * np.pi * 1000 * t)
    lv = M.third_octave_levels(x, FS)
    i = M.THIRD_OCT_NOMINAL.index(1000)
    assert lv[i] == pytest.approx(-3.01, abs=0.1)
    assert lv[i - 2] < lv[i] - 25 and lv[i + 2] < lv[i] - 25
    assert lv[i - 1] < lv[i] - 8 or lv[i - 1] < -10  # 1 kHz is a band centre, neighbours attenuated


def test_pink_is_flat_per_band_and_octave_consistent():
    x = pink(int(20 * FS))
    lv = M.third_octave_levels(x, FS)
    op = lv[M.OPERATING]
    assert np.max(np.abs(op - op.mean())) < 1.0  # pink = equal power per band
    # sum over all bands approximates the total power (0 dB) within filter overlap
    assert 10 * np.log10(np.sum(10 ** (lv[~np.isnan(lv)] / 10))) == pytest.approx(0.0, abs=1.5)  # band-limited to 44 Hz-17.9 kHz
    oct_sum = np.array(M.octave_from_third(lv))
    oct_fb = np.array(M.octave_levels_filterbank(x, FS))
    assert np.max(np.abs(oct_sum - oct_fb)) < 0.8
    assert np.all(np.abs(oct_sum - oct_sum.mean()) < 1.0)
    assert oct_sum.mean() - op.mean() == pytest.approx(10 * np.log10(3), abs=0.3)


def test_ltass_deviation_zero_for_matching_target_and_offset_invariant():
    x = pink(int(10 * FS), seed=3)
    lv = M.third_octave_levels(x, FS)
    d = M.ltass_deviation(lv, np.asarray(lv))
    assert d["rms125to8kDb"] == pytest.approx(0, abs=1e-9)
    d2 = M.ltass_deviation(lv + 7.0, np.asarray(lv))  # pure level change is not a shape error
    assert d2["rms125to8kDb"] == pytest.approx(0, abs=1e-9)
    tgt = np.asarray(lv).copy()
    tgt[10] += 3.0
    assert M.ltass_deviation(lv, tgt)["maxAbsDb"] > 2.0
    assert d["maxAbsDb"] < 1e-9


def test_pink_slope_is_minus_3db_per_octave_density_flat_band_power():
    x = pink(int(10 * FS), seed=4)
    assert abs(M.slope_db_per_oct(M.third_octave_levels(x, FS))) < 0.3  # band power: flat for pink


def test_envelope_and_l10_l90():
    n = int(10 * FS)
    rng = np.random.default_rng(1)
    x = rng.standard_normal(n)
    p = M.envelope_power_10ms(x, FS)
    assert len(p) == 1000 and np.mean(p) == pytest.approx(1.0, rel=0.05)
    # stationary Gaussian noise: small L10-L90 (10 ms frames of 480 samples)
    assert M.l10_l90(p)[2] < 1.5
    # 50 % duty square-gated noise: huge L10-L90
    gate = (np.arange(n) // int(0.5 * FS)) % 2
    pg = M.envelope_power_10ms(x * (0.001 + gate), FS)
    assert M.l10_l90(pg)[2] > 40


def test_gap_detection_known_gap():
    rate = 100.0
    p = np.ones(3000)
    p[1000:1030] = 1e-4  # 300 ms, -40 dB
    p[2000:2005] = 1e-4  # 50 ms
    g = M.gap_durations(p, rate)
    assert list(np.round(g, 3)) == [0.3, 0.05]
    st = M.gap_stats(p, rate)
    assert st["count"] == 2 and st["maxS"] == pytest.approx(0.3) and st["medianS"] == pytest.approx(0.175)
    assert st["histogram"][3] == 1 and st["histogram"][1] == 1  # 0.3 s in [0.2, 0.5)
    # 11 dB dip is not a gap (threshold Leq - 12 dB)
    q = np.ones(3000)
    q[1000:1030] = 10 ** (-1.1)
    assert len(M.gap_durations(q, rate)) == 0
    # trailing open gap is not counted
    r = np.ones(1000)
    r[-20:] = 1e-5
    assert len(M.gap_durations(r, rate)) == 0


def test_modulation_spectrum_am_noise_peak_and_index():
    n = int(60 * FS)
    rng = np.random.default_rng(2)
    m_idx, fm = 0.5, 4.0
    t = np.arange(n) / FS
    # power envelope (1 + m sin)^2-ish; use amplitude env so that the POWER envelope mod index ~ 2m for small m:
    env_amp = np.sqrt(1.0 + m_idx * np.sin(2 * np.pi * fm * t))  # power envelope = 1 + m sin
    x = env_amp * rng.standard_normal(n)
    p = M.envelope_power_10ms(x, FS)
    mod = M.modulation_spectrum(p)
    k = int(np.argmax(mod))
    assert M.MOD_BAND_NOMINAL[k] in (3.15, 4.0, 5.0)
    assert M.modulation_at(mod, 4.0) == pytest.approx(m_idx, rel=0.25)
    flat = M.modulation_spectrum(M.envelope_power_10ms(rng.standard_normal(n), FS))
    assert np.max(flat) < 0.05  # statistical floor for stationary noise (SPECTRUM_ENGINE 8.3)
    assert mod[9] > 5 * np.median(flat)


def test_modulation_spectrum_direct_sinusoid_index_is_exact():
    rate, m_idx = 100.0, 0.3
    t = np.arange(int(60 * rate)) / rate
    p = 2.0 * (1 + m_idx * np.sin(2 * np.pi * 2.5 * t))
    mod = M.modulation_spectrum(p)
    assert M.modulation_at(mod, 2.5) == pytest.approx(m_idx, rel=0.05)
    assert M.modulation_at(mod, 10.0) < 0.02


def test_short_signal_gives_nan_modulation():
    assert np.all(np.isnan(M.modulation_spectrum(np.ones(500))))


def test_channel_correlation():
    n = int(12 * FS)
    rng = np.random.default_rng(5)
    a = rng.standard_normal(n)
    x = np.vstack([a, a, -a, rng.standard_normal(n)])
    c = M.channel_correlation(x, fs=FS)
    mat = np.array(c["matrix"])
    assert mat[0, 1] == pytest.approx(1.0, abs=1e-9) and mat[0, 2] == pytest.approx(-1.0, abs=1e-9)
    assert abs(mat[0, 3]) < 0.02
    assert np.array(c["max10sAbs"])[0, 2] == pytest.approx(1.0, abs=1e-9)
    assert M.channel_correlation(a, fs=FS) is None


def test_analyze_structure_and_wav_roundtrip(tmp_path):
    x = pink(int(5 * FS), seed=7)
    x = np.vstack([x, pink(int(5 * FS), seed=8)]) * 0.1
    p = tmp_path / "t.wav"
    wavio.write_wav_f32(p, x, FS)
    y, fs = wavio.read_wav(p)
    assert fs == FS and y.shape == x.shape and np.max(np.abs(y - x)) < 1e-7
    a = M.analyze(y, fs)
    assert a["channels"] == 2 and len(a["spectrum"]["thirdOctDb"]) == 26
    assert len(a["temporal"]["modulationSpectrum"]) == 16
    assert abs(a["correlation"]["matrix"][0][1]) < 0.1


def test_target_rolloff_attenuates_lf_hf_only():
    t = np.zeros(26)
    r = np.array(M.apply_target_rolloff(t, 80.0, 12500.0, FS))
    assert abs(r[M.THIRD_OCT_NOMINAL.index(1000)]) < 0.01
    assert r[0] < -8 and r[-1] < -5


def test_event_stats_synthetic():
    ev = {"sampleRate": 100.0, "events": [
        {"start": -50, "end": 200, "speaker": 1, "recording": 1, "segment": 1},
        {"start": 100, "end": 400, "speaker": 2, "recording": 2, "segment": 1},
        {"start": 300, "end": 1200, "speaker": 1, "recording": 1, "segment": 1},
        {"start": 700, "end": 900, "speaker": 3, "recording": 3, "segment": 5}]}
    t = events.talker_stats(ev, 1000, 100.0)
    # k_a: [0,100)=1, [100,200)=2, [200,300)=1, [300,400)=2, [400,700)=1, [700,900)=2, [900,1000)=1
    assert t["meanActive"] == pytest.approx((200 + 100 + 300 + 200 + 200 + 100 + 100 + 100 - 100 + 50 + 0) / 1000 * 1.0, abs=2.0)
    assert t["minActive"] == 1 and t["maxActive"] == 2
    assert t["events"] == 4 and t["speakers"] == 3
    assert t["regionReuseCount"] == 1 and t["speakerReuseGapMin"] == 2
    assert t["startSpacingMinS"] == pytest.approx(2.0)
