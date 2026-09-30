#include "core/strategy/TalkerCountModes.h"

#include <algorithm>
#include <cmath>

namespace bf {

namespace {
struct Row {
  int n, pool;
  double mean;
  int min, max;
  bool fixedK;
};
constexpr Row kRows[] = {
    {1, 4, 1, 1, 1, true},     {2, 6, 2, 2, 2, true},       {3, 7, 3, 3, 3, true},
    {4, 8, 4, 4, 4, true},     {5, 10, 5, 5, 5, true},      {7, 12, 7, 7, 7, true},
    {8, 18, 8, 6, 10, false},  {12, 26, 12, 9, 15, false},  {16, 34, 16, 12, 20, false},
    {24, 48, 24, 18, 30, false},
};
}  // namespace

TalkerCountMode talkerCountMode(int n, std::optional<int> availableSpeakers) {
  n = std::clamp(n, 1, 64);
  TalkerCountMode m;
  m.n = n;
  m.labContinuousN = n;
  bool found = false;
  for (const Row& r : kRows)
    if (r.n == n) {
      m.fixedK = r.fixedK;
      m.pool = r.pool;
      m.mean = r.mean;
      m.minActive = r.min;
      m.maxActive = r.max;
      m.tabulated = true;
      found = true;
    }
  if (!found) {
    if (n <= 7) {  // only n = 6 is missing
      m.fixedK = true;
      m.mean = n;
      m.minActive = m.maxActive = n;
      m.pool = n + 5;
    } else {
      m.fixedK = false;
      m.mean = std::min(static_cast<double>(n), 32.0);
      m.maxActive = std::min(static_cast<int>(std::lround(1.25 * n)), 48);
      m.minActive = std::min(static_cast<int>(std::lround(0.75 * n)), static_cast<int>(m.mean));
      m.pool = std::min(std::max(2 * n + 2, m.maxActive + 4), 64);
    }
  }
  if (availableSpeakers && *availableSpeakers > 0) m.pool = std::min(m.pool, *availableSpeakers);
  return m;
}

int multiVoicePool(int k) {
  k = std::clamp(k, 1, 16);
  int p;
  if (k <= 7) {
    p = talkerCountMode(k).pool;
  } else {
    const int mx = static_cast<int>(std::lround(1.25 * k));
    p = std::max(2 * k + 2, mx + 4);
  }
  return std::max(p, k + 3);
}

}  // namespace bf
