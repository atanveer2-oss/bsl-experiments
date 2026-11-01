// Correctness checks for the structures, run via `ctest`. These are not
// benchmarks: they only assert that the skip list is a correct dictionary and
// that the gated policy has the distributional properties the analysis assumes.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "bsl/skiplist.hpp"
#include "bsl/zipf.hpp"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

// The dictionary must hold everything inserted and nothing else, under both
// policies, including after accesses have moved heights around.
template <typename List>
void dictionary_invariants(List& list, const char* name) {
  constexpr std::uint64_t kN = 2000;
  for (std::uint64_t i = 0; i < kN; ++i) list.insert(i * 2);
  check(list.size() == kN, "size after insert");

  for (std::uint64_t i = 0; i < kN; ++i) {
    check(list.contains(i * 2), "inserted key present");
    check(!list.contains(i * 2 + 1), "absent key reported absent");
  }

  // Hammer a hot set so the gated policy actually promotes.
  for (int round = 0; round < 50; ++round) {
    for (std::uint64_t i = 0; i < 20; ++i) list.access(i * 2);
  }
  for (std::uint64_t i = 0; i < kN; ++i) {
    check(list.contains(i * 2), "key still present after promotions");
  }
  check(list.level() <= bsl::kMaxLevel, "level within cap");
  std::printf("  %s: dictionary invariants checked\n", name);
}

// Heights never decrease, and a hot key ends up at least as tall as a cold one.
void gated_promotion_is_monotone() {
  bsl::GatedSkipList list(bsl::GatedPolicy(0.5, 3), /*seed=*/42);
  for (std::uint64_t i = 0; i < 500; ++i) list.insert(i);

  unsigned prev = list.height_of(7);
  for (int i = 0; i < 2000; ++i) {
    list.access(7);
    const unsigned now = list.height_of(7);
    check(now >= prev, "gated height never decreases");
    prev = now;
  }
  std::printf("  gated: height monotone, hot key reached height %u\n", prev);
  check(prev > 1, "hot key was promoted at all");
}

// The classic policy must leave heights untouched no matter how often a key is
// accessed; this is the control arm of every comparison.
void classic_policy_is_static() {
  bsl::ClassicSkipList list(bsl::ClassicPolicy(0.5), /*seed=*/7);
  for (std::uint64_t i = 0; i < 500; ++i) list.insert(i);
  const unsigned h0 = list.height_of(7);
  for (int i = 0; i < 5000; ++i) list.access(7);
  check(list.height_of(7) == h0, "classic height unchanged by access");
  std::printf("  classic: height static under access (h=%u)\n", h0);
}

// Geom_0(1-p) must satisfy P[G >= g] = p^g; the height analysis leans on it.
void geometric_matches_tail() {
  bsl::Rng rng(123);
  constexpr int kTrials = 200000;
  const double p = 0.5;
  int at_least_3 = 0;
  for (int i = 0; i < kTrials; ++i) {
    if (rng.geometric0(p) >= 3) ++at_least_3;
  }
  const double observed = static_cast<double>(at_least_3) / kTrials;
  const double expected = p * p * p;  // 0.125
  check(std::abs(observed - expected) < 0.005, "geometric tail P[G>=3] = p^3");
  std::printf("  rng: P[G>=3] observed %.4f vs expected %.4f\n", observed,
              expected);
}

// The weight floor must (a) keep the dictionary correct while raising items
// mid-run, (b) never let an item sit below the floor, and (c) actually fire
// once F >> n.
void weight_floor_holds() {
  bsl::GatedSkipList list(bsl::GatedPolicy(0.5, 1), /*seed=*/9,
                          bsl::FloorConfig{true, 0});
  constexpr std::uint64_t kN = 1000;
  for (std::uint64_t i = 0; i < kN; ++i) list.insert(i);

  bsl::ZipfGenerator zipf(kN, 1.5, 11);
  for (int t = 0; t < 400000; ++t) list.access(zipf.sample());

  check(list.raises() > 0, "floor raised at least once when F >> n");
  check(list.min_height() >= list.floor_level(),
        "no item below the floor after raises");
  for (std::uint64_t i = 0; i < kN; ++i) {
    check(list.contains(i), "key present after floor raises");
    check(!list.contains(i + kN), "absent key absent after floor raises");
  }
  std::printf("  floor: %llu raises, floor=%u, depth=%u (n=%llu)\n",
              static_cast<unsigned long long>(list.raises()),
              list.floor_level(), list.level() - list.min_height(),
              static_cast<unsigned long long>(kN));
}

// The counter gates must keep the height invariant H >= rank estimate, which
// the old paper's analysis leans on.
void counter_gates_keep_height_invariant() {
  bsl::ExactGateSkipList exact(bsl::ExactGatePolicy(0.5), 21);
  bsl::MorrisGateSkipList morris(bsl::MorrisGatePolicy(0.5), 22);
  for (std::uint64_t i = 0; i < 200; ++i) {
    exact.insert(i);
    morris.insert(i);
  }
  for (int t = 0; t < 20000; ++t) {
    exact.access(3);
    morris.access(3);
  }
  // Exact: after 20001 accesses, rank = floor(log2 20001) = 14.
  check(exact.height_of(3) >= 14, "exact gate: height >= rank");
  // Morris: E[R] ~ log2(20001) ~ 14; height must be >= R, so at least a
  // loose lower bound that a non-promoting bug would violate.
  check(morris.height_of(3) >= 8, "morris gate: hot key promoted");
  std::printf("  counter gates: exact h=%u, morris h=%u after 20k accesses\n",
              exact.height_of(3), morris.height_of(3));
}

// Regression test for the defect an adversarial audit found in the paper's
// original weight floor (combined/main.tex Remark 8.5). If the floor is a
// running maximum, then after a hot phase (n=1, so the floor climbs to
// log_b t) followed by a burst of insertions (t/n_t collapses, but a running
// maximum cannot fall), every new item enters at a floor far above
// log_b(t/n_t) and the maximum height roughly doubles. Theorem D's
// max-height bound allows H_max >= K + log_b t + a only with probability
// C_p p^a; under the running max this was measured at 98% for a = 10.
void floor_must_not_be_a_running_maximum() {
  const int kLogT = 14;
  const std::size_t T = std::size_t{1} << kLogT;
  bsl::SkipList<bsl::GatedPolicy> list(bsl::GatedPolicy(0.5, 1), 9,
                                       bsl::FloorConfig{true, 1});
  list.insert(0);
  for (std::size_t i = 1; i < T; ++i) list.access(0);      // hot phase, n = 1
  const unsigned floor_hot = list.floor_level();
  for (std::size_t i = 1; i <= T; ++i) list.insert(i);     // insertion burst
  const unsigned floor_after = list.floor_level();
  const double t = static_cast<double>(2 * T);
  const double budget = 1.0 + std::log2(t) + 10.0;         // K + log_b t + 10

  check(floor_after < floor_hot,
        "floor falls when t/n_t collapses (it is not a running maximum)");
  check(list.level() <= budget,
        "max height stays within K + log_b t + 10 after an insertion burst");
  std::printf("  floor: %u during hot phase -> %u after burst; "
              "H_max=%u, budget=%.1f\n",
              floor_hot, floor_after, list.level(), budget);
}

// Epochal reseeding, hand-checked schedule. Inserting 5 items: d goes
// 1 (N=1), 2 (N=2, doubles), 3 (N=4, doubles), 4, 5 (N=8, doubles), so the
// insertions open epochs 1..4 and rebuild three times (over 1, 2 and 4
// items). The fourth epoch opened with insertion 5, so it holds that charge
// plus accesses 1..7 (8 = N charges); access 8 opens epoch 5, and access 16
// opens epoch 6.
void reseed_schedule_hand_checked() {
  bsl::GatedSkipList list(bsl::GatedPolicy(0.5, 3), 31, bsl::FloorConfig{},
                          bsl::ReseedConfig{true});
  check(list.epochs() == 0, "reseed: no epoch before the first charge");
  list.insert(10);
  check(list.epochs() == 1 && list.rebuilds() == 0,
        "reseed: first insertion opens epoch 1 without a rebuild");
  list.insert(20);
  check(list.epochs() == 2 && list.schedule().capacity() == 2,
        "reseed: insertion 2 doubles N to 2 and opens epoch 2");
  list.insert(30);
  check(list.epochs() == 3 && list.schedule().capacity() == 4,
        "reseed: insertion 3 doubles N to 4 and opens epoch 3");
  list.insert(40);
  check(list.epochs() == 3, "reseed: insertion 4 stays in epoch 3");
  list.insert(50);
  check(list.epochs() == 4 && list.schedule().capacity() == 8 &&
            list.rebuilds() == 3,
        "reseed: insertion 5 doubles N to 8 and opens epoch 4");
  // Rebuild work so far: 1, 2 and 4 nodes, each 1 + its fresh height >= 2.
  check(list.rebuild_work() >= 2 * (1 + 2 + 4),
        "reseed: rebuild work counts nodes and levels");
  list.insert(30);  // already present: one charge, no doubling
  check(list.epochs() == 4 && list.schedule().charges_in_epoch() == 2,
        "reseed: re-inserting a present key is a charge but not a doubling");

  bsl::GatedSkipList fresh(bsl::GatedPolicy(0.5, 3), 32, bsl::FloorConfig{},
                           bsl::ReseedConfig{true});
  for (std::uint64_t k = 1; k <= 5; ++k) fresh.insert(k);
  bool ok = true;
  for (int a = 1; a <= 7; ++a) {
    fresh.access(1 + a % 5);
    ok = ok && fresh.epochs() == 4;
  }
  check(ok, "reseed: accesses 1..7 stay in epoch 4");
  fresh.access(3);
  check(fresh.epochs() == 5 && fresh.rebuilds() == 4,
        "reseed: access 8 (the 9th charge of epoch 4) opens epoch 5");
  for (int a = 9; a <= 15; ++a) fresh.access(1 + a % 5);
  check(fresh.epochs() == 5, "reseed: accesses 9..15 stay in epoch 5");
  fresh.access(2);
  check(fresh.epochs() == 6, "reseed: access 16 opens epoch 6");
  check(fresh.well_formed(), "reseed: structure well formed after rebuilds");

  // The schedule on its own agrees with the structure's.
  bsl::EpochSchedule s;
  int rebuilds = 0;
  for (int i = 0; i < 5; ++i) rebuilds += s.charge(true);
  for (int a = 0; a < 16; ++a) rebuilds += s.charge(false);
  check(rebuilds == 5 && s.epochs() == 6,
        "reseed: standalone schedule matches the structure");
  std::printf("  reseed: schedule hand-checked (epochs %llu, rebuilds %llu, "
              "work %llu)\n",
              static_cast<unsigned long long>(fresh.epochs()),
              static_cast<unsigned long long>(fresh.rebuilds()),
              static_cast<unsigned long long>(fresh.rebuild_work()));
}

// After many rebuilds every key is still found, absent keys are absent, the
// levels are consistent with the level-0 order, the counter policies' state
// is epoch-local, and a hot key's height has been reset (it is bounded by
// what one epoch can build, not by the whole history).
template <typename Policy>
void reseed_keeps_dictionary(Policy policy, const char* name) {
  constexpr std::uint64_t kN = 1000;
  bsl::SkipList<Policy> list(policy, 77, bsl::FloorConfig{},
                             bsl::ReseedConfig{true});
  for (std::uint64_t i = 0; i < kN; ++i) list.insert(i * 2);
  check(list.well_formed(), "reseed: well formed after insertion rebuilds");
  bsl::ZipfGenerator zipf(kN, 1.5, 5);
  for (int t = 0; t < 200000; ++t) list.access(2 * zipf.sample());
  check(list.rebuilds() >= 150, "reseed: about F/N rebuilds happened");
  check(list.well_formed(), "reseed: well formed after access rebuilds");
  for (std::uint64_t i = 0; i < kN; ++i) {
    check(list.contains(i * 2), "reseed: key present after rebuilds");
    check(!list.contains(i * 2 + 1), "reseed: absent key absent");
  }
  // Epoch capacity is 1024 charges, so no height can reflect more than ~1024
  // accesses: log2(1024) = 10 plus initial and geometric slack.
  check(list.level() <= 40, "reseed: heights bounded by one epoch's history");
  std::printf("  reseed %s: %llu rebuilds, dictionary intact, level %u\n",
              name, static_cast<unsigned long long>(list.rebuilds()),
              list.level());
}

// Clocked ceiling: after every charge no height exceeds Lambda =
// floor(log_b ops) + c1, and the dictionary stays correct.
void ceiling_holds() {
  constexpr std::uint64_t kN = 1000;
  bsl::GatedSkipList list(bsl::GatedPolicy(0.5, 3), 41, bsl::FloorConfig{},
                          bsl::ReseedConfig{}, bsl::CeilingConfig{true, 2});
  bool ok = true;
  for (std::uint64_t i = 0; i < kN; ++i) {
    list.insert(i);
    ok = ok && list.level() <= list.ceiling_level();
  }
  bsl::ZipfGenerator zipf(kN, 1.5, 13);
  for (int t = 0; t < 100000; ++t) {
    list.access(zipf.sample());
    ok = ok && list.level() <= list.ceiling_level();
  }
  check(ok, "ceiling: no height above Lambda after any charge");
  // ops = 101000, log2 = 16.62, so Lambda = 16 + 2 = 18.
  check(list.ceiling_level() == 18, "ceiling: Lambda = floor(log2 ops) + c1");
  check(list.well_formed(), "ceiling: structure well formed");
  for (std::uint64_t i = 0; i < kN; ++i) {
    check(list.contains(i), "ceiling: key present");
    check(!list.contains(i + kN), "ceiling: absent key absent");
  }
  std::printf("  ceiling: Lambda=%u, level=%u, hot key h=%u\n",
              list.ceiling_level(), list.level(), list.height_of(0));
}

// Ceiling with top-up: the same invariants hold and the top-up sweep fires.
void ceiling_topup_holds() {
  constexpr std::uint64_t kN = 1000;
  bsl::GatedSkipList list(bsl::GatedPolicy(0.5, 1), 43, bsl::FloorConfig{},
                          bsl::ReseedConfig{},
                          bsl::CeilingConfig{true, 2, true});
  bool ok = true;
  for (std::uint64_t i = 0; i < kN; ++i) {
    list.insert(i);
    ok = ok && list.level() <= list.ceiling_level();
  }
  bsl::ZipfGenerator zipf(kN, 1.5, 17);
  for (int t = 0; t < 100000; ++t) {
    list.access(zipf.sample());
    ok = ok && list.level() <= list.ceiling_level();
  }
  check(ok, "ceiling+topup: no height above Lambda after any charge");
  check(list.topup_work() > 0, "ceiling+topup: top-up swept at least once");
  check(list.well_formed(), "ceiling+topup: structure well formed");
  for (std::uint64_t i = 0; i < kN; ++i) {
    check(list.contains(i), "ceiling+topup: key present");
    check(!list.contains(i + kN), "ceiling+topup: absent key absent");
  }
  std::printf("  ceiling+topup: Lambda=%u, level=%u, topup work=%llu, "
              "Q=%zu\n",
              list.ceiling_level(), list.level(),
              static_cast<unsigned long long>(list.topup_work()),
              list.count_at_height(list.ceiling_level()));
}

// Shadow (double-buffered) reseeding on a fixed set: after every epoch
// boundary the active list is well formed and holds exactly the keys, every
// boundary after the insertion phase is an O(1) swap of a complete shadow,
// and no single operation does Theta(n) work, while the eager arm's worst
// operation is a full rebuild.
//
// Bound. The shadow copies ceil(n/N) + 1 keys per charge, each costing
// 1 + an insertion-law height, so its per-operation work is
// (ceil(n/N) + 1)(1 + H) for H ~ J + Geom: O(1 + n/N) in expectation and
// O((1 + n/N) log n) as a maximum over the run. The search part is
// O(log n) per operation but also a maximum over 200k searches. The
// threshold below is 8 log2 n + 4 (n/N + 1) log2 n, about 159 at n = 1000
// and an order of magnitude below n; the eager arm's worst operation is
// >= 2n. (Over 30 seeds the shadow arm's max_op_work had mean 50 and max 56,
// so the tighter 50 + 4 n/N would fail on about one seed in ten.)
void shadow_reseed_bounded_work() {
  constexpr std::uint64_t kN = 1000;
  bsl::GatedSkipList eager(bsl::GatedPolicy(0.5, 3), 91, bsl::FloorConfig{},
                           bsl::ReseedConfig{true});
  bsl::GatedSkipList shadow(bsl::GatedPolicy(0.5, 3), 91, bsl::FloorConfig{},
                            bsl::ReseedConfig{true, true});
  for (std::uint64_t i = 0; i < kN; ++i) {
    eager.insert(i * 2);
    shadow.insert(i * 2);
  }
  const auto catchups0 = shadow.shadow_catchups();
  const auto fallbacks0 = shadow.shadow_fallbacks();
  const auto swaps0 = shadow.shadow_swaps();
  const auto rebuild_work0 = shadow.rebuild_work();
  check(shadow.well_formed() && shadow.size() == kN,
        "shadow: well formed after the insertion phase");

  auto exact_keys = [&](bsl::GatedSkipList& list) {
    bool ok = list.size() == kN && list.well_formed();
    for (std::uint64_t i = 0; i < kN && ok; ++i) {
      ok = list.contains(i * 2) && !list.contains(i * 2 + 1);
    }
    return ok;
  };

  bsl::ZipfGenerator zipf(kN, 1.5, 19);
  bool ok_shadow = true, ok_eager = true;
  std::uint64_t boundaries = 0;
  std::uint64_t seen_s = shadow.rebuilds(), seen_e = eager.rebuilds();
  for (int t = 0; t < 200000; ++t) {
    const std::uint64_t k = 2 * zipf.sample();
    shadow.access(k);
    eager.access(k);
    if (shadow.rebuilds() != seen_s) {
      seen_s = shadow.rebuilds();
      ++boundaries;
      ok_shadow = ok_shadow && exact_keys(shadow);
    }
    if (eager.rebuilds() != seen_e) {
      seen_e = eager.rebuilds();
      ok_eager = ok_eager && exact_keys(eager);
    }
  }
  check(ok_shadow, "shadow: active list exact and well formed at every "
                   "boundary");
  check(ok_eager, "eager: active list exact and well formed at every "
                  "boundary");
  check(boundaries >= 150, "shadow: about F/N boundaries happened");
  check(shadow.rebuilds() == eager.rebuilds(),
        "shadow: same epoch schedule as the eager arm");
  check(shadow.shadow_swaps() - swaps0 == boundaries,
        "shadow: every access-phase boundary is a swap");
  check(shadow.shadow_catchups() == catchups0 &&
            shadow.shadow_fallbacks() == fallbacks0,
        "shadow: fixed set needs no catch-up and no eager fallback");
  check(shadow.rebuild_work() == rebuild_work0 && shadow.shadow_work() > 0,
        "shadow: no eager rebuild in the access phase; shadow work counted");
  check(shadow.level() <= 40, "shadow: heights bounded by one epoch");

  const double n = static_cast<double>(kN);
  const double N = static_cast<double>(shadow.schedule().capacity());
  const double lg = std::log2(n);
  const double bound = 8.0 * lg + 4.0 * (n / N + 1.0) * lg;
  check(static_cast<double>(shadow.max_op_work()) < bound,
        "shadow: max_op_work is O(log n + (n/N) log n)");
  check(eager.max_op_work() >= kN, "eager: max_op_work is at least n");
  std::printf("  shadow reseed: %llu boundaries, max_op_work shadow %llu "
              "(overhead %llu, bound %.0f) vs eager %llu (overhead %llu); "
              "work/F shadow %.3f vs eager rebuild %.3f\n",
              static_cast<unsigned long long>(boundaries),
              static_cast<unsigned long long>(shadow.max_op_work()),
              static_cast<unsigned long long>(shadow.max_op_overhead()),
              bound, static_cast<unsigned long long>(eager.max_op_work()),
              static_cast<unsigned long long>(eager.max_op_overhead()),
              static_cast<double>(shadow.shadow_work() +
                                  shadow.rebuild_work()) / 200000.0,
              static_cast<double>(eager.rebuild_work()) / 200000.0);
}

// Shadow reseeding with a growing set in random key order: new keys land
// behind the copy cursor, so some boundaries fall back to the eager rebuild.
// The dictionary must stay exact throughout.
void shadow_reseed_dynamic() {
  constexpr std::uint64_t kN = 3000;
  bsl::GatedSkipList list(bsl::GatedPolicy(0.5, 3), 93, bsl::FloorConfig{},
                          bsl::ReseedConfig{true, true});
  std::vector<std::uint64_t> keys(kN);
  for (std::uint64_t i = 0; i < kN; ++i) keys[i] = i * 2;
  bsl::Rng rng(95);
  for (std::size_t i = keys.size(); i > 1; --i) {
    std::swap(keys[i - 1], keys[rng.next() % i]);
  }
  bool ok = true;
  for (std::uint64_t i = 0; i < kN; ++i) {
    list.insert(keys[i]);
    // Interleave accesses so epochs also end by charge count mid-growth.
    for (int a = 0; a < 3; ++a) list.access(keys[rng.next() % (i + 1)]);
    if (i % 97 == 0) ok = ok && list.well_formed();
  }
  check(ok, "shadow dynamic: well formed during growth");
  check(list.size() == kN && list.well_formed(),
        "shadow dynamic: well formed after growth");
  bool exact = true;
  for (std::uint64_t i = 0; i < kN; ++i) {
    exact = exact && list.contains(i * 2) && !list.contains(i * 2 + 1);
  }
  check(exact, "shadow dynamic: holds exactly the inserted keys");
  check(list.shadow_fallbacks() > 0,
        "shadow dynamic: out-of-order insertions exercise the fallback");
  check(list.shadow_swaps() + list.shadow_fallbacks() == list.rebuilds(),
        "shadow dynamic: every boundary is a swap or a fallback");
  std::printf("  shadow reseed dynamic, random order: %llu boundaries = "
              "%llu swaps (%llu with catch-up) + %llu eager fallbacks\n",
              static_cast<unsigned long long>(list.rebuilds()),
              static_cast<unsigned long long>(list.shadow_swaps()),
              static_cast<unsigned long long>(list.shadow_catchups()),
              static_cast<unsigned long long>(list.shadow_fallbacks()));

  // Increasing key order: every new key lands ahead of the cursor, so there
  // is never a fallback. A catch-up needs an epoch that opens with d close
  // to N and is cut short by a doubling: grow to 1000 keys (N = 1024),
  // access until a boundary by charge count, then insert 25 more keys; the
  // 25th doubles N two dozen charges into the epoch, when the shadow holds
  // about 50 of 1024 keys, and the boundary operation finishes it eagerly.
  bsl::GatedSkipList seq(bsl::GatedPolicy(0.5, 3), 97, bsl::FloorConfig{},
                         bsl::ReseedConfig{true, true});
  constexpr std::uint64_t kM = 1000;
  for (std::uint64_t i = 0; i < kM; ++i) seq.insert(i * 2);
  const std::uint64_t r0 = seq.rebuilds();
  while (seq.rebuilds() == r0) seq.access(2 * (rng.next() % kM));
  const std::uint64_t catchups0 = seq.shadow_catchups();
  for (std::uint64_t i = kM; i < kM + 25; ++i) seq.insert(i * 2);
  bool seq_ok = seq.size() == kM + 25 && seq.well_formed() &&
                seq.schedule().capacity() == 2048;
  for (std::uint64_t i = 0; i < kM + 25; ++i) {
    seq_ok = seq_ok && seq.contains(i * 2) && !seq.contains(i * 2 + 1);
  }
  check(seq_ok, "shadow dynamic, increasing order: exact and well formed");
  check(seq.shadow_fallbacks() == 0,
        "shadow dynamic, increasing order: no fallback");
  check(seq.shadow_catchups() == catchups0 + 1,
        "shadow dynamic, increasing order: the doubling forces a catch-up");
  check(seq.shadow_swaps() == seq.rebuilds(),
        "shadow dynamic, increasing order: every boundary is a swap");
  std::printf("  shadow reseed dynamic, increasing order: %llu swaps, %llu "
              "with catch-up (max op overhead %llu)\n",
              static_cast<unsigned long long>(seq.shadow_swaps()),
              static_cast<unsigned long long>(seq.shadow_catchups()),
              static_cast<unsigned long long>(seq.max_op_overhead()));
}

// lem:pointercount: the compressed representation (def:compression) stores
// at most 2n pointers whatever the heights. Checked after heavy promotion,
// under the self-referential rule and the paper's exact estimator, against
// the brute-force definition: per node, the distinct non-null successors.
template <typename Policy>
void compressed_pointer_bound(Policy policy, const char* name) {
  constexpr std::uint64_t kN = 3000;
  bsl::SkipList<Policy> list(policy, /*seed=*/77);
  for (std::uint64_t i = 0; i < kN; ++i) list.insert(i);
  bsl::ZipfGenerator zipf(kN, 1.5, 78);
  for (int i = 0; i < 400000; ++i) list.access(zipf.sample());
  for (int i = 0; i < 20000; ++i) list.access(5);  // one very tall tower

  const std::uint64_t with_head = list.compressed_pointer_count(true);
  const std::uint64_t items = list.compressed_pointer_count(false);
  std::uint64_t sum_h = 0;
  list.for_each_item([&](std::uint64_t, unsigned h, const auto&) {
    sum_h += h;
  });
  check(list.pointer_count() == sum_h, "pointer_count is the sum of heights");
  check(items <= with_head, "compressed: head adds pointers, never removes");
  check(items <= 2 * kN, "lem:pointercount: items-only count <= 2n");
  check(with_head <= 2 * kN + list.level(),
        "lem:pointercount: head-inclusive count <= 2n + hmax");
  check(with_head < list.pointer_count(),
        "compression removes pointers once heights have grown");
  check(list.well_formed(), "compressed count: structure well formed");
  std::printf("  %s: compressed pointers %llu (head incl.), %llu (items), "
              "uncompressed %llu, 2n = %llu, hmax = %u\n",
              name, static_cast<unsigned long long>(with_head),
              static_cast<unsigned long long>(items),
              static_cast<unsigned long long>(list.pointer_count()),
              static_cast<unsigned long long>(2 * kN), list.level());
}

// A demoting rule that always leaks, so the demotions can be checked by hand.
struct AlwaysLeakPolicy {
  struct State {};
  double p = 0.5;
  unsigned L = 3;
  unsigned initial_height(State&, bsl::Rng&) const { return 1u; }
  unsigned on_access(State&, unsigned h, bsl::Rng&) const {
    return h > L ? h : L;
  }
  bool on_pass(State&, unsigned, bsl::Rng&) const { return true; }
};

// Keys 0..7 at height 1, top L = 3. The passed items are the ones stepped
// over at levels >= 2, excluding the descent point and the target.
void demotion_hand_checked() {
  bsl::SkipList<AlwaysLeakPolicy> list(AlwaysLeakPolicy{}, 1);
  for (std::uint64_t k = 0; k < 8; ++k) list.insert(k);
  list.access(0);  // nothing passed; 0 -> 3
  list.access(7);  // level 3 steps onto 0 only (the descent point); 7 -> 3
  list.access(3);  // likewise; 3 -> 3
  check(list.height_of(0) == 3 && list.height_of(3) == 3 &&
            list.height_of(7) == 3 && list.demotions() == 0,
        "demotion: descent points and level-1 passes are never demoted");
  list.access(5);  // level 3: head, 0, 3 (descent point); 0 is passed
  check(list.height_of(0) == 2 && list.height_of(3) == 3 &&
            list.height_of(5) == 3 && list.height_of(4) == 1 &&
            list.demotions() == 1,
        "demotion: the passed item loses exactly one level");
  const bsl::SearchCost c = list.access(6);  // level 3: 3 passed, 5 descent
  check(c.vertical == 3 && c.horizontal == 2,
        "demotion: the scan is costed before it is disturbed");
  check(list.height_of(3) == 2 && list.height_of(5) == 3 &&
            list.height_of(6) == 3 && list.height_of(0) == 2 &&
            list.demotions() == 2,
        "demotion: second access demotes only its own passed item");
  check(list.well_formed(), "demotion: structure well formed by hand");
  std::printf("  demotion: hand-checked sequence of five accesses\n");
}

// Move-to-top with leak: every hit ends at L, nothing exceeds L, the
// structure stays well formed, and the leak actually fires.
void move_to_top_leak_holds() {
  constexpr std::uint64_t kN = 1000;
  const unsigned L = bsl::MoveToTopLeakPolicy::legacy_top(kN, 0.5);
  check(L == 12, "move-to-top: legacy L = ceil(log2 1000) + 2 = 12");
  check(bsl::MoveToTopLeakPolicy::legacy_top(1024, 0.25) == 7,
        "move-to-top: legacy L = log4 1024 + 2 = 7 (exact power)");
  check(bsl::MoveToTopLeakPolicy::default_top(1000, 0.5) == 9,
        "move-to-top: default L = ceil(log3 1000) + 2 = 9");
  check(bsl::MoveToTopLeakPolicy::default_top(625, 0.25) == 6,
        "move-to-top: default L = log5 625 + 2 = 6 (exact power)");
  bsl::MoveToTopLeakSkipList list(bsl::MoveToTopLeakPolicy(0.5, L), 11);
  for (std::uint64_t k = 0; k < kN; ++k) list.insert(k);
  bsl::Rng rng(5);
  bool ok = true, formed = true;
  for (int t = 0; t < 20000; ++t) {
    const std::uint64_t k = rng.next() % kN;
    bool found = false;
    list.access(k, &found);
    ok = ok && found && list.height_of(k) == L && list.level() == L;
    if (t % 2000 == 0) formed = formed && list.well_formed();
  }
  unsigned hmax = 0;
  list.for_each_item([&](std::uint64_t, unsigned h, const auto&) {
    hmax = std::max(hmax, h);
  });
  check(ok, "move-to-top: every hit is promoted to L");
  check(formed && list.well_formed(), "move-to-top: well formed throughout");
  check(hmax == L, "move-to-top: no height above L");
  check(list.demotions() > 0, "move-to-top: the leak demotes");
  for (std::uint64_t k = 0; k < kN; ++k) {
    ok = ok && list.contains(k) && !list.contains(k + kN);
  }
  check(ok, "move-to-top: dictionary intact after demotions");
  std::printf("  move-to-top leak: L = %u, %llu demotions in 20000 accesses\n",
              L, static_cast<unsigned long long>(list.demotions()));
}

// GatedLeak with q = 0 is GatedPolicy, coin for coin; with q > 0 it stays a
// well-formed dictionary.
void gated_leak_reduces_to_gated() {
  bsl::GatedSkipList a(bsl::GatedPolicy(0.5, 1), 99);
  bsl::GatedLeakSkipList b(bsl::GatedLeakPolicy(0.5, 1, 0.0), 99);
  bsl::GatedLeakSkipList c(bsl::GatedLeakPolicy(0.5, 1, 0.05), 99);
  for (std::uint64_t k = 0; k < 500; ++k) {
    a.insert(k);
    b.insert(k);
    c.insert(k);
  }
  bsl::ZipfGenerator zipf(500, 1.0, 3);
  bool same = true;
  for (int t = 0; t < 30000; ++t) {
    const std::uint64_t k = zipf.sample();
    same = same && a.access(k).total() == b.access(k).total();
    c.access(k);
  }
  for (std::uint64_t k = 0; k < 500; ++k) {
    same = same && a.height_of(k) == b.height_of(k);
  }
  check(same, "gated leak q = 0: identical to the gated rule");
  check(b.demotions() == 0, "gated leak q = 0: no demotions");
  check(c.well_formed() && c.demotions() > 0,
        "gated leak q > 0: demotes and stays well formed");
  std::printf("  gated leak: q = 0 matches gated; q = 0.05 made %llu "
              "demotions\n", static_cast<unsigned long long>(c.demotions()));
}

// The same keys without the descent-point exemption: the descent point is
// demoted too, and the promotion then splices behind its predecessor.
struct AlwaysLeakAllPolicy : AlwaysLeakPolicy {
  bool exempt_descent = false;
};

void demotion_without_exemption_hand_checked() {
  bsl::SkipList<AlwaysLeakAllPolicy> list(AlwaysLeakAllPolicy{}, 1);
  for (std::uint64_t k = 0; k < 8; ++k) list.insert(k);
  list.access(0);  // nothing passed; 0 -> 3
  // Level 3: head steps onto 0, the descent point, which is now demoted; 7
  // is spliced at level 3 behind the head and at level 2 behind 0.
  const bsl::SearchCost c = list.access(7);
  check(c.vertical == 3 && c.horizontal == 7,
        "demotion (no exemption): scan costed before it is disturbed");
  check(list.height_of(0) == 2 && list.height_of(7) == 3 &&
            list.demotions() == 1,
        "demotion (no exemption): the descent point loses one level");
  check(list.well_formed(), "demotion (no exemption): well formed");
  list.access(3);  // level 3: head, 7 > 3, no step; level 2: 0 demoted
  check(list.height_of(0) == 1 && list.height_of(3) == 3 &&
            list.demotions() == 2 && list.well_formed(),
        "demotion (no exemption): second descent point demoted, well formed");
  std::printf("  demotion without exemption: hand-checked three accesses\n");
}

// Both leak variants with geometric insertion: well formed, every hit at L,
// nothing above L, and the insertion law has level ratio 1 + 1/q.
void move_to_top_variants_hold() {
  constexpr std::uint64_t kN = 3000;
  for (bool exempt : {true, false}) {
    const unsigned L = bsl::MoveToTopLeakPolicy::default_top(kN, 0.5);
    bsl::MoveToTopLeakSkipList list(
        bsl::MoveToTopLeakPolicy(0.5, L, exempt,
                                 bsl::LeakInsert::kGeometric), 21);
    for (std::uint64_t k = 0; k < kN; ++k) list.insert(k);
    std::uint64_t tall = 0;
    list.for_each_item([&](std::uint64_t, unsigned h, const auto&) {
      if (h >= 2) ++tall;
    });
    // P[H >= 2] = r = 1/3; sd of the count is about 26.
    check(tall > 900 && tall < 1100,
          "move-to-top geometric insert: P[H >= 2] = q / (1 + q)");
    bsl::Rng rng(17);
    bool ok = true, formed = true;
    for (int t = 0; t < 20000; ++t) {
      const std::uint64_t k = rng.next() % kN;
      list.access(k);
      ok = ok && list.height_of(k) == L;
      if (t % 2000 == 0) formed = formed && list.well_formed();
    }
    unsigned hmax = 0;
    list.for_each_item([&](std::uint64_t, unsigned h, const auto&) {
      hmax = std::max(hmax, h);
    });
    check(ok && formed && list.well_formed() && hmax == L,
          "move-to-top variants: hits at L, well formed, nothing above L");
    std::printf("  move-to-top %s, geometric insert: %llu of %llu inserted "
                "at height >= 2, %llu demotions\n",
                exempt ? "exempt" : "no exemption",
                static_cast<unsigned long long>(tall),
                static_cast<unsigned long long>(kN),
                static_cast<unsigned long long>(list.demotions()));
  }
}

// The based rule: no item ever sits below its insertion height, under either
// exemption setting, on a sequence that erodes the unbased rule (a decreasing
// sweep over a contiguous block, repeated).
void based_rule_holds() {
  constexpr std::uint64_t kN = 2000;
  for (bool exempt : {true, false}) {
    const unsigned L = bsl::MoveToTopLeakPolicy::default_top(kN, 0.25);
    bsl::MoveToTopLeakSkipList list(
        bsl::MoveToTopLeakPolicy(0.25, L, exempt, bsl::LeakInsert::kGeometric,
                                 /*based=*/true), 31);
    for (std::uint64_t k = 0; k < kN; ++k) list.insert(k);
    bool formed = true;
    for (int round = 0; round < 300; ++round) {
      for (std::uint64_t j = 64; j >= 1; --j) list.access(kN / 2 + j);
      list.access(kN / 2);
      if (round % 50 == 0) formed = formed && list.well_formed();
    }
    bool above = true;
    std::uint64_t at_base = 0;
    list.for_each_item([&](std::uint64_t, unsigned h, const auto& st) {
      above = above && h >= st.base;
      if (h == st.base) ++at_base;
    });
    check(above, "based: no item below its insertion height");
    check(formed && list.well_formed() && list.demotions() > 0,
          "based: well formed, and the leak still fires");
    std::printf("  based move-to-top %s: %llu of %llu items at their "
                "insertion height, %llu demotions\n",
                exempt ? "exempt" : "no exemption",
                static_cast<unsigned long long>(at_base),
                static_cast<unsigned long long>(kN),
                static_cast<unsigned long long>(list.demotions()));
  }
}

}  // namespace

int main() {
  std::printf("bsl sanity checks\n");

  bsl::ClassicSkipList classic(bsl::ClassicPolicy(0.5), 1);
  dictionary_invariants(classic, "classic");

  bsl::GatedSkipList gated(bsl::GatedPolicy(0.5, 3), 2);
  dictionary_invariants(gated, "gated");

  bsl::ExactGateSkipList exact(bsl::ExactGatePolicy(0.5), 3);
  dictionary_invariants(exact, "exact-gate (legacy)");

  bsl::MorrisGateSkipList morris(bsl::MorrisGatePolicy(0.5), 4);
  dictionary_invariants(morris, "morris-gate (legacy)");

  bsl::ExactEstimatorSkipList exact_est(bsl::ExactEstimatorPolicy(0.5), 5);
  dictionary_invariants(exact_est, "exact-estimator");

  bsl::MorrisEstimatorSkipList morris_est(bsl::MorrisEstimatorPolicy(0.5), 6);
  dictionary_invariants(morris_est, "morris-estimator");

  gated_promotion_is_monotone();
  classic_policy_is_static();
  geometric_matches_tail();
  weight_floor_holds();
  counter_gates_keep_height_invariant();
  floor_must_not_be_a_running_maximum();
  reseed_schedule_hand_checked();
  reseed_keeps_dictionary(bsl::GatedPolicy(0.5, 3), "gated");
  reseed_keeps_dictionary(bsl::ExactGatePolicy(0.5), "exact");
  reseed_keeps_dictionary(bsl::MorrisGatePolicy(0.5), "morris");
  reseed_keeps_dictionary(bsl::ExactEstimatorPolicy(0.5), "exact-estimator");
  reseed_keeps_dictionary(bsl::MorrisEstimatorPolicy(0.5), "morris-estimator");
  ceiling_holds();
  ceiling_topup_holds();
  shadow_reseed_bounded_work();
  shadow_reseed_dynamic();
  compressed_pointer_bound(bsl::GatedPolicy(0.5, 3), "gated");
  compressed_pointer_bound(bsl::ExactEstimatorPolicy(0.5), "exact-estimator");
  demotion_hand_checked();
  move_to_top_leak_holds();
  gated_leak_reduces_to_gated();
  demotion_without_exemption_hand_checked();
  move_to_top_variants_hold();
  based_rule_holds();
  {
    bsl::MoveToTopLeakSkipList mtt(bsl::MoveToTopLeakPolicy(0.5, 13), 8);
    dictionary_invariants(mtt, "move-to-top leak");
    bsl::GatedLeakSkipList gl(bsl::GatedLeakPolicy(0.5, 1, 0.1), 9);
    dictionary_invariants(gl, "gated leak");
  }

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
