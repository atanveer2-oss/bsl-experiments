#pragma once
// Workload generators. Zipf is the workload the older draft's experiments used
// (s = 1.5 over 500 keys) and the one the entropy benchmark is stated against.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "bsl/rng.hpp"

namespace bsl {

// Zipf over {0,...,n-1} with P[i] proportional to 1/(i+1)^s.
// Precomputes the CDF, so sampling is a binary search: fine for the key counts
// these experiments use, and it keeps the generator out of the measured path.
class ZipfGenerator {
 public:
  ZipfGenerator(std::size_t n, double s, std::uint64_t seed)
      : cdf_(n), rng_(seed) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      acc += 1.0 / std::pow(static_cast<double>(i + 1), s);
      cdf_[i] = acc;
    }
    for (auto& c : cdf_) c /= acc;
  }

  std::size_t sample() {
    const double u = rng_.uniform();
    std::size_t lo = 0, hi = cdf_.size() - 1;
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      if (cdf_[mid] < u) lo = mid + 1; else hi = mid;
    }
    return lo;
  }

  // True probability of rank i, recovered from the CDF.
  double prob(std::size_t i) const {
    return i == 0 ? cdf_[0] : cdf_[i] - cdf_[i - 1];
  }

  std::size_t size() const { return cdf_.size(); }

 private:
  std::vector<double> cdf_;
  Rng rng_;
};

// Entropy benchmark Ent(sigma) = F + sum_x f_x log_b (F / f_x), the quantity
// Theorems B and C are stated against. `b` is the log base, 1/p in the paper.
inline double entropy_benchmark(const std::vector<std::uint64_t>& freqs,
                                double b) {
  double F = 0.0;
  for (auto f : freqs) F += static_cast<double>(f);
  if (F == 0.0) return 0.0;
  const double ln_b = std::log(b);
  double acc = 0.0;
  for (auto f : freqs) {
    if (f == 0) continue;
    const double fx = static_cast<double>(f);
    acc += fx * (std::log(F / fx) / ln_b);
  }
  return F + acc;
}

}  // namespace bsl
