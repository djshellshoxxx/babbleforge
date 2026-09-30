#pragma once
// Spectrum measurement (docs/SPECTRUM_ENGINE.md section 5).
//
// FFT band-power aggregation: Hann window, 8192 points at 48 kHz (scaled with fs to the next
// power of two), 50 % overlap; power is summed inside IEC 61260 base-10 1/3-octave band edges
// (fc * 10^(+-1/20)) with fractional-bin weighting at the edges.
//
// Band levels are band POWER (mean square, so a full-scale sine of amplitude A gives A^2/2 in
// its band and the sum over all bands equals the signal variance). Multi-channel input is
// power-summed (per-channel band powers are added). Time scales: 1 s exponential (display),
// 5 s analysis blocks, 60 s exponential long-term estimate (updated once per 5 s block).
// Not real-time (allocates); single-threaded.
#include <array>
#include <cstddef>
#include <cstdint>
#include <complex>
#include <memory>
#include <vector>

#include "core/dsp/Fft.h"
#include "core/spectrum/SpectrumTarget.h"

namespace bf {

using OperatingBands = std::array<double, kNumOperatingBands>;  // 100 Hz ... 10 kHz

struct SpectrumBlock {
    ThirdOctArray powerLin{};  // mean-square power per 1/3-octave band, 5 s average
    double endSeconds = 0.0;   // stream time at the end of the block
};

// Slice the 21 operating bands (100 Hz..10 kHz) out of a 26-band array.
OperatingBands operatingSlice(const ThirdOctArray& a) noexcept;
// 10*log10(max(p, 1e-30)).
double powerToDb(double p) noexcept;
ThirdOctArray powerToDb(const ThirdOctArray& p) noexcept;
// Octave levels (125 Hz..8 kHz) from linear band powers (power-sum of the 3 constituents).
OctaveArray octaveDbFromPower(const ThirdOctArray& powerLin) noexcept;

class SpectrumAnalyzer {
public:
    static constexpr double kBlockSeconds = 5.0;
    static constexpr double kLongTauSeconds = 60.0;
    static constexpr double kShortTauSeconds = 1.0;

    // fs > 0, numChannels >= 1. Resets all state.
    void prepare(double fs, std::size_t numChannels = 1);
    void reset();

    // Planar input: in[c][0..nFrames), c < numChannels.
    void process(const float* const* in, std::size_t nFrames);
    void process(const double* const* in, std::size_t nFrames);

    // Completed 5 s blocks since the last call (oldest first).
    std::vector<SpectrumBlock> takeBlocks();

    std::size_t fftSize() const noexcept { return n_; }
    double binWidthHz() const noexcept { return fs_ / static_cast<double>(n_); }
    double timeSeconds() const noexcept { return static_cast<double>(samples_) / fs_; }
    std::size_t numHops() const noexcept { return hopCount_; }

    bool hasShortTerm() const noexcept { return hopCount_ > 0; }
    bool hasLongTerm() const noexcept { return haveLong_; }
    const ThirdOctArray& shortTermPower() const noexcept { return shortP_; }  // 1 s exponential
    const ThirdOctArray& longTermPower() const noexcept { return longP_; }    // 60 s exponential
    ThirdOctArray overallPower() const noexcept;                              // mean of all hops

private:
    template <typename T>
    void processImpl(const T* const* in, std::size_t nFrames);
    void analyseHop();

    double fs_ = 48000.0;
    std::size_t nCh_ = 1, n_ = 8192, hop_ = 4096, fill_ = 0;
    std::vector<std::vector<double>> buf_;
    std::vector<double> window_, work_, binPower_;
    std::vector<std::complex<double>> spec_;
    std::unique_ptr<FftD> fft_;
    struct Weight { std::uint32_t bin; double w; };
    std::array<std::vector<Weight>, kNumThirdOctBands> weights_;
    double norm_ = 1.0;

    std::uint64_t samples_ = 0;  // frames consumed
    std::size_t hopCount_ = 0, hopsPerBlock_ = 1, blockHops_ = 0;
    ThirdOctArray shortP_{}, longP_{}, blockAcc_{}, overallAcc_{};
    bool haveLong_ = false;
    double shortAlpha_ = 1.0, longAlpha_ = 1.0;
    std::vector<SpectrumBlock> blocks_;
};

// ---- shape comparison and metrics (section 5.1) ------------------------------------------
struct ShapeMetrics {
    OperatingBands d{};            // per band, dB (mean offset removed), 100 Hz..10 kHz
    double mu = 0.0;               // power-weighted mean offset removed (dB)
    double rmsDev125to8k = 0.0;    // RMS of d over 125 Hz..8 kHz (dB)
    double maxAbsDev = 0.0;        // max |d| over the 21 operating bands (dB)
    std::size_t maxAbsBand = 0;    // index into the 26-band array of that band
    double slopeDbPerOct = 0.0;    // LSQ slope of measured band levels, 250 Hz..4 kHz
    double lfFraction = 0.0;       // energy 100..200 Hz bands / total (all 26 bands)
    double hfFraction = 0.0;       // energy 4..10 kHz bands / total
    double speechFraction = 0.0;   // energy 200 Hz..5 kHz bands / total
};

// d_b = L_meas,b - L_tgt,b - mu with mu the target-power-weighted mean of the offset over the
// 21 operating bands (a pure level difference gives d = 0).
void shapeDeviation(const OperatingBands& measuredDb, const OperatingBands& targetDb,
                    OperatingBands& d, double* mu = nullptr) noexcept;
ShapeMetrics computeShapeMetrics(const ThirdOctArray& measuredPowerLin, const ThirdOctArray& targetDb) noexcept;
// Least-squares slope (dB per octave) of levels over bands [firstBand, lastBand] (26-band indices).
double leastSquaresSlopeDbPerOct(const ThirdOctArray& levelsDb, std::size_t firstBand = 7,
                                 std::size_t lastBand = 19) noexcept;

// ---- offline convenience -----------------------------------------------------------------
struct SpectrumAnalysis {
    ThirdOctArray overallPowerLin{};  // mean over the whole input
    ThirdOctArray overallDb{};
    OctaveArray octaveDb{};
    std::vector<SpectrumBlock> blocks;
    bool hasLongTerm = false;
    ThirdOctArray longTermDb{};
    bool hasShape = false;
    ShapeMetrics shape{};             // overall levels vs the target when one was given
    double durationSeconds = 0.0;
};
// Planar buffers. target may be null (then no shape metrics).
SpectrumAnalysis analyzeSpectrum(const float* const* planar, std::size_t numChannels, std::size_t nFrames,
                                 double fs, const ThirdOctArray* targetDb = nullptr);

}  // namespace bf
