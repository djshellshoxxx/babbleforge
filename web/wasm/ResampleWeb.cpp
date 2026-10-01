// Web build replacement for bf::ingest::resample (corpus/ingest/Signal.cpp, which needs r8brain
// and libfvad). The web demo always runs the engine at 48 kHz, the corpus rate, so the
// SourcePreparer only ever takes the identity path. Other rates fall back to linear
// interpolation (not used by the demo; documented in web/README.md).
#include <cmath>
#include <cstddef>
#include <vector>

#include "core/corpus/ingest/Signal.h"

namespace bf::ingest {

std::vector<float> resample(const float* x, std::size_t n, double srcRate, double dstRate) {
    if (srcRate == dstRate) return std::vector<float>(x, x + n);
    const auto outLen = static_cast<std::size_t>(std::llround(static_cast<double>(n) * dstRate / srcRate));
    std::vector<float> out(outLen, 0.0f);
    for (std::size_t i = 0; i < outLen && n > 0; ++i) {
        const double t = static_cast<double>(i) * srcRate / dstRate;
        const auto k = static_cast<std::size_t>(t);
        const double f = t - static_cast<double>(k);
        const float a = x[k < n ? k : n - 1], b = x[k + 1 < n ? k + 1 : n - 1];
        out[i] = static_cast<float>(a + (b - a) * f);
    }
    return out;
}

}  // namespace bf::ingest
