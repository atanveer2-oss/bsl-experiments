// Reseeding experiment: the measurement behind the reseeding subsection of the
// combined paper (Definition "Epochal reseeding", Theorem "High-probability
// static optimality by reseeding", Proposition "Persistence barrier").
//
// Two questions:
//   (1) Does epochal reseeding cost anything on stationary profiles, and does
//       it help on drifting ones? Measured as total cost / Ent(sigma), and
//       total cost / (sum over epochs of the epoch-local entropy).
//   (2) The tail of cost / F on the persistence-barrier sequence, persistent
//       vs reseeded, one-gate rule (K = 1).
//   (3) Follow-ups: a clocked-ceiling arm on the barrier (CeilingConfig, c1 =
//       2 and 4, optionally with the top-up), and a fast-drift "phases" profile (phase k hammers key k
//       for n accesses) where the persistent cost should grow like log_b T.
//   (4) The shadow (double-buffered) rebuild, ReseedConfig::shadow, against
//       the eager rebuild and the persistent process (--only=shadow): same
//       cost and concentration expected, and the worst single operation
//       (SkipList::max_op_work) should drop from Theta(n) to O(log n + n/N).
//
// Cost = search cost (vertical span + horizontal steps, SearchCost::total())
// of every access, plus all rebuild work for the reseeded arms. Profiles a-c
// count accesses only (as structural.cpp does); the barrier counts insertions
// too, since the proposition's F = 2m + 2n does.
//
// Horizontal-only cost (sum of SearchCost::horizontal, no vertical part and no
// rebuild work) is recorded alongside: tree depth in the zip-tree / treap
// correspondence is at most 1 + horizontal steps, so it is the proxy for the
// tree hosts' cost.
//
// Output: results/reseed_<profile>.csv, and markdown summary tables on stdout.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "bsl/rng.hpp"
#include "bsl/skiplist.hpp"
#include "bsl/zipf.hpp"

namespace {

struct Config {
  std::size_t keys = 1000;
  std::uint64_t accesses_per_key = 200;  // F = 200 n
  std::uint64_t drift_period_per_key = 50;
  int runs = 20;
  int barrier_runs = 200;
  std::vector<std::size_t> barrier_ns = {100, 1000};
  double barrier_fmax = 5e6;
  double p = 0.5;
  unsigned K = 3;
  std::uint64_t seed = 20261003;
  unsigned threads = 0;
  std::string out = "results";
  std::string only;  // run a single profile family: stationary|drift|barrier
  std::string suffix;  // appended to CSV names, e.g. "_R2000"
  std::vector<std::string> barrier_arms;  // empty = all
  std::vector<std::size_t> phase_ns = {256, 1024};
  std::vector<std::size_t> phase_Ts = {4, 16, 64, 256, 1024};  // T <= n only
  std::size_t shadow_phase_n = 1024;
  std::size_t shadow_phase_T = 1024;
};

// Runs job(i) for i in [0, count) on a small thread pool.
void parallel_for(int count, unsigned threads,
                  const std::function<void(int)>& job) {
  std::atomic<int> next{0};
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      for (int i = next++; i < count; i = next++) job(i);
    });
  }
  for (auto& th : pool) th.join();
}

void shuffle(std::vector<std::uint32_t>& v, bsl::Rng& rng) {
  for (std::size_t i = v.size(); i > 1; --i) {
    const std::size_t j = rng.next() % i;
    std::swap(v[i - 1], v[j]);
  }
}

double mean(const std::vector<double>& v) {
  return std::accumulate(v.begin(), v.end(), 0.0) /
         static_cast<double>(v.size());
}

double sd(const std::vector<double>& v) {
  if (v.size() < 2) return 0.0;
  const double m = mean(v);
  double acc = 0.0;
  for (double x : v) acc += (x - m) * (x - m);
  return std::sqrt(acc / static_cast<double>(v.size() - 1));
}

// Nearest-rank empirical quantile.
double quantile(std::vector<double> v, double q) {
  std::sort(v.begin(), v.end());
  std::size_t k = static_cast<std::size_t>(
      std::ceil(q * static_cast<double>(v.size())));
  if (k < 1) k = 1;
  if (k > v.size()) k = v.size();
  return v[k - 1];
}

// ---------------------------------------------------------------------------
// Profiles a-c: stationary and drifting
// ---------------------------------------------------------------------------

struct Workload {
  std::vector<std::uint32_t> seq;  // accessed keys, in order
  double ent_global = 0.0;         // Ent(sigma) over the global frequencies
  double ent_local = 0.0;          // sum over epochs of Ent(epoch-local freqs)
  std::uint64_t epochs = 0;
};

// Replays the epoch schedule (n insertions, then the accesses) and sums the
// entropy benchmark of each epoch's access frequencies. The schedule depends
// only on sigma, so this is the same for every arm.
void epoch_entropies(std::size_t n, double b, Workload* w) {
  bsl::EpochSchedule sched;
  std::vector<std::uint64_t> counts(n, 0);
  std::vector<std::uint32_t> touched;
  std::uint64_t L = 0;
  double acc = 0.0;
  const double ln_b = std::log(b);
  auto close = [&] {
    for (auto k : touched) {
      const double a = static_cast<double>(counts[k]);
      acc += a * std::log(static_cast<double>(L) / a) / ln_b;
      counts[k] = 0;
    }
    acc += static_cast<double>(L);
    touched.clear();
    L = 0;
  };
  for (std::size_t i = 0; i < n; ++i) {
    if (sched.charge(true)) close();
  }
  for (auto k : w->seq) {
    if (sched.charge(false)) close();
    if (counts[k]++ == 0) touched.push_back(k);
    ++L;
  }
  close();
  w->ent_local = acc;
  w->epochs = sched.epochs();
  std::vector<std::uint64_t> freqs(n, 0);
  for (auto k : w->seq) ++freqs[k];
  w->ent_global = bsl::entropy_benchmark(freqs, b);
}

// kind: 0 uniform, 1 zipf (stationary, fixed random key permutation),
// 2 drift (zipf whose key permutation is redrawn every `period` accesses).
Workload make_workload(int kind, double s, const Config& cfg, int run) {
  const std::size_t n = cfg.keys;
  const std::uint64_t F = cfg.accesses_per_key * n;
  const std::uint64_t period = cfg.drift_period_per_key * n;
  const std::uint64_t base =
      cfg.seed + 1000003ULL * static_cast<std::uint64_t>(run) +
      7919ULL * static_cast<std::uint64_t>(kind);
  bsl::Rng rng(base);
  Workload w;
  w.seq.reserve(F);
  std::vector<std::uint32_t> perm(n);
  std::iota(perm.begin(), perm.end(), 0u);
  shuffle(perm, rng);
  if (kind == 0) {
    for (std::uint64_t t = 0; t < F; ++t) {
      w.seq.push_back(static_cast<std::uint32_t>(rng.next() % n));
    }
  } else {
    bsl::ZipfGenerator zipf(n, s, base ^ 0x5bd1e995ULL);
    for (std::uint64_t t = 0; t < F; ++t) {
      if (kind == 2 && t > 0 && t % period == 0) shuffle(perm, rng);
      w.seq.push_back(perm[zipf.sample()]);
    }
  }
  epoch_entropies(n, 1.0 / cfg.p, &w);
  return w;
}

struct ArmResult {
  std::uint64_t access_cost = 0;
  std::uint64_t horizontal = 0;
  std::uint64_t rebuild_work = 0;
  std::uint64_t epochs = 0;
  unsigned level = 0;
};

template <typename Policy>
ArmResult run_sequence(Policy policy, bool reseed, std::size_t n,
                       const std::vector<std::uint32_t>& seq,
                       std::uint64_t seed) {
  bsl::SkipList<Policy> list(policy, seed, bsl::FloorConfig{},
                             bsl::ReseedConfig{reseed});
  for (std::size_t i = 0; i < n; ++i) list.insert(i);
  ArmResult r;
  for (auto k : seq) {
    const bsl::SearchCost c = list.access(k);
    r.access_cost += c.total();
    r.horizontal += c.horizontal;
  }
  r.rebuild_work = list.rebuild_work();
  r.epochs = list.epochs();
  r.level = list.level();
  return r;
}

struct Arm {
  std::string name;
  int policy;  // 0 gated, 1 exact, 2 morris
  bool reseed;
};

const std::vector<Arm> kArms = {
    {"gated", 0, false},  {"gated+reseed", 0, true},
    {"exact", 1, false},  {"exact+reseed", 1, true},
    {"morris", 2, false}, {"morris+reseed", 2, true},
};

ArmResult run_arm(const Arm& arm, const Config& cfg,
                  const std::vector<std::uint32_t>& seq, std::uint64_t seed) {
  switch (arm.policy) {
    case 0:
      return run_sequence(bsl::GatedPolicy(cfg.p, cfg.K), arm.reseed,
                          cfg.keys, seq, seed);
    case 1:
      return run_sequence(bsl::ExactGatePolicy(cfg.p), arm.reseed, cfg.keys,
                          seq, seed);
    default:
      return run_sequence(bsl::MorrisGatePolicy(cfg.p), arm.reseed, cfg.keys,
                          seq, seed);
  }
}

void run_profile(const char* profile, int kind, double s, const Config& cfg) {
  const int R = cfg.runs;
  std::vector<Workload> work(R);
  parallel_for(R, cfg.threads,
               [&](int run) { work[run] = make_workload(kind, s, cfg, run); });

  const int A = static_cast<int>(kArms.size());
  std::vector<ArmResult> res(static_cast<std::size_t>(R * A));
  parallel_for(R * A, cfg.threads, [&](int job) {
    const int run = job / A, a = job % A;
    // Same structure seed for persistent and reseeded arms of one run.
    const std::uint64_t seed =
        cfg.seed * 31ULL + 104729ULL * static_cast<std::uint64_t>(run) +
        static_cast<std::uint64_t>(kArms[a].policy);
    res[job] = run_arm(kArms[a], cfg, work[run].seq, seed);
  });

  const std::string path = cfg.out + "/reseed_" + profile + cfg.suffix + ".csv";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    std::perror(path.c_str());
    return;
  }
  std::fprintf(f,
               "profile,arm,run,n,F,access_cost,rebuild_work,total_cost,"
               "ent_global,ent_local,cost_over_ent,cost_over_local_ent,"
               "rebuild_per_access,epochs,final_level,horizontal_cost,"
               "horizontal_over_F\n");
  const double F = static_cast<double>(cfg.accesses_per_key * cfg.keys);
  for (int run = 0; run < R; ++run) {
    for (int a = 0; a < A; ++a) {
      const ArmResult& r = res[static_cast<std::size_t>(run * A + a)];
      const double total =
          static_cast<double>(r.access_cost + r.rebuild_work);
      std::fprintf(f, "%s,%s,%d,%zu,%.0f,%llu,%llu,%.0f,%.3f,%.3f,%.5f,%.5f,"
                      "%.5f,%llu,%u,%llu,%.5f\n",
                   profile, kArms[a].name.c_str(), run, cfg.keys, F,
                   static_cast<unsigned long long>(r.access_cost),
                   static_cast<unsigned long long>(r.rebuild_work), total,
                   work[run].ent_global, work[run].ent_local,
                   total / work[run].ent_global, total / work[run].ent_local,
                   static_cast<double>(r.rebuild_work) / F,
                   static_cast<unsigned long long>(r.epochs), r.level,
                   static_cast<unsigned long long>(r.horizontal),
                   static_cast<double>(r.horizontal) / F);
    }
  }
  std::fclose(f);

  std::vector<double> eg, el;
  for (const auto& w : work) {
    eg.push_back(w.ent_global / F);
    el.push_back(w.ent_local / F);
  }
  std::printf("\n### %s (n = %zu, F = %.0f, R = %d)\n\n", profile, cfg.keys,
              F, R);
  std::printf("Ent/F = %.3f, local Ent/F = %.3f, epochs = %llu\n\n", mean(eg),
              mean(el), static_cast<unsigned long long>(work[0].epochs));
  std::printf("| arm | mean cost/Ent | sd | mean cost/local Ent | sd | "
              "rebuild/F | mean final level | mean horizontal/F |\n");
  std::printf("|---|---|---|---|---|---|---|---|\n");
  for (int a = 0; a < A; ++a) {
    std::vector<double> cg, cl, rw, lv, hz;
    for (int run = 0; run < R; ++run) {
      const ArmResult& r = res[static_cast<std::size_t>(run * A + a)];
      const double total =
          static_cast<double>(r.access_cost + r.rebuild_work);
      cg.push_back(total / work[run].ent_global);
      cl.push_back(total / work[run].ent_local);
      rw.push_back(static_cast<double>(r.rebuild_work) / F);
      lv.push_back(r.level);
      hz.push_back(static_cast<double>(r.horizontal) / F);
    }
    std::printf("| %s | %.4f | %.4f | %.4f | %.4f | %.3f | %.1f | %.3f |\n",
                kArms[a].name.c_str(), mean(cg), sd(cg), mean(cl), sd(cl),
                mean(rw), mean(lv), mean(hz));
  }
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Profile d: the persistence barrier
// ---------------------------------------------------------------------------

struct BarrierResult {
  double total_cost = 0.0;
  double x_block_cost = 0.0;
  std::uint64_t horizontal = 0;          // all searches, insertions included
  std::uint64_t x_block_horizontal = 0;
  std::uint64_t rebuild_work = 0;
  unsigned h_y = 0, h_x = 0, h_max = 0;  // at the end of the y-block
  std::uint64_t topup_work = 0;
  unsigned lambda_end = 0;      // ceiling at the end (64 with no ceiling)
  std::size_t q_end = 0;        // items at height Lambda at the end (Q_t)
  std::size_t top_end = 0;      // items at height H_max at the end
  unsigned h_max_end = 0;
};

struct BarrierArm {
  const char* name;
  bool reseed;
  int c1;  // clocked ceiling offset; 0 = no ceiling
  bool topup = false;
};

const std::vector<BarrierArm> kBarrierArms = {
    {"gated1", false, 0},
    {"gated1+reseed", true, 0},
    {"gated1+ceiling2", false, 2},
    {"gated1+ceiling4", false, 4},
    {"gated1+ceiling2+topup", false, 2, true},
    {"gated1+ceiling4+topup", false, 4, true},
};

// Insert n items, access each once, access y m times, then x m times.
BarrierResult run_barrier(const BarrierArm& arm, std::size_t n,
                          std::uint64_t m, double p, std::uint64_t seed) {
  bsl::SkipList<bsl::GatedPolicy> list(
      bsl::GatedPolicy(p, 1), seed, bsl::FloorConfig{},
      bsl::ReseedConfig{arm.reseed},
      bsl::CeilingConfig{arm.c1 > 0, arm.c1, arm.topup});
  const std::uint64_t y = n / 4, x = (3 * n) / 4;
  std::uint64_t search = 0, horiz = 0;
  auto add = [&](const bsl::SearchCost& c) {
    search += c.total();
    horiz += c.horizontal;
  };
  for (std::size_t i = 0; i < n; ++i) add(list.insert(i));
  for (std::size_t i = 0; i < n; ++i) add(list.access(i));
  for (std::uint64_t j = 0; j < m; ++j) add(list.access(y));
  BarrierResult r;
  r.h_y = list.height_of(y);
  r.h_x = list.height_of(x);
  r.h_max = list.level();
  const std::uint64_t rw0 = list.rebuild_work();
  const std::uint64_t search0 = search, horiz0 = horiz;
  for (std::uint64_t j = 0; j < m; ++j) add(list.access(x));
  const std::uint64_t xs = search - search0;
  r.x_block_horizontal = horiz - horiz0;
  r.horizontal = horiz;
  r.rebuild_work = list.rebuild_work();
  r.x_block_cost = static_cast<double>(xs + (r.rebuild_work - rw0));
  r.total_cost = static_cast<double>(search + r.rebuild_work);
  r.topup_work = list.topup_work();
  r.lambda_end = list.ceiling_level();
  r.q_end = arm.c1 > 0 ? list.count_at_height(r.lambda_end) : 0;
  r.h_max_end = list.level();
  r.top_end = list.count_at_height(r.h_max_end);
  return r;
}

void run_barrier_profile(const Config& cfg) {
  const std::string path = cfg.out + "/reseed_barrier" + cfg.suffix + ".csv";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    std::perror(path.c_str());
    return;
  }
  std::fprintf(f, "arm,n,m,run,F,total_cost,cost_over_F,cost_over_ent,"
                  "x_block_cost,x_block_over_m,H_y,H_x,H_max,rebuild_work,"
                  "horizontal_cost,horizontal_over_F,x_block_horizontal,"
                  "x_block_horizontal_over_m,topup_work,topup_over_F,"
                  "lambda_end,Q_end,H_max_end,top_count_end\n");
  const int R = cfg.barrier_runs;
  for (std::size_t n : cfg.barrier_ns) {
    std::uint64_t m = static_cast<std::uint64_t>(n) * n;
    const double cap = (cfg.barrier_fmax - 2.0 * static_cast<double>(n)) / 2.0;
    if (static_cast<double>(m) > cap) m = static_cast<std::uint64_t>(cap);
    const double F = 2.0 * static_cast<double>(n) + 2.0 * static_cast<double>(m);
    // Ent: n-2 items with f = 2 (insert + access), y and x with f = m + 2.
    std::vector<std::uint64_t> freqs(n, 2);
    freqs[n / 4] += m;
    freqs[(3 * n) / 4] += m;
    const double ent = bsl::entropy_benchmark(freqs, 1.0 / cfg.p);

    std::vector<BarrierArm> arms;
    for (const auto& a : kBarrierArms) {
      if (cfg.barrier_arms.empty() ||
          std::find(cfg.barrier_arms.begin(), cfg.barrier_arms.end(),
                    a.name) != cfg.barrier_arms.end()) {
        arms.push_back(a);
      }
    }
    const int A = static_cast<int>(arms.size());
    std::vector<BarrierResult> res(static_cast<std::size_t>(A * R));
    parallel_for(A * R, cfg.threads, [&](int job) {
      const int run = job / A;
      const BarrierArm& arm = arms[static_cast<std::size_t>(job % A)];
      // Same structure seed for every arm of one run.
      const std::uint64_t seed = cfg.seed * 17ULL +
                                 7777ULL * static_cast<std::uint64_t>(run) +
                                 static_cast<std::uint64_t>(n);
      res[job] = run_barrier(arm, n, m, cfg.p, seed);
    });

    std::printf("\n### barrier (K = 1, n = %zu, m = %llu, F = %.0f, R = %d, "
                "Ent/F = %.3f)\n\n",
                n, static_cast<unsigned long long>(m), F, R, ent / F);
    std::printf("| arm | mean cost/F | sd | q50 | q90 | q99 | max | "
                "x-block/m q50 | q99 | max | mean H_y - H_x | horizontal/F q50 | "
                "q90 | q99 | max |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
                "\n");
    std::vector<std::vector<double>> xblocks(static_cast<std::size_t>(A));
    for (int a = 0; a < A; ++a) {
      const char* arm = arms[static_cast<std::size_t>(a)].name;
      std::vector<double>& xb = xblocks[static_cast<std::size_t>(a)];
      std::vector<double> c, gap, hz;
      for (int run = 0; run < R; ++run) {
        const BarrierResult& r = res[static_cast<std::size_t>(A * run + a)];
        c.push_back(r.total_cost / F);
        xb.push_back(r.x_block_cost / static_cast<double>(m));
        gap.push_back(static_cast<double>(r.h_y) - static_cast<double>(r.h_x));
        hz.push_back(static_cast<double>(r.horizontal) / F);
        std::fprintf(f, "%s,%zu,%llu,%d,%.0f,%.0f,%.5f,%.5f,%.0f,%.5f,%u,%u,"
                        "%u,%llu,%llu,%.5f,%llu,%.5f,%llu,%.6f,%u,%zu,%u,%zu\n",
                     arm, n, static_cast<unsigned long long>(m), run, F,
                     r.total_cost, r.total_cost / F, r.total_cost / ent,
                     r.x_block_cost, r.x_block_cost / static_cast<double>(m),
                     r.h_y, r.h_x, r.h_max,
                     static_cast<unsigned long long>(r.rebuild_work),
                     static_cast<unsigned long long>(r.horizontal),
                     static_cast<double>(r.horizontal) / F,
                     static_cast<unsigned long long>(r.x_block_horizontal),
                     static_cast<double>(r.x_block_horizontal) /
                         static_cast<double>(m),
                     static_cast<unsigned long long>(r.topup_work),
                     static_cast<double>(r.topup_work) / F, r.lambda_end,
                     r.q_end, r.h_max_end, r.top_end);
      }
      std::printf("| %s | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f | "
                  "%.3f | %.3f | %.2f | %.3f | %.3f | %.3f | %.3f |\n",
                  arm, mean(c), sd(c), quantile(c, 0.5), quantile(c, 0.9),
                  quantile(c, 0.99), quantile(c, 1.0), quantile(xb, 0.5),
                  quantile(xb, 0.99), quantile(xb, 1.0), mean(gap),
                  quantile(hz, 0.5), quantile(hz, 0.9), quantile(hz, 0.99),
                  quantile(hz, 1.0));
    }
    std::printf("\nP(x-block/m >= t), n = %zu, R = %d\n\n", n, R);
    std::printf("| arm |");
    for (int t = 4; t <= 14; ++t) std::printf(" t=%d |", t);
    std::printf("\n|---|");
    for (int t = 4; t <= 14; ++t) std::printf("---|");
    std::printf("\n");
    for (int a = 0; a < A; ++a) {
      const auto& xb = xblocks[static_cast<std::size_t>(a)];
      std::printf("| %s |", arms[static_cast<std::size_t>(a)].name);
      for (int t = 4; t <= 14; ++t) {
        const auto hits = std::count_if(xb.begin(), xb.end(),
                                        [t](double v) { return v >= t; });
        std::printf(" %.4f |", static_cast<double>(hits) / R);
      }
      std::printf("\n");
    }
    std::printf("\nCeiling bookkeeping at the end of the run, n = %zu, "
                "R = %d (Q = items at height Lambda)\n\n", n, R);
    std::printf("| arm | mean topup work/F | max topup work | mean Lambda | "
                "mean Q | max Q | mean H_max | mean items at H_max |\n");
    std::printf("|---|---|---|---|---|---|---|---|\n");
    for (int a = 0; a < A; ++a) {
      if (arms[static_cast<std::size_t>(a)].c1 == 0) continue;
      std::vector<double> tw, lam, q, hm, top;
      for (int run = 0; run < R; ++run) {
        const BarrierResult& r = res[static_cast<std::size_t>(A * run + a)];
        tw.push_back(static_cast<double>(r.topup_work));
        lam.push_back(r.lambda_end);
        q.push_back(static_cast<double>(r.q_end));
        hm.push_back(r.h_max_end);
        top.push_back(static_cast<double>(r.top_end));
      }
      std::printf("| %s | %.6f | %.0f | %.2f | %.3f | %.0f | %.2f | %.3f |\n",
                  arms[static_cast<std::size_t>(a)].name, mean(tw) / F,
                  quantile(tw, 1.0), mean(lam), mean(q), quantile(q, 1.0),
                  mean(hm), mean(top));
    }
    std::fflush(stdout);
  }
  std::fclose(f);
}

// ---------------------------------------------------------------------------
// Profile e: fast drift in phases
// ---------------------------------------------------------------------------

// Insert n keys, then T phases; phase k accesses key (k mod n) exactly n
// times. Gated K = 3, persistent vs reseeded. Insertions are not costed.
void run_phases_profile(const Config& cfg) {
  const std::string path = cfg.out + "/reseed_phases" + cfg.suffix + ".csv";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    std::perror(path.c_str());
    return;
  }
  std::fprintf(f, "arm,n,T,run,F,access_cost,vertical,horizontal,"
                  "rebuild_work,total_cost,ent_global,ent_local,"
                  "cost_over_F,vertical_over_F,horizontal_over_F,"
                  "rebuild_over_F\n");
  std::printf("\n### phases (gated K = %u, phase k accesses key k mod n "
              "n times, R = %d)\n\n", cfg.K, cfg.runs);
  std::printf("| n | T | arm | mean cost/F | sd | search/F | vertical/F | "
              "horizontal/F | rebuild/F | Ent/F | local Ent/F |\n");
  std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
  for (std::size_t n : cfg.phase_ns) {
    for (std::size_t T : cfg.phase_Ts) {
      if (T > n) continue;
      Workload w;
      w.seq.reserve(T * n);
      for (std::size_t k = 0; k < T; ++k) {
        for (std::size_t j = 0; j < n; ++j) {
          w.seq.push_back(static_cast<std::uint32_t>(k % n));
        }
      }
      // epoch_entropies takes n = key universe; F = T n accesses.
      epoch_entropies(n, 1.0 / cfg.p, &w);
      const double F = static_cast<double>(w.seq.size());
      const int R = cfg.runs;
      struct PR {
        std::uint64_t search = 0, vert = 0, horiz = 0, rebuild = 0;
      };
      std::vector<PR> res(static_cast<std::size_t>(2 * R));
      parallel_for(2 * R, cfg.threads, [&](int job) {
        const int run = job / 2;
        const bool reseed = job % 2 == 1;
        bsl::SkipList<bsl::GatedPolicy> list(
            bsl::GatedPolicy(cfg.p, cfg.K),
            cfg.seed * 13ULL + 4099ULL * static_cast<std::uint64_t>(run) + n,
            bsl::FloorConfig{}, bsl::ReseedConfig{reseed});
        for (std::size_t i = 0; i < n; ++i) list.insert(i);
        PR r;
        for (auto k : w.seq) {
          const bsl::SearchCost c = list.access(k);
          r.search += c.total();
          r.vert += c.vertical;
          r.horiz += c.horizontal;
        }
        r.rebuild = list.rebuild_work();
        res[static_cast<std::size_t>(job)] = r;
      });
      for (int a = 0; a < 2; ++a) {
        const char* arm = a ? "gated+reseed" : "gated";
        std::vector<double> c, sr, v, h, rb;
        for (int run = 0; run < R; ++run) {
          const PR& r = res[static_cast<std::size_t>(2 * run + a)];
          const double total = static_cast<double>(r.search + r.rebuild);
          c.push_back(total / F);
          sr.push_back(static_cast<double>(r.search) / F);
          v.push_back(static_cast<double>(r.vert) / F);
          h.push_back(static_cast<double>(r.horiz) / F);
          rb.push_back(static_cast<double>(r.rebuild) / F);
          std::fprintf(f, "%s,%zu,%zu,%d,%.0f,%llu,%llu,%llu,%llu,%.0f,%.3f,"
                          "%.3f,%.5f,%.5f,%.5f,%.5f\n",
                       arm, n, T, run, F,
                       static_cast<unsigned long long>(r.search),
                       static_cast<unsigned long long>(r.vert),
                       static_cast<unsigned long long>(r.horiz),
                       static_cast<unsigned long long>(r.rebuild), total,
                       w.ent_global, w.ent_local, total / F, v.back(),
                       h.back(), rb.back());
        }
        std::printf("| %zu | %zu | %s | %.3f | %.3f | %.3f | %.3f | %.3f | "
                    "%.3f | %.3f | %.3f |\n",
                    n, T, arm, mean(c), sd(c), mean(sr), mean(v), mean(h),
                    mean(rb), w.ent_global / F, w.ent_local / F);
      }
      std::fflush(stdout);
    }
  }
  std::fclose(f);
}

// ---------------------------------------------------------------------------
// Profile f: shadow rebuild vs eager rebuild vs persistent
// ---------------------------------------------------------------------------

// Gated K = 3 on uniform, zipf1.0 and zipf1.5 (same workloads and structure
// seeds as run_profile, so the persistent and eager rows reproduce the gated
// rows there exactly) and on phases (n = shadow_phase_n, T = shadow_phase_T,
// same seeds as run_phases_profile). Insertions are not costed; cost =
// access search cost + rebuild work + shadow work. max_op_work is the
// structure's, over every operation including the insertions;
// max_access_work is the same maximum over the accesses only.
struct ShadowResult {
  std::uint64_t search = 0, horizontal = 0, rebuild = 0, shadow = 0;
  std::uint64_t max_op = 0, max_overhead = 0, max_access = 0;
  std::uint64_t boundaries = 0, swaps = 0, catchups = 0, fallbacks = 0;
  unsigned level = 0;
};

ShadowResult run_shadow_arm(int mode, const Config& cfg, std::size_t n,
                            const std::vector<std::uint32_t>& seq,
                            std::uint64_t seed) {
  bsl::SkipList<bsl::GatedPolicy> list(
      bsl::GatedPolicy(cfg.p, cfg.K), seed, bsl::FloorConfig{},
      bsl::ReseedConfig{mode >= 1, mode == 2});
  for (std::size_t i = 0; i < n; ++i) list.insert(i);
  ShadowResult r;
  for (auto k : seq) {
    const std::uint64_t over0 = list.rebuild_work() + list.shadow_work();
    const bsl::SearchCost c = list.access(k);
    const std::uint64_t op =
        c.total() + list.rebuild_work() + list.shadow_work() - over0;
    r.max_access = std::max(r.max_access, op);
    r.search += c.total();
    r.horizontal += c.horizontal;
  }
  r.rebuild = list.rebuild_work();
  r.shadow = list.shadow_work();
  r.max_op = list.max_op_work();
  r.max_overhead = list.max_op_overhead();
  r.boundaries = list.rebuilds();
  r.swaps = list.shadow_swaps();
  r.catchups = list.shadow_catchups();
  r.fallbacks = list.shadow_fallbacks();
  r.level = list.level();
  return r;
}

void run_shadow_profile(const Config& cfg) {
  const char* kModes[] = {"gated", "gated+reseed", "gated+shadow"};
  const std::string path = cfg.out + "/reseed_shadow" + cfg.suffix + ".csv";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    std::perror(path.c_str());
    return;
  }
  std::fprintf(f, "profile,arm,run,n,F,access_cost,rebuild_work,shadow_work,"
                  "total_cost,ent_global,cost_over_ent,cost_over_F,"
                  "search_over_F,overhead_over_F,horizontal_over_F,"
                  "max_op_work,max_op_overhead,max_access_work,boundaries,"
                  "swaps,catchups,fallbacks,final_level\n");
  std::printf("\n### shadow rebuild (gated K = %u, R = %d; cost/Ent for the "
              "stationary profiles, cost/F for phases)\n\n", cfg.K, cfg.runs);
  std::printf("| profile | arm | mean cost/Ent or cost/F | sd | search/F | "
              "(rebuild+shadow)/F | max_op_work mean | max_op_work max | "
              "max op overhead max | catch-ups | fallbacks |\n");
  std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");

  struct Prof {
    std::string name;
    int kind;  // 0 uniform, 1 zipf, 3 phases
    double s;
  };
  const std::vector<Prof> profs = {{"uniform", 0, 0.0},
                                   {"zipf1.0", 1, 1.0},
                                   {"zipf1.5", 1, 1.5},
                                   {"phases", 3, 0.0}};
  const int R = cfg.runs;
  for (const Prof& pr : profs) {
    const bool phases = pr.kind == 3;
    const std::size_t n = phases ? cfg.shadow_phase_n : cfg.keys;
    std::vector<Workload> work(static_cast<std::size_t>(phases ? 1 : R));
    if (phases) {
      const std::size_t T = cfg.shadow_phase_T;
      Workload& w = work[0];
      w.seq.reserve(T * n);
      for (std::size_t k = 0; k < T; ++k) {
        for (std::size_t j = 0; j < n; ++j) {
          w.seq.push_back(static_cast<std::uint32_t>(k % n));
        }
      }
      epoch_entropies(n, 1.0 / cfg.p, &w);
    } else {
      parallel_for(R, cfg.threads, [&](int run) {
        work[static_cast<std::size_t>(run)] =
            make_workload(pr.kind, pr.s, cfg, run);
      });
    }
    auto wl = [&](int run) -> const Workload& {
      return work[phases ? 0 : static_cast<std::size_t>(run)];
    };
    std::vector<ShadowResult> res(static_cast<std::size_t>(3 * R));
    parallel_for(3 * R, cfg.threads, [&](int job) {
      const int run = job / 3, mode = job % 3;
      const std::uint64_t seed =
          phases ? cfg.seed * 13ULL + 4099ULL * static_cast<std::uint64_t>(run) +
                       n
                 : cfg.seed * 31ULL + 104729ULL * static_cast<std::uint64_t>(run);
      res[static_cast<std::size_t>(job)] =
          run_shadow_arm(mode, cfg, n, wl(run).seq, seed);
    });
    const double F = static_cast<double>(wl(0).seq.size());
    for (int mode = 0; mode < 3; ++mode) {
      std::vector<double> c, sr, ov, mo, mov;
      std::uint64_t catchups = 0, fallbacks = 0;
      for (int run = 0; run < R; ++run) {
        const ShadowResult& r = res[static_cast<std::size_t>(3 * run + mode)];
        const double total =
            static_cast<double>(r.search + r.rebuild + r.shadow);
        const double ent = wl(run).ent_global;
        c.push_back(phases ? total / F : total / ent);
        sr.push_back(static_cast<double>(r.search) / F);
        ov.push_back(static_cast<double>(r.rebuild + r.shadow) / F);
        mo.push_back(static_cast<double>(r.max_op));
        mov.push_back(static_cast<double>(r.max_overhead));
        catchups += r.catchups;
        fallbacks += r.fallbacks;
        std::fprintf(f, "%s,%s,%d,%zu,%.0f,%llu,%llu,%llu,%.0f,%.3f,%.5f,"
                        "%.5f,%.5f,%.5f,%.5f,%llu,%llu,%llu,%llu,%llu,%llu,"
                        "%llu,%u\n",
                     pr.name.c_str(), kModes[mode], run, n, F,
                     static_cast<unsigned long long>(r.search),
                     static_cast<unsigned long long>(r.rebuild),
                     static_cast<unsigned long long>(r.shadow), total, ent,
                     total / ent, total / F, sr.back(), ov.back(),
                     static_cast<double>(r.horizontal) / F,
                     static_cast<unsigned long long>(r.max_op),
                     static_cast<unsigned long long>(r.max_overhead),
                     static_cast<unsigned long long>(r.max_access),
                     static_cast<unsigned long long>(r.boundaries),
                     static_cast<unsigned long long>(r.swaps),
                     static_cast<unsigned long long>(r.catchups),
                     static_cast<unsigned long long>(r.fallbacks), r.level);
      }
      std::printf("| %s | %s | %.4f | %.4f | %.3f | %.3f | %.1f | %.0f | %.0f "
                  "| %llu | %llu |\n",
                  pr.name.c_str(), kModes[mode], mean(c), sd(c), mean(sr),
                  mean(ov), mean(mo), quantile(mo, 1.0), quantile(mov, 1.0),
                  static_cast<unsigned long long>(catchups),
                  static_cast<unsigned long long>(fallbacks));
    }
    std::fflush(stdout);
  }
  std::fclose(f);
}

std::vector<std::size_t> parse_list(const std::string& v) {
  std::vector<std::size_t> out;
  std::size_t start = 0;
  while (start <= v.size()) {
    const std::size_t comma = v.find(',', start);
    const std::string tok = v.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!tok.empty()) out.push_back(std::stoull(tok));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

void parse_args(int argc, char** argv, Config* cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto eq = a.find('=');
    if (a.rfind("--", 0) != 0 || eq == std::string::npos) continue;
    const std::string k = a.substr(2, eq - 2);
    const std::string v = a.substr(eq + 1);
    if (k == "keys") cfg->keys = std::stoull(v);
    else if (k == "accesses_per_key") cfg->accesses_per_key = std::stoull(v);
    else if (k == "drift_period") cfg->drift_period_per_key = std::stoull(v);
    else if (k == "runs") cfg->runs = std::stoi(v);
    else if (k == "barrier_runs") cfg->barrier_runs = std::stoi(v);
    else if (k == "barrier_ns") cfg->barrier_ns = parse_list(v);
    else if (k == "barrier_fmax") cfg->barrier_fmax = std::stod(v);
    else if (k == "p") cfg->p = std::stod(v);
    else if (k == "K") cfg->K = static_cast<unsigned>(std::stoul(v));
    else if (k == "seed") cfg->seed = std::stoull(v);
    else if (k == "threads") cfg->threads = static_cast<unsigned>(std::stoul(v));
    else if (k == "out") cfg->out = v;
    else if (k == "only") cfg->only = v;
    else if (k == "suffix") cfg->suffix = v;
    else if (k == "phase_ns") cfg->phase_ns = parse_list(v);
    else if (k == "barrier_arms") {
      std::size_t start = 0;
      while (start <= v.size()) {
        const std::size_t comma = v.find(',', start);
        const std::string tok = v.substr(
            start,
            comma == std::string::npos ? std::string::npos : comma - start);
        if (!tok.empty()) cfg->barrier_arms.push_back(tok);
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
    else if (k == "phase_Ts") cfg->phase_Ts = parse_list(v);
    else if (k == "shadow_phase_n") cfg->shadow_phase_n = std::stoull(v);
    else if (k == "shadow_phase_T") cfg->shadow_phase_T = std::stoull(v);
  }
  if (cfg->threads == 0) {
    cfg->threads = std::max(1u, std::thread::hardware_concurrency());
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  parse_args(argc, argv, &cfg);
  std::fprintf(stderr, "keys=%zu F=%llu*n runs=%d barrier_runs=%d p=%.2f K=%u "
                       "threads=%u out=%s\n",
               cfg.keys, static_cast<unsigned long long>(cfg.accesses_per_key),
               cfg.runs, cfg.barrier_runs, cfg.p, cfg.K, cfg.threads,
               cfg.out.c_str());
  const bool all = cfg.only.empty();
  if (all || cfg.only == "stationary") {
    run_profile("uniform", 0, 0.0, cfg);
    run_profile("zipf1.0", 1, 1.0, cfg);
    run_profile("zipf1.5", 1, 1.5, cfg);
  }
  if (all || cfg.only == "drift") run_profile("drift", 2, 1.5, cfg);
  if (all || cfg.only == "barrier") run_barrier_profile(cfg);
  if (all || cfg.only == "phases") run_phases_profile(cfg);
  if (all || cfg.only == "shadow") run_shadow_profile(cfg);
  return 0;
}
