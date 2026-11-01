// Structural experiment: does each variant's shape converge to the access
// distribution, does its total search cost track the entropy benchmark
// Ent(sigma), how do the depth and the top level grow, what does the fixed-
// time hazard look like across items, and how much space does the compressed
// representation of the submission's Section "Space" take?
//
// Arms (p = 0.5 unless --p is given):
//   classic          control: heights fixed at insertion
//   gated-K1         self-referential gate (Definition 1.1), K = 1
//   gated-K3         self-referential gate, K = 3
//   exact            exact estimator, the paper's rule (def:estimators)
//   morris           Morris estimator, the paper's rule (def:estimators)
//   exact-legacy     the old RSASL exact gate, verbatim (baseline only)
//   morris-legacy    the old RSASLA Morris gate, verbatim (baseline only)
// With --floor=1 (full version only; the weight floor is cut from the
// submission) these are added, all with the clocked floor of c0:
//   gated-K3+floor, exact+floor, morris+floor,
//   exact-legacy+floor, morris-legacy+floor
//
// Distributions (--dist=):
//   zipf<s>    Zipf with exponent s over the ranks, e.g. zipf1.5 (default)
//   uniform    every key equally likely (the adversarial control)
//   hot<A>-<B> A% of the accesses to a random B% of the keys, uniform within
//              each part, e.g. hot90-10 (the SetBench protocol's workload)
// Keys get a fresh random permutation of the ranks per run, so key order and
// rank are uncorrelated. Within a run every arm sees the same sequence.
//
// Accounting (unchanged): n keys are inserted first and not costed; then F
// accesses, each costed as served (vertical span + horizontal steps, before
// any promotion). Ent and A are computed over the accesses.
//
// Output is CSV on stdout for scripts/plot_structural.py.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numeric>
#include <string>
#include <type_traits>
#include <vector>

#include "bsl/skiplist.hpp"
#include "bsl/zipf.hpp"

namespace {

struct Config {
  std::size_t keys = 1000;
  std::uint64_t accesses = 500000;
  std::string dist = "zipf1.5";
  double p = 0.5;
  int c0 = 1;  // def:floor requires c_0 >= 1; at c_0 = 0 the floor reacts to the
              // startup transient where n_t is 1 and t/n_t is momentarily large.
  bool floor = false;
  int runs = 10;
  std::uint64_t seed = 12345;
  std::uint64_t sample_every = 10000;
  // --items=PATH: at t = F, one row per item (arm, run, key, f_x, H_x), so
  // the distribution of the height gap H_x - log_b(1 + f_x) across items can
  // be drawn (Section "Height concentration", lem:heightupper).
  std::string items_path;
  // --hist=PATH: at t = F, the whole-run histogram of cost_t / a_t (bins of
  // kRatioBin, lower edge given) and the largest ratio, one row per arm, run
  // and non-empty bin, so the distribution of the per-search ratio can be
  // drawn as a box plot (Theorem A) rather than summarised by q99 and max.
  std::string hist_path;
};

FILE* g_items = nullptr;
FILE* g_hist = nullptr;

// Levels with fewer items than this are left out of hazard_min.
constexpr std::size_t kHazardMinItems = 50;

// Histogram of cost_t / a_t for the per-search tail (Theorem A).
constexpr double kRatioBin = 0.05;
constexpr std::size_t kRatioBins = 4000;  // up to 200; the last bin overflows

void shuffle(std::vector<std::size_t>& v, bsl::Rng& rng) {
  for (std::size_t i = v.size(); i > 1; --i) {
    const std::size_t j = rng.next() % i;
    std::swap(v[i - 1], v[j]);
  }
}

// One run's access sequence and the distribution it is drawn from, indexed by
// key. Ranks are mapped to keys through a random permutation.
class Workload {
 public:
  Workload(const Config& cfg, int run)
      : n_(cfg.keys),
        perm_(cfg.keys),
        opt_(cfg.keys),
        rng_(cfg.seed + static_cast<std::uint64_t>(run) * 104729) {
    std::iota(perm_.begin(), perm_.end(), std::size_t{0});
    bsl::Rng perm_rng(cfg.seed + 1000003 +
                      static_cast<std::uint64_t>(run) * 15485863);
    shuffle(perm_, perm_rng);

    const std::string& d = cfg.dist;
    int hot_pct = 0, key_pct = 0;
    if (d.rfind("zipf", 0) == 0) {
      kind_ = Kind::kZipf;
      const double s = std::stod(d.substr(4));
      zipf_ = std::make_unique<bsl::ZipfGenerator>(
          n_, s, cfg.seed + static_cast<std::uint64_t>(run) * 104729);
      for (std::size_t r = 0; r < n_; ++r) opt_[perm_[r]] = zipf_->prob(r);
    } else if (d == "uniform") {
      kind_ = Kind::kUniform;
      for (auto& o : opt_) o = 1.0 / static_cast<double>(n_);
    } else if (std::sscanf(d.c_str(), "hot%d-%d", &hot_pct, &key_pct) == 2 &&
               hot_pct > 0 && hot_pct < 100 && key_pct > 0 && key_pct < 100) {
      kind_ = Kind::kHot;
      hot_mass_ = hot_pct / 100.0;
      hot_ = std::max<std::size_t>(
          1, static_cast<std::size_t>(std::llround(n_ * (key_pct / 100.0))));
      if (hot_ >= n_) hot_ = n_ - 1;
      for (std::size_t r = 0; r < n_; ++r) {
        opt_[perm_[r]] = r < hot_ ? hot_mass_ / static_cast<double>(hot_)
                                  : (1.0 - hot_mass_) /
                                        static_cast<double>(n_ - hot_);
      }
    } else {
      std::fprintf(stderr, "structural: unknown --dist=%s\n", d.c_str());
      std::exit(2);
    }
  }

  std::size_t next() {
    switch (kind_) {
      case Kind::kZipf:
        return perm_[zipf_->sample()];
      case Kind::kUniform:
        return perm_[rng_.next() % n_];
      case Kind::kHot:
        if (rng_.bernoulli(hot_mass_)) return perm_[rng_.next() % hot_];
        return perm_[hot_ + rng_.next() % (n_ - hot_)];
    }
    return 0;
  }

  const std::vector<double>& opt() const { return opt_; }

 private:
  enum class Kind { kZipf, kUniform, kHot };
  Kind kind_ = Kind::kZipf;
  std::size_t n_;
  std::vector<std::size_t> perm_;
  std::vector<double> opt_;
  bsl::Rng rng_;
  std::unique_ptr<bsl::ZipfGenerator> zipf_;
  double hot_mass_ = 0.0;
  std::size_t hot_ = 0;
};

// OBS(x): the distribution the shape implies, item at height h carries b^h.
std::vector<double> observed_distribution(const std::vector<unsigned>& heights,
                                          double b) {
  std::vector<double> obs(heights.size());
  double total = 0.0;
  for (std::size_t i = 0; i < heights.size(); ++i) {
    obs[i] = std::pow(b, static_cast<double>(heights[i]));
    total += obs[i];
  }
  for (auto& o : obs) o /= total;
  return obs;
}

double kl_divergence(const std::vector<double>& opt,
                     const std::vector<double>& obs) {
  double kl = 0.0;
  for (std::size_t i = 0; i < opt.size(); ++i) {
    if (opt[i] <= 0.0 || obs[i] <= 0.0) continue;
    kl += opt[i] * std::log(opt[i] / obs[i]);
  }
  return kl;
}

// Bits of per-item estimator state, ceil(log2(v + 1)) for the stored value v.
// The counter-free rules store nothing beyond the height.
template <typename Policy>
std::uint64_t estimator_bits(const typename Policy::State& s) {
  if constexpr (std::is_same_v<Policy, bsl::ExactEstimatorPolicy> ||
                std::is_same_v<Policy, bsl::ExactGatePolicy>) {
    return std::bit_width(static_cast<std::uint64_t>(s.f));
  } else if constexpr (std::is_same_v<Policy, bsl::MorrisEstimatorPolicy>) {
    return std::bit_width(static_cast<std::uint64_t>(s.Q));
  } else if constexpr (std::is_same_v<Policy, bsl::MorrisGatePolicy>) {
    return std::bit_width(static_cast<std::uint64_t>(s.R));
  } else {
    (void)s;
    return 0;
  }
}

// min over levels l holding >= kHazardMinItems items of
// #{H >= l+1} / #{H >= l}: an across-items view of the fixed-time hazard,
// not the per-item quantity of cor:hazard. NaN if no level qualifies.
double hazard_min(const std::vector<unsigned>& heights) {
  unsigned top = 0;
  for (unsigned h : heights) top = std::max(top, h);
  std::vector<std::size_t> at(top + 3, 0);
  for (unsigned h : heights) ++at[h];
  std::vector<std::size_t> ge(top + 3, 0);
  for (unsigned l = top + 1; l-- > 0;) ge[l] = at[l] + ge[l + 1];
  double m = NAN;
  for (unsigned l = 1; l <= top; ++l) {
    if (ge[l] < kHazardMinItems) break;
    const double r = static_cast<double>(ge[l + 1]) / static_cast<double>(ge[l]);
    if (std::isnan(m) || r < m) m = r;
  }
  return m;
}

// Upper edge of the bin holding the q-quantile, capped at the observed max.
double ratio_quantile(const std::vector<std::uint64_t>& hist,
                      std::uint64_t total, double q, double max) {
  const std::uint64_t target =
      static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(total)));
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < hist.size(); ++i) {
    acc += hist[i];
    if (acc >= target) return std::min(max, static_cast<double>(i + 1) * kRatioBin);
  }
  return max;
}

template <typename Policy>
void run_arm(const char* label, Policy policy, bsl::FloorConfig floor,
             const Config& cfg, int run) {
  bsl::SkipList<Policy> list(policy,
                             cfg.seed + static_cast<std::uint64_t>(run) * 7919,
                             floor);
  for (std::size_t i = 0; i < cfg.keys; ++i) list.insert(i);

  Workload work(cfg, run);
  const double b = 1.0 / cfg.p;
  const double ln_b = std::log(b);
  const std::size_t n = cfg.keys;
  const unsigned lg_n = static_cast<unsigned>(std::bit_width(n));  // ceil(log2(n+1))

  std::vector<std::uint64_t> freqs(n, 0);
  std::vector<unsigned> heights(n, 0);
  std::vector<std::uint64_t> ratio_hist(kRatioBins, 0);
  std::uint64_t cost_total = 0;
  double a_total = 0.0;
  double ratio_max = 0.0;

  for (std::uint64_t t = 1; t <= cfg.accesses; ++t) {
    const std::size_t key = work.next();
    ++freqs[key];
    const std::uint64_t c = list.access(key).total();
    cost_total += c;

    // a_t = 1 + log_b(charges so far / operations on x_t so far), with the n
    // insertions counted as charges as in the paper; Theorem A's benchmark.
    const double a = 1.0 + std::log(static_cast<double>(n + t) /
                                     static_cast<double>(1 + freqs[key])) /
                               ln_b;
    a_total += a;
    const double ratio = static_cast<double>(c) / a;
    ratio_max = std::max(ratio_max, ratio);
    ++ratio_hist[std::min<std::size_t>(kRatioBins - 1,
                                       static_cast<std::size_t>(ratio / kRatioBin))];

    if (t % cfg.sample_every == 0 || t == cfg.accesses) {
      std::uint64_t est_bits = 0;
      list.for_each_item([&](std::uint64_t k, unsigned h,
                             const typename Policy::State& s) {
        heights[k] = h;
        est_bits += estimator_bits<Policy>(s);
      });
      if (g_items && t == cfg.accesses) {
        for (std::size_t i = 0; i < n; ++i) {
          std::fprintf(g_items, "%s,%d,%zu,%llu,%u,%.3f\n", label, run, i,
                       static_cast<unsigned long long>(freqs[i]), heights[i],
                       cfg.p);
        }
      }
      if (g_hist && t == cfg.accesses) {
        for (std::size_t i = 0; i < ratio_hist.size(); ++i) {
          if (ratio_hist[i] == 0) continue;
          std::fprintf(g_hist, "%s,%d,%.2f,%llu,%.4f,%s,%zu,%.3f\n", label, run,
                       static_cast<double>(i) * kRatioBin,
                       static_cast<unsigned long long>(ratio_hist[i]),
                       ratio_max, cfg.dist.c_str(), n, cfg.p);
        }
      }
      const auto obs = observed_distribution(heights, b);
      const double kl = kl_divergence(work.opt(), obs);
      const double ent = bsl::entropy_benchmark(freqs, b);
      // Depth = top level minus the lowest item; the vertical span a cold
      // search pays. Raise work is the sweep cost charged to the floor.
      const unsigned hmax = list.level();
      const unsigned depth = hmax - list.min_height();
      const std::uint64_t ptr_unc = list.pointer_count();
      const std::uint64_t ptr_c = list.compressed_pointer_count(true);
      const std::uint64_t ptr_c_items = list.compressed_pointer_count(false);
      const unsigned lg_h = static_cast<unsigned>(std::bit_width(hmax));
      const double nd = static_cast<double>(n);
      // Compressed: every pointer carries a span counter; head included.
      const double bits = (static_cast<double>(ptr_c) * (lg_n + lg_h) +
                           static_cast<double>(est_bits)) / nd;
      // Uncompressed: pointer bits only, no estimator state.
      const double bits_unc = static_cast<double>(ptr_unc) * lg_n / nd;
      const double hz = hazard_min(heights);
      // Section "Height concentration": H_x(j) = log_b j + O(1), with j the
      // operations on x so far, insertion included. Gap over all items.
      double gap_sum = 0.0, gap_min = INFINITY, gap_max = -INFINITY;
      for (std::size_t i = 0; i < n; ++i) {
        const double g = static_cast<double>(heights[i]) -
                         std::log(static_cast<double>(1 + freqs[i])) / ln_b;
        gap_sum += g;
        gap_min = std::min(gap_min, g);
        gap_max = std::max(gap_max, g);
      }

      std::printf(
          "%s,%d,%llu,%.6f,%llu,%.3f,%.4f,%.3f,%.4f,%.3f,%.3f,%u,%u,%llu,%llu,"
          "%llu,%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,%llu,%llu,%s,%zu,%.3f,"
          "%llu\n",
          label, run, static_cast<unsigned long long>(t), kl,
          static_cast<unsigned long long>(cost_total), ent,
          ent > 0.0 ? static_cast<double>(cost_total) / ent : 0.0, a_total,
          static_cast<double>(cost_total) / a_total,
          ratio_quantile(ratio_hist, t, 0.99, ratio_max), ratio_max, depth, hmax,
          static_cast<unsigned long long>(ptr_unc),
          static_cast<unsigned long long>(ptr_c),
          static_cast<unsigned long long>(ptr_c_items),
          static_cast<unsigned long long>(est_bits), bits, bits_unc, hz,
          gap_sum / nd, gap_min, gap_max,
          list.floor_level(), static_cast<unsigned long long>(list.raises()),
          static_cast<unsigned long long>(list.raise_work()), cfg.dist.c_str(),
          n, cfg.p, static_cast<unsigned long long>(cfg.seed));
    }
  }
}

void parse_args(int argc, char** argv, Config* cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto eq = a.find('=');
    if (a.rfind("--", 0) != 0 || eq == std::string::npos) {
      std::fprintf(stderr, "structural: ignoring argument %s\n", a.c_str());
      continue;
    }
    const std::string k = a.substr(2, eq - 2);
    const std::string v = a.substr(eq + 1);
    if (k == "keys") cfg->keys = std::stoull(v);
    else if (k == "accesses") cfg->accesses = std::stoull(v);
    else if (k == "dist") cfg->dist = v;
    else if (k == "zipf") cfg->dist = "zipf" + v;  // older spelling
    else if (k == "p") cfg->p = std::stod(v);
    else if (k == "c0") cfg->c0 = std::stoi(v);
    else if (k == "floor") cfg->floor = std::stoi(v) != 0;
    else if (k == "runs") cfg->runs = std::stoi(v);
    else if (k == "seed") cfg->seed = std::stoull(v);
    else if (k == "sample_every") cfg->sample_every = std::stoull(v);
    else if (k == "items") cfg->items_path = v;
    else if (k == "hist") cfg->hist_path = v;
    else std::fprintf(stderr, "structural: unknown flag --%s\n", k.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  parse_args(argc, argv, &cfg);
  if (cfg.keys < 2 || cfg.sample_every == 0) {
    std::fprintf(stderr, "structural: need --keys >= 2, --sample_every >= 1\n");
    return 2;
  }

  std::fprintf(stderr,
               "keys=%zu accesses=%llu dist=%s p=%.3f floor=%d c0=%d runs=%d "
               "seed=%llu sample_every=%llu\n",
               cfg.keys, static_cast<unsigned long long>(cfg.accesses),
               cfg.dist.c_str(), cfg.p, cfg.floor ? 1 : 0, cfg.c0, cfg.runs,
               static_cast<unsigned long long>(cfg.seed),
               static_cast<unsigned long long>(cfg.sample_every));

  const bsl::FloorConfig no_floor{};
  const bsl::FloorConfig floor{true, cfg.c0};
  const double p = cfg.p;

  if (!cfg.items_path.empty()) {
    g_items = std::fopen(cfg.items_path.c_str(), "w");
    if (!g_items) {
      std::fprintf(stderr, "cannot open %s\n", cfg.items_path.c_str());
      return 1;
    }
    std::fprintf(g_items, "policy,run,key,f,h,p\n");
  }
  if (!cfg.hist_path.empty()) {
    g_hist = std::fopen(cfg.hist_path.c_str(), "w");
    if (!g_hist) {
      std::fprintf(stderr, "cannot open %s\n", cfg.hist_path.c_str());
      return 1;
    }
    std::fprintf(g_hist, "policy,run,bin_lo,count,ratio_max,dist,n,p\n");
  }

  std::printf("policy,run,t,kl,cost_total,entropy,cost_over_entropy,a_total,"
              "cost_over_a,ratio_q99,ratio_max,depth,hmax,ptr_uncompressed,"
              "ptr_compressed,ptr_compressed_items,estimator_bits,"
              "bits_per_item,bits_per_item_uncompressed,hazard_min,hgap_mean,"
              "hgap_min,hgap_max,floor,"
              "raises,raise_work,dist,n,p,seed\n");
  for (int run = 0; run < cfg.runs; ++run) {
    run_arm("classic", bsl::ClassicPolicy(p), no_floor, cfg, run);
    run_arm("gated-K1", bsl::GatedPolicy(p, 1), no_floor, cfg, run);
    run_arm("gated-K3", bsl::GatedPolicy(p, 3), no_floor, cfg, run);
    run_arm("exact", bsl::ExactEstimatorPolicy(p), no_floor, cfg, run);
    run_arm("morris", bsl::MorrisEstimatorPolicy(p), no_floor, cfg, run);
    run_arm("exact-legacy", bsl::ExactGatePolicy(p), no_floor, cfg, run);
    run_arm("morris-legacy", bsl::MorrisGatePolicy(p), no_floor, cfg, run);
    if (cfg.floor) {
      run_arm("gated-K3+floor", bsl::GatedPolicy(p, 3), floor, cfg, run);
      run_arm("exact+floor", bsl::ExactEstimatorPolicy(p), floor, cfg, run);
      run_arm("morris+floor", bsl::MorrisEstimatorPolicy(p), floor, cfg, run);
      run_arm("exact-legacy+floor", bsl::ExactGatePolicy(p), floor, cfg, run);
      run_arm("morris-legacy+floor", bsl::MorrisGatePolicy(p), floor, cfg,
              run);
    }
    std::fprintf(stderr, "run %d done\n", run);
  }
  if (g_items) std::fclose(g_items);
  if (g_hist) std::fclose(g_hist);
  return 0;
}
