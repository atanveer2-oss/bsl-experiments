// Unit tests for the paper's counter-based estimators (submission:
// def:estimators and Appendix A; the same definitions in the full version),
// run via `ctest`. They check the properties the proofs use, on the policies
// directly (one item, many independent trajectories) and through the skip
// list:
//
//   invariant   H >= rank after every operation, for p on both sides of 1/2;
//   excess      the exact estimator's excess H - rank has the tail of Lemma
//               "The excess", P[E >= e] = (1-theta_j) p^e + theta_j p^(e-1);
//   hazard      at a fixed time, P[H(j) >= l+1 | H(j) >= l] >= p;
//   crossing    P[H(j) >= l] <= j p^(l-1), (T1) with C = 1 and K = 1;
//   scale       after j operations the mean height is near log_b j;
//   legacy      the old exact rule leaves H < rank for p > 1/2, as recorded
//               in analysis/handoff-experiments.md.
//
// Every tolerance is a multiple of the binomial standard error at the sample
// size used, so with the fixed seeds the tests are deterministic, and a
// failure means the rule or the lemma is wrong, not that the sample was
// unlucky.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bsl/skiplist.hpp"
#include "bsl/zipf.hpp"

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
  }
}

std::string fmt(const char* f, double a, double b = 0, double c = 0) {
  char buf[256];
  std::snprintf(buf, sizeof buf, f, a, b, c);
  return buf;
}

// The rank each rule keeps, read off its state.
unsigned rank_of(const bsl::ExactEstimatorPolicy::State& s) { return s.rank; }
unsigned rank_of(const bsl::MorrisEstimatorPolicy::State& s) { return s.Q; }
unsigned rank_of(const bsl::ExactGatePolicy::State& s) { return s.r; }
unsigned rank_of(const bsl::GatedPolicy::State&) { return 0; }

// `trials` independent single-item trajectories of `ops` operations each,
// operation 1 being the insertion. Records the final height and rank of
// every trajectory, and counts the operations after which the height was
// below the rank or had decreased.
struct Run {
  std::vector<unsigned> height;
  std::vector<unsigned> rank;
  std::uint64_t violations = 0;
  std::uint64_t decreases = 0;
};

template <typename Policy>
Run simulate(const Policy& policy, std::uint64_t ops, std::size_t trials,
             std::uint64_t seed) {
  Run out;
  out.height.resize(trials);
  out.rank.resize(trials);
  bsl::Rng rng(seed);
  for (std::size_t t = 0; t < trials; ++t) {
    typename Policy::State s;
    unsigned h = policy.initial_height(s, rng);
    if (h < rank_of(s)) ++out.violations;
    for (std::uint64_t j = 2; j <= ops; ++j) {
      const unsigned h2 = policy.on_access(s, h, rng);
      if (h2 < h) ++out.decreases;
      if (h2 < rank_of(s)) ++out.violations;
      h = h2;
    }
    out.height[t] = h;
    out.rank[t] = rank_of(s);
  }
  return out;
}

// Number of trajectories with height >= l.
std::vector<std::uint64_t> tail_counts(const Run& run, unsigned max_level) {
  std::vector<std::uint64_t> at_least(max_level + 2, 0);
  for (unsigned h : run.height) {
    const unsigned top = std::min(h, max_level + 1);
    for (unsigned l = 0; l <= top; ++l) ++at_least[l];
  }
  return at_least;
}

double binomial_se(double q, double n) {
  return std::sqrt(std::max(q * (1.0 - q), 1e-12) / n);
}

// floor(log_b f) by the obvious formula, independent of the policy's loop.
unsigned rank_by_log(std::uint64_t f, double p) {
  return static_cast<unsigned>(
      std::floor(std::log(static_cast<double>(f)) / std::log(1.0 / p) + 1e-9));
}

// ---------------------------------------------------------------------------

// H >= rank after every operation, heights never decrease, for p on both
// sides of 1/2. Checked on the policies and, for the accessed key, through
// the skip list under a skewed workload.
void invariant_holds() {
  for (double p : {0.25, 0.5, 0.75}) {
    const auto exact =
        simulate(bsl::ExactEstimatorPolicy(p), 3000, 2000, 101);
    const auto morris =
        simulate(bsl::MorrisEstimatorPolicy(p), 3000, 2000, 102);
    check(exact.violations == 0,
          fmt("exact estimator: H >= rank at every step, p=%.2f", p));
    check(exact.decreases == 0,
          fmt("exact estimator: height never decreases, p=%.2f", p));
    check(morris.violations == 0,
          fmt("Morris estimator: H >= Q at every step, p=%.2f", p));
    check(morris.decreases == 0,
          fmt("Morris estimator: height never decreases, p=%.2f", p));
    for (std::uint64_t f : {1ull, 2ull, 3ull, 4ull, 7ull, 8ull, 9ull, 1000ull,
                            1ull << 20}) {
      check(bsl::ExactEstimatorPolicy::rank_of(f, p) == rank_by_log(f, p),
            fmt("exact estimator: rank_of(%g) is floor(log_b f), p=%.2f",
                static_cast<double>(f), p));
    }

    constexpr std::uint64_t kN = 300;
    bsl::ExactEstimatorSkipList elist(bsl::ExactEstimatorPolicy(p), 103);
    bsl::MorrisEstimatorSkipList mlist(bsl::MorrisEstimatorPolicy(p), 104);
    for (std::uint64_t i = 0; i < kN; ++i) {
      elist.insert(i);
      mlist.insert(i);
    }
    bsl::ZipfGenerator zipf(kN, 1.2, 105);
    bool ok_e = true, ok_m = true;
    for (int t = 0; t < 50000; ++t) {
      const std::uint64_t k = zipf.sample();
      elist.access(k);
      mlist.access(k);
      ok_e = ok_e && elist.height_of(k) >= rank_of(*elist.state_of(k)) &&
             elist.height_of(k) >= 1;
      ok_m = ok_m && mlist.height_of(k) >= rank_of(*mlist.state_of(k)) &&
             mlist.height_of(k) >= 1;
    }
    check(ok_e && elist.well_formed(),
          fmt("exact estimator in the skip list: H >= rank, p=%.2f", p));
    check(ok_m && mlist.well_formed(),
          fmt("Morris estimator in the skip list: H >= Q, p=%.2f", p));
    std::printf("  invariant p=%.2f: exact hot key h=%u rank=%u, Morris hot "
                "key h=%u Q=%u\n",
                p, elist.height_of(0), rank_of(*elist.state_of(0)),
                mlist.height_of(0), rank_of(*mlist.state_of(0)));
  }
}

// The handoff's reason for the new rules: the old exact gate promotes only
// when the rank has just risen and the height equals the previous rank, so a
// rank jump of two (possible for p > 1/2, where b < 2) strands the height
// below the rank. The paper's rule does not.
void legacy_exact_rule_breaks_the_invariant() {
  const auto legacy = simulate(bsl::ExactGatePolicy(0.75), 3000, 2000, 111);
  const auto paper =
      simulate(bsl::ExactEstimatorPolicy(0.75), 3000, 2000, 111);
  check(legacy.violations > 0,
        "legacy exact gate: H < rank observed at p = 0.75 (as recorded)");
  check(paper.violations == 0, "paper's exact estimator: none at p = 0.75");
  std::uint64_t below = 0;
  for (std::size_t t = 0; t < legacy.height.size(); ++t) {
    below += legacy.height[t] < legacy.rank[t];
  }
  std::printf("  legacy exact gate, p=0.75: %llu step violations, %llu of "
              "%zu items end below their rank; paper's rule: 0\n",
              static_cast<unsigned long long>(legacy.violations),
              static_cast<unsigned long long>(below), legacy.height.size());
}

// Lemma "The excess": with E = H - rank and theta_1 = 1,
// theta_j = kappa p^d, kappa = theta_{j-1} + p(1-theta_{j-1}),
// d = rank(j) - rank(j-1), the tail is
// P[E(j) >= e] = (1-theta_j) p^e + theta_j p^(e-1) for e >= 1.
// Compared with the empirical tail at powers of b and in between.
void exact_excess_law() {
  constexpr std::size_t kTrials = 100000;
  for (double p : {0.25, 0.5, 0.75}) {
    double worst_z = 0.0;
    for (std::uint64_t j : {1ull, 2ull, 3ull, 4ull, 8ull, 9ull, 100ull, 1000ull}) {
      double theta = 1.0;
      for (std::uint64_t i = 2; i <= j; ++i) {
        const double kappa = theta + p * (1.0 - theta);
        const unsigned d = rank_by_log(i, p) - rank_by_log(i - 1, p);
        theta = kappa * std::pow(p, static_cast<double>(d));
      }
      const auto run = simulate(bsl::ExactEstimatorPolicy(p), j, kTrials,
                                200 + j);
      const unsigned rank = rank_by_log(j, p);
      bool rank_ok = true;
      std::vector<std::uint64_t> excess_at_least(12, 0);
      for (std::size_t t = 0; t < kTrials; ++t) {
        rank_ok = rank_ok && run.rank[t] == rank;
        const unsigned e = run.height[t] - run.rank[t];
        for (unsigned k = 0; k <= std::min(e, 11u); ++k) ++excess_at_least[k];
      }
      check(rank_ok, fmt("exact estimator: rank after %g operations is "
                         "floor(log_b j), p=%.2f", static_cast<double>(j), p));
      for (unsigned e = 1; e <= 8; ++e) {
        const double predicted = (1.0 - theta) * std::pow(p, e) +
                                 theta * std::pow(p, e - 1.0);
        const double observed =
            static_cast<double>(excess_at_least[e]) / kTrials;
        const double se = binomial_se(predicted, kTrials);
        const double z = std::abs(observed - predicted) / se;
        if (predicted * kTrials >= 20.0) worst_z = std::max(worst_z, z);
        check(predicted * kTrials < 20.0 || z < 4.5,
              fmt("exact excess law at j=%g, e=%g, p=%.2f",
                  static_cast<double>(j), static_cast<double>(e), p));
        check(observed <= std::pow(p, e - 1.0) + 4.5 * se,
              fmt("exact excess tail <= p^(e-1) at j=%g, e=%g, p=%.2f",
                  static_cast<double>(j), static_cast<double>(e), p));
      }
    }
    std::printf("  excess law p=%.2f: worst |z| over j and e is %.2f "
                "(fails at 4.5)\n", p, worst_z);
  }
}

// Fixed-time hazard: P[H(j) >= l+1 | H(j) >= l] >= p at every level with
// enough mass to test, for the self-referential rule (K = 1 and K = 3), the
// exact estimator and the Morris estimator.
template <typename Policy>
void hazard_for(const char* name, const Policy& policy, double p,
                std::uint64_t seed) {
  constexpr std::size_t kTrials = 100000;
  double worst_margin = 1.0;
  unsigned levels_tested = 0;
  for (std::uint64_t j : {1ull, 10ull, 1000ull}) {
    const auto run = simulate(policy, j, kTrials, seed + j);
    const auto at_least = tail_counts(run, 60);
    for (unsigned l = 1; l <= 60; ++l) {
      const double n = static_cast<double>(at_least[l]);
      if (n < 1000.0) break;
      const double hazard = static_cast<double>(at_least[l + 1]) / n;
      const double margin = (hazard - p) / binomial_se(p, n);
      worst_margin = std::min(worst_margin, margin);
      ++levels_tested;
      check(margin > -4.5,
            fmt(std::string(name).append(": hazard >= p at level %g, j=%g, "
                                         "p=%.2f").c_str(),
                static_cast<double>(l), static_cast<double>(j), p));
    }
  }
  std::printf("  hazard %s p=%.2f: %u levels tested, worst margin %.2f SE\n",
              name, p, levels_tested, worst_margin);
}

void hazard_at_fixed_time() {
  for (double p : {0.25, 0.5, 0.75}) {
    hazard_for("gated K=1", bsl::GatedPolicy(p, 1), p, 301);
    hazard_for("gated K=3", bsl::GatedPolicy(p, 3), p, 302);
    hazard_for("exact", bsl::ExactEstimatorPolicy(p), p, 303);
    hazard_for("Morris", bsl::MorrisEstimatorPolicy(p), p, 304);
  }
}

// (T1) with C = 1: P[H(j) >= l] <= j p^(l-1), for the rules the appendix
// states it for with that constant (K = 1 and the two counter estimators).
template <typename Policy>
void crossing_for(const char* name, const Policy& policy, double p,
                  std::uint64_t seed) {
  constexpr std::size_t kTrials = 100000;
  double worst_ratio = 0.0;
  for (std::uint64_t j : {1ull, 2ull, 5ull, 50ull, 1000ull}) {
    const auto run = simulate(policy, j, kTrials, seed + j);
    const auto at_least = tail_counts(run, 60);
    for (unsigned l = 1; l <= 60; ++l) {
      const double observed = static_cast<double>(at_least[l]) / kTrials;
      const double bound = static_cast<double>(j) * std::pow(p, l - 1.0);
      if (bound >= 1.0 || at_least[l] < 20) continue;
      worst_ratio = std::max(worst_ratio, observed / bound);
      check(observed <= bound + 4.5 * binomial_se(bound, kTrials),
            fmt(std::string(name).append(": P[H(j) >= l] <= j p^(l-1) at "
                                         "l=%g, j=%g, p=%.2f").c_str(),
                static_cast<double>(l), static_cast<double>(j), p));
    }
  }
  std::printf("  crossing %s p=%.2f: worst observed/bound %.3f\n", name, p,
              worst_ratio);
}

void crossing_tail() {
  for (double p : {0.25, 0.5, 0.75}) {
    crossing_for("gated K=1", bsl::GatedPolicy(p, 1), p, 401);
    crossing_for("exact", bsl::ExactEstimatorPolicy(p), p, 402);
    crossing_for("Morris", bsl::MorrisEstimatorPolicy(p), p, 403);
  }
}

// Section 2's scale: an item accessed j times sits near log_b j. The exact
// estimator's mean height is log_b j plus the mean excess, at most 1/(1-p);
// the others are allowed the jump K and a few geometric means of slack. The
// Morris counter with base b has E[b^Q(j)] = b + (j-1)(b-1), so its scale is
// log_b(j(b-1)), which is log_b j shifted by the constant log_b(b-1): zero at
// p = 1/2, positive below it and negative above it.
template <typename Policy>
void scale_for(const char* name, const Policy& policy, double p, unsigned K,
               bool morris = false) {
  constexpr std::uint64_t kJ = 4096;
  const auto run = simulate(policy, kJ, 20000, 501);
  double mean = 0.0;
  for (unsigned h : run.height) mean += h;
  mean /= static_cast<double>(run.height.size());
  const double b = 1.0 / p;
  const double scale = morris ? static_cast<double>(kJ) * (b - 1.0)
                              : static_cast<double>(kJ);
  const double target = std::log(scale) / std::log(b);
  const double slack = K + 3.0 * p / (1.0 - p) + 2.0;
  check(mean >= target - 1.0 && mean <= target + slack,
        fmt(std::string(name).append(": mean height near log_b j, p=%.2f")
                .c_str(), p));
  std::printf("  scale %s p=%.2f: mean height %.2f, target %.2f\n", name, p,
              mean, target);
}

void heights_track_log_b_j() {
  for (double p : {0.25, 0.5, 0.75}) {
    scale_for("gated K=1", bsl::GatedPolicy(p, 1), p, 1);
    scale_for("gated K=3", bsl::GatedPolicy(p, 3), p, 3);
    scale_for("exact", bsl::ExactEstimatorPolicy(p), p, 1);
    scale_for("Morris", bsl::MorrisEstimatorPolicy(p), p, 1, /*morris=*/true);
  }
}

}  // namespace

int main() {
  std::printf("bsl estimator checks\n");
  invariant_holds();
  legacy_exact_rule_breaks_the_invariant();
  exact_excess_law();
  hazard_at_fixed_time();
  crossing_tail();
  heights_track_log_b_j();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
