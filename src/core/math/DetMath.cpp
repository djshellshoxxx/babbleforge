#include "core/math/DetMath.h"

#include <cstdint>
#include <cstring>
#include <limits>

// NOTE: algorithms follow FreeBSD msun / fdlibm (Sun Microsystems, permissive licence).
namespace bf {
namespace {

inline std::uint64_t bitsOf(double d) noexcept { std::uint64_t u; std::memcpy(&u, &d, 8); return u; }
inline double fromBits(std::uint64_t u) noexcept { double d; std::memcpy(&d, &u, 8); return d; }

constexpr double kLn2Hi = 6.93147180369123816490e-01;
constexpr double kLn2Lo = 1.90821492927058770002e-10;

// ---- sin/cos kernels on [-pi/4, pi/4] ----
constexpr double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                 S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                 S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
constexpr double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                 C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                 C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;

double kSin(double x, double y) noexcept {
    const double z = x * x;
    const double v = z * x;
    const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

double kCos(double x, double y) noexcept {
    const double ax = x < 0 ? -x : x;
    const double z = x * x;
    const double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ax < 0.3) return 1.0 - (0.5 * z - (z * r - x * y));
    double qx;
    if (ax > 0.78125) qx = 0.28125;
    else qx = fromBits((bitsOf(ax) - 0x0020000000000000ULL) & 0xFFFFFFFF00000000ULL);
    const double hz = 0.5 * z - qx;
    const double a = 1.0 - qx;
    return a - (hz - (z * r - x * y));
}

// Range reduction for 0 <= x < ~1.6e6: x = n*pi/2 + (y0+y1). Returns n.
int remPio2(double x, double& y0, double& y1) noexcept {
    constexpr double invpio2 = 6.36619772367581382433e-01;
    constexpr double pio2_1 = 1.57079632673412561417e+00, pio2_1t = 6.07710050650619224932e-11;
    constexpr double pio2_2 = 6.07710050630396597660e-11, pio2_2t = 2.02226624879595063154e-21;
    constexpr double pio2_3 = 2.02226624871116645580e-21, pio2_3t = 8.47842766036889956997e-32;
    const int n = static_cast<int>(x * invpio2 + 0.5);
    const double fn = static_cast<double>(n);
    double r = x - fn * pio2_1;
    double w = fn * pio2_1t;
    const int j = static_cast<int>((bitsOf(x) >> 52) & 0x7FF);
    y0 = r - w;
    int i = j - static_cast<int>((bitsOf(y0) >> 52) & 0x7FF);
    if (i > 16) {
        double t = r;
        w = fn * pio2_2;
        r = t - w;
        w = fn * pio2_2t - ((t - r) - w);
        y0 = r - w;
        i = j - static_cast<int>((bitsOf(y0) >> 52) & 0x7FF);
        if (i > 49) {
            t = r;
            w = fn * pio2_3;
            r = t - w;
            w = fn * pio2_3t - ((t - r) - w);
            y0 = r - w;
        }
    }
    y1 = (r - y0) - w;
    return n;
}

}  // namespace

double detlog(double x) noexcept {
    constexpr double Lg1 = 6.666666666666735130e-01, Lg2 = 3.999999999940941908e-01,
                     Lg3 = 2.857142874366239149e-01, Lg4 = 2.222219843214978396e-01,
                     Lg5 = 1.818357216161805012e-01, Lg6 = 1.531383769920937332e-01,
                     Lg7 = 1.479819860511658591e-01;
    if (x != x) return x;
    if (x == 0.0) return -std::numeric_limits<double>::infinity();
    if (x < 0.0) return std::numeric_limits<double>::quiet_NaN();
    if (x > std::numeric_limits<double>::max()) return x;
    int k = 0;
    if (x < 0x1.0p-1022) { k -= 54; x *= 0x1.0p54; }
    std::uint64_t b = bitsOf(x);
    std::uint32_t hx = static_cast<std::uint32_t>(b >> 32);
    k += static_cast<int>(hx >> 20) - 1023;
    hx &= 0x000FFFFF;
    const std::uint32_t i0 = (hx + 0x95F64) & 0x100000;
    b = (static_cast<std::uint64_t>(hx | (i0 ^ 0x3FF00000)) << 32) | (b & 0xFFFFFFFFULL);
    x = fromBits(b);
    k += static_cast<int>(i0 >> 20);
    const double f = x - 1.0;
    const double dk = static_cast<double>(k);
    const double s = f / (2.0 + f);
    const double z = s * s;
    const std::int32_t i = static_cast<std::int32_t>(hx) - 0x6147A;
    const double w = z * z;
    const std::int32_t j = 0x6B851 - static_cast<std::int32_t>(hx);
    const double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    const double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    const double R = t2 + t1;
    if ((i | j) > 0) {
        const double hfsq = 0.5 * f * f;
        return dk * kLn2Hi - ((hfsq - (s * (hfsq + R) + dk * kLn2Lo)) - f);
    }
    return dk * kLn2Hi - ((s * (f - R) - dk * kLn2Lo) - f);
}

double detexp(double x) noexcept {
    constexpr double P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03,
                     P3 = 6.61375632143793436117e-05, P4 = -1.65339022054652515390e-06,
                     P5 = 4.13813679705723846039e-08;
    constexpr double invln2 = 1.44269504088896338700e+00;
    if (x != x) return x;
    if (x > 709.0) return std::numeric_limits<double>::infinity();
    if (x < -708.0) return 0.0;
    const int k = static_cast<int>(x * invln2 + (x >= 0 ? 0.5 : -0.5));
    const double fk = static_cast<double>(k);
    const double hi = x - fk * kLn2Hi;
    const double lo = fk * kLn2Lo;
    const double r = hi - lo;
    const double t = r * r;
    const double c = r - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    const double y = 1.0 - ((lo - (r * c) / (2.0 - c)) - hi);
    return y * fromBits(static_cast<std::uint64_t>(k + 1023) << 52);
}

void detsincos(double x, double& s, double& c) noexcept {
    if (x != x || x - x != 0.0) { s = c = std::numeric_limits<double>::quiet_NaN(); return; }
    const bool neg = x < 0;
    const double ax = neg ? -x : x;
    double y0 = ax, y1 = 0.0;
    int n = 0;
    if (ax > 0.785398163397448309616) n = remPio2(ax, y0, y1);
    double ss, cc;
    if (ax < 0x1.0p-27) { ss = ax; cc = 1.0; }
    else { ss = kSin(y0, y1); cc = kCos(y0, y1); }
    switch (n & 3) {
        case 0: break;
        case 1: { const double t = ss; ss = cc; cc = -t; break; }
        case 2: ss = -ss; cc = -cc; break;
        default: { const double t = ss; ss = -cc; cc = t; break; }
    }
    s = neg ? -ss : ss;
    c = cc;
}

double detsin(double x) noexcept { double s, c; detsincos(x, s, c); return s; }
double detcos(double x) noexcept { double s, c; detsincos(x, s, c); return c; }

}  // namespace bf
