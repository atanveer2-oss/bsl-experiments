#pragma once
// Shared RNG plumbing. One splitmix64-seeded PCG-ish generator per structure so
// that runs are reproducible from a single seed.

#include <cstdint>
#include <limits>
#include <cmath>
#include <random>

namespace bsl {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(mix(seed)) {}

  std::uint64_t next() {
    // xorshift64*, plenty for coin flips and geometric sampling.
    std::uint64_t x = state_;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state_ = x;
    return x * 0x2545F4914F6CDD1DULL;
  }

  // Uniform in [0,1).
  double uniform() {
    return static_cast<double>(next() >> 11) * 0x1.0p-53;
  }

  bool bernoulli(double q) { return uniform() < q; }

  // G ~ Geom_0(1-p): P[G = g] = (1-p) p^g, so P[G >= g] = p^g. Mean p/(1-p).
  // Closed form via inversion, so the cost does not grow as p -> 1.
  unsigned geometric0(double p) {
    if (p <= 0.0) return 0;
    double u = uniform();
    if (u <= 0.0) u = 0x1.0p-53;
    double g = std::log(u) / std::log(p);
    return static_cast<unsigned>(g);
  }

  // Uniform on {1,...,k}.
  unsigned uniform_int(unsigned k) {
    return static_cast<unsigned>(next() % k) + 1u;
  }

 private:
  static std::uint64_t mix(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return z ? z : 0x9E3779B97F4A7C15ULL;
  }

  std::uint64_t state_;
};

}  // namespace bsl
