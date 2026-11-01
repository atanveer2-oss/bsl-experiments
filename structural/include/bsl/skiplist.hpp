#pragma once
// Skip list with a pluggable promotion policy and an optional weight floor.
//
// The policy decides three things: the per-item state it keeps (none for the
// counter-free rules), the height an item gets on insertion, and how an access
// changes that height. Swapping the policy is how we compare the classic skip
// list against the gated family without changing the search or splice code.
//
// The weight floor (analysis/bounded-depth.md) is not a policy: it is driven by
// the operation clock, and it raises every item below `floor` to `floor + G`
// in one bottom-level sweep. Levels below the floor are identical for every
// item, so the search starts its descent at the floor — that is what pointer
// compression would do for free, and it is what makes the vertical span
// O(log n) instead of O(log F).
//
// Epochal reseeding (combined/main.tex, Definition "Epochal reseeding") is
// also not a policy: it is driven by a deterministic charge schedule
// (EpochSchedule below). At each epoch boundary every item's height and policy
// state are redrawn from the insertion law and the structure is rebuilt in one
// bottom-level sweep. The rebuild work is reported separately so that callers
// can add it to the cost.
//
// The shadow (double-buffered) variant of reseeding (ReseedConfig::shadow,
// analysis/frontier.md "a shadow rebuild") builds the next epoch's host
// incrementally in an inactive second list while the current epoch runs, and
// swaps it in at the boundary, so no single operation pays the Theta(n)
// rebuild.
//
// Demoting rules (MoveToTopLeakPolicy, GatedLeakPolicy) are policies with an
// extra on_pass hook: a served search may lower the items it passes at their
// top level by one, by unsplicing that level's link (descend_leak). Policies
// without the hook compile to the original monotone code path.
//
// Every operation's work (its search cost plus any rebuild, shadow, floor or
// top-up work it triggered) is tracked, and the maximum is max_op_work().
//
// Cost accounting follows the paper: vertical span plus horizontal sibling
// steps of the search path.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <concepts>
#include <cstdlib>
#include <vector>

#include "bsl/rng.hpp"

namespace bsl {

inline constexpr unsigned kMaxLevel = 64;

struct SearchCost {
  std::uint64_t vertical = 0;    // levels descended
  std::uint64_t horizontal = 0;  // forward steps within a level
  std::uint64_t total() const { return vertical + horizontal; }
};

// ---------------------------------------------------------------------------
// Promotion policies
//
// Interface:
//   struct State;                                  per-item state
//   double p;                                      list parameter, b = 1/p
//   unsigned initial_height(State&, Rng&) const;   height at insertion
//   unsigned on_access(State&, unsigned h, Rng&);  new height (>= h)
//
// Optional, for demoting rules (see "Demoting policies" below):
//   bool on_pass(State&, unsigned h, Rng&);        demote a passed item by 1?
// A policy without on_pass never lowers a height, and the structure compiles
// the demotion path out for it, so the counter-based and gated rules run the
// same code and draw the same random numbers as before demotion existed.
// ---------------------------------------------------------------------------

// Classic skip list: height drawn once at insert, accesses never change it.
struct ClassicPolicy {
  struct State {};
  double p;

  explicit ClassicPolicy(double p_) : p(p_) {}

  unsigned initial_height(State&, Rng& rng) const {
    return 1u + rng.geometric0(p);
  }
  unsigned on_access(State&, unsigned height, Rng&) const { return height; }
};

// Self-referential gate (counter-free): the height is the rank estimate.
//
//   (i)   open a gate with probability p^c;
//   (ii)  if the gate fails, do nothing;
//   (iii) if it succeeds, draw J in {1..K} and G ~ Geom_0(1-p), c <- c + J + G.
//
// Insertion gives H = J0 + G0. Heights never decrease.
struct GatedPolicy {
  struct State {};
  double p;
  unsigned K;

  GatedPolicy(double p_, unsigned K_) : p(p_), K(K_) { assert(K_ >= 1); }

  unsigned initial_height(State&, Rng& rng) const {
    return jump(rng) + rng.geometric0(p);
  }

  unsigned on_access(State&, unsigned height, Rng& rng) const {
    // p^c underflows to 0 for large c, which is the right limit.
    const double gate = std::pow(p, static_cast<double>(height));
    if (!rng.bernoulli(gate)) return height;
    return height + jump(rng) + rng.geometric0(p);
  }

 private:
  unsigned jump(Rng& rng) const {
    return K == 1u ? 1u : rng.uniform_int(K);
  }
};

// Two-gate one-jump rule (2G1J), Definition 9.1 of the combined paper. This
// is the rule section 9 rules out, and it is here as the adversarial control:
// the geometric hangs off a *second* gate, so it fires with probability p^c
// instead of unconditionally, the increment law is not q-thick at the seam
// r = 1 -> 2, and the fixed-time hazard genuinely fails.
//
//   gate 1 w.p. p^c  ->  +1;  then gate 2 w.p. pi_c  ->  a further 1 + G.
//
// `rho` implements the floored second gate pi_c = max(p^c, rho) of
// Proposition 9.14: rho = 0 is plain 2G1J, and any constant rho > 0 restores
// q-thickness with q = rho(1-p)/(1-rho), hence Theorems A-C. The knob is the
// point -- the structure is untuned, not broken.
struct TwoGatePolicy {
  struct State {};
  double p;
  double rho;

  explicit TwoGatePolicy(double p_, double rho_ = 0.0) : p(p_), rho(rho_) {
    assert(rho_ >= 0.0 && rho_ < 1.0);
  }

  unsigned initial_height(State&, Rng& rng) const {
    // Same initialization as the one-gate arm, so the comparison isolates the
    // update rule and not the seeding.
    return 1u + rng.geometric0(p);
  }

  unsigned on_access(State&, unsigned height, Rng& rng) const {
    const double gate1 = std::pow(p, static_cast<double>(height));
    if (!rng.bernoulli(gate1)) return height;
    unsigned h = height + 1u;
    const double gate2 = std::max(gate1, rho);
    if (rng.bernoulli(gate2)) h += 1u + rng.geometric0(p);
    return h;
  }
};

// LEGACY. Exact gate of the old RSASL paper, verbatim: keep f_x and
// r_x = floor(log_b f_x), and when the height has been caught by the rank,
// promote by 1 if the rank just increased, then by G (Algorithm RSASL-SEARCH).
// This is not the rule the paper now defines and proves (see
// ExactEstimatorPolicy below): for p > 1/2 a rank can rise by two in one
// access, and this rule then leaves the height below the rank for good. Kept
// as a named baseline so that a figure can say which rule it measured.
struct ExactGatePolicy {
  struct State {
    std::uint64_t f = 1;  // insertion counts as the first access
    unsigned r = 0;
  };
  double p;

  explicit ExactGatePolicy(double p_) : p(p_) {}

  unsigned initial_height(State& s, Rng& rng) const {
    s = State{};
    return 1u + rng.geometric0(p);
  }

  unsigned on_access(State& s, unsigned height, Rng& rng) const {
    ++s.f;
    const unsigned r_new = rank(s.f);
    unsigned h = height;
    if (height == s.r) {
      if (r_new > s.r) h += 1;
      h += rng.geometric0(p);
    }
    s.r = r_new;
    return h;
  }

 private:
  unsigned rank(std::uint64_t f) const {
    return static_cast<unsigned>(std::floor(std::log(static_cast<double>(f)) /
                                            std::log(1.0 / p)));
  }
};

// LEGACY. Morris gate of the old RSASLA paper, verbatim (Algorithm
// RSASL-SEARCH-EFF): an approximate counter R_x incremented with probability
// p^R; when the height equals the counter it rises by G whether or not the
// coin succeeded, and by one more if it did. This is not the rule the paper
// now defines and proves (see MorrisEstimatorPolicy below): the unconditional
// G is a promotion without the p^c gate, which the crossing tail (T1) needs.
// Kept as a named baseline so that a figure can say which rule it measured.
struct MorrisGatePolicy {
  struct State {
    unsigned R = 1;  // Flajolet's convention: start at unity
  };
  double p;

  explicit MorrisGatePolicy(double p_) : p(p_) {}

  unsigned initial_height(State& s, Rng& rng) const {
    s = State{};
    return 1u + rng.geometric0(p);
  }

  unsigned on_access(State& s, unsigned height, Rng& rng) const {
    const bool heads = rng.bernoulli(std::pow(p, static_cast<double>(s.R)));
    unsigned h = height;
    if (height == s.R) {
      if (heads) h += 1;
      h += rng.geometric0(p);
    }
    if (heads) ++s.R;
    return h;
  }
};

// Exact estimator, the paper's rule (submission: def:estimators and
// Definition "Exact estimator" in Appendix A; the same in the full version).
// The item stores its operation count f_x, insertion included, and its rank
// floor(log_b f_x). On an access, f_x is incremented; if now H_x <= rank_x,
// the height is redrawn as rank_x + G with a fresh G ~ Geom_0(1-p). Otherwise
// nothing happens.
//
// H_x >= rank_x holds after every operation, for every p. The excess
// H_x - rank_x has the tail P[E >= e] = (1-theta_j) p^e + theta_j p^(e-1)
// (Lemma "The excess"), so P[E >= e] <= p^(e-1).
struct ExactEstimatorPolicy {
  struct State {
    std::uint64_t f = 1;      // operations on the item, insertion included
    unsigned rank = 0;        // floor(log_b f)
    double next_power = 0.0;  // b^(rank+1), so the rank update is O(1)
  };
  double p;

  explicit ExactEstimatorPolicy(double p_) : p(p_) {
    assert(p_ > 0.0 && p_ < 1.0);
  }

  unsigned initial_height(State& s, Rng& rng) const {
    s = State{};
    s.next_power = 1.0 / p;
    return 1u + rng.geometric0(p);
  }

  unsigned on_access(State& s, unsigned height, Rng& rng) const {
    ++s.f;
    advance_rank(s);
    if (height <= s.rank) return s.rank + rng.geometric0(p);
    return height;
  }

  // floor(log_b f) computed as the policy computes it, for tests and plots.
  static unsigned rank_of(std::uint64_t f, double p) {
    State s;
    s.f = f;
    s.next_power = 1.0 / p;
    ExactEstimatorPolicy(p).advance_rank(s);
    return s.rank;
  }

 private:
  // The rank is the largest r with b^r <= f. Repeated multiplication is exact
  // for integer b and, for other b, b^r is never an integer, so a relative
  // tolerance of 1e-12 cannot change a comparison; it only protects the exact
  // powers (b = 2, f = 8) from rounding in the other direction.
  void advance_rank(State& s) const {
    const double b = 1.0 / p;
    const double f = static_cast<double>(s.f) * (1.0 + 1e-12);
    while (s.next_power <= f) {
      ++s.rank;
      s.next_power *= b;
    }
  }
};

// Morris estimator, the paper's rule (submission: def:estimators and
// Definition "Morris estimator" in Appendix A; the same in the full version).
// The item stores an approximate counter Q_x, starting at 1, incremented
// with probability p^Q per access. If the counter is incremented and
// H_x = Q_x before the increment, the height rises by 1 + G with a fresh
// G ~ Geom_0(1-p). Otherwise nothing happens. H_x >= Q_x always.
struct MorrisEstimatorPolicy {
  struct State {
    unsigned Q = 1;  // Flajolet's convention: start at unity
  };
  double p;

  explicit MorrisEstimatorPolicy(double p_) : p(p_) {
    assert(p_ > 0.0 && p_ < 1.0);
  }

  unsigned initial_height(State& s, Rng& rng) const {
    s = State{};
    return 1u + rng.geometric0(p);
  }

  unsigned on_access(State& s, unsigned height, Rng& rng) const {
    if (!rng.bernoulli(std::pow(p, static_cast<double>(s.Q)))) return height;
    unsigned h = height;
    if (height == s.Q) h += 1u + rng.geometric0(p);
    ++s.Q;
    return h;
  }
};

// ---------------------------------------------------------------------------
// Demoting policies
//
// A demoting policy also defines on_pass. During a served search (access(),
// not insert(), probe() or the other read-only lookups), every item the
// horizontal scan at level l passes has height exactly l: an item of height
// > l between the scan's start and the level-(l+1) successor would have been
// in the level-(l+1) list. So "passed at its top level" is "stepped over in
// the scan at level l", excluding the descent point (the last node of the
// scan, which is the splice predecessor at level l; a policy can opt out of
// this exemption with exempt_descent = false) and the target. For each
// such item, after the scan at that level is complete and in scan order,
// on_pass decides whether it is demoted by one level: its level-l link is
// unspliced and its lower levels are kept. An item of height 1 is never
// demoted (the scan at the bottom level does not call on_pass).
// ---------------------------------------------------------------------------

template <typename Policy>
concept DemotingPolicy =
    requires(Policy& pol, typename Policy::State& s, unsigned h, Rng& r) {
      { pol.on_pass(s, h, r) } -> std::same_as<bool>;
    };

// Insertion law of the demoting rules. kNative is the rule's own (height 1 for
// move-to-top, the gated insertion J + G for GatedLeakPolicy); kFlat is
// height 1; kGeometric is a Pugh height 1 + Geom_0(1 - r), r = q / (1 + q), so
// that the base structure has level ratio 1/r = 1 + 1/q (capped at L for
// move-to-top, so L stays the top level).
enum class LeakInsert { kNative, kFlat, kGeometric };

inline unsigned leak_geometric_height(double q, Rng& rng) {
  return 1u + rng.geometric0(q / (1.0 + q));
}

// Whether a demoting policy exempts the descent point of each level from the
// leak; true unless the policy says otherwise.
template <typename Policy>
constexpr bool exempt_descent_of(const Policy& pol) {
  if constexpr (requires { pol.exempt_descent; }) {
    return pol.exempt_descent;
  } else {
    return true;
  }
}

// Move-to-top with leak. Insertion puts an item at height 1 (or, with
// LeakInsert::kGeometric, at a Pugh height with ratio 1 + 1/q, capped at L).
// An access to x, found at its top level H_x, promotes x to the fixed top
// level L (spliced into levels H_x+1..L with the search path's predecessors,
// as an insertion would). Every item the search passes at its top level is
// demoted by one level with probability q, independently. With
// exempt_descent (the default, "mtt'"), the descent point of each level is
// not passed; without it ("mtt"), it is, and if it is demoted the splice
// predecessor at that level moves back to the previous surviving node.
//
// With floor_at_base ("based"), every item keeps its insertion height G_x in
// its state and on_pass never demotes it below G_x (no coin is drawn for an
// item already at G_x). Without it the state is still recorded but unused.
//
// L is fixed for a run. default_top(n, q) = ceil(log_{1+1/q} n) + 2, the
// level ratio the leak produces near the top; legacy_top(n, q) =
// ceil(log_{1/q} n) + 2, the first-round default. `p` is the list parameter
// seen by the clocked machinery (b = 1/p); it is set to q and unused unless
// the ceiling is enabled.
struct MoveToTopLeakPolicy {
  struct State {
    unsigned base = 1;  // insertion height G_x
  };
  double p;
  double q;
  unsigned L;
  bool exempt_descent = true;
  LeakInsert insert = LeakInsert::kFlat;
  bool floor_at_base = false;

  MoveToTopLeakPolicy(double q_, unsigned L_, bool exempt = true,
                      LeakInsert ins = LeakInsert::kFlat, bool based = false)
      : p(q_), q(q_), L(L_), exempt_descent(exempt), insert(ins),
        floor_at_base(based) {
    assert(q_ > 0.0 && q_ < 1.0 && L_ >= 1);
  }

  static unsigned top_for_base(std::size_t n, double base) {
    if (n < 2) return 3u;
    const double lg = std::log(static_cast<double>(n)) / std::log(base);
    // The epsilon keeps exact powers (n = 1024, q = 1/4) from rounding up.
    return static_cast<unsigned>(std::ceil(lg - 1e-9)) + 2u;
  }
  static unsigned default_top(std::size_t n, double q) {
    return top_for_base(n, 1.0 + 1.0 / q);
  }
  static unsigned legacy_top(std::size_t n, double q) {
    return top_for_base(n, 1.0 / q);
  }

  unsigned initial_height(State& s, Rng& rng) const {
    if (insert != LeakInsert::kGeometric) {
      s.base = 1u;
      return 1u;
    }
    const unsigned h = leak_geometric_height(q, rng);
    s.base = h < L ? h : L;
    return s.base;
  }
  unsigned on_access(State&, unsigned height, Rng&) const {
    return height > L ? height : L;
  }
  bool on_pass(State& s, unsigned height, Rng& rng) const {
    if (floor_at_base && height <= s.base) return false;
    return rng.bernoulli(q);
  }
};

// The one-gate rule (GatedPolicy, same promotion) plus the same leak q on
// passed items; for the stationarity check only. With the native insertion
// and q = 0 it draws no leak coins, so it reproduces GatedPolicy(p, K)
// exactly.
struct GatedLeakPolicy {
  struct State {};
  double p;
  unsigned K;
  double q;
  bool exempt_descent = true;
  LeakInsert insert = LeakInsert::kNative;

  GatedLeakPolicy(double p_, unsigned K_, double q_, bool exempt = true,
                  LeakInsert ins = LeakInsert::kNative)
      : p(p_), K(K_), q(q_), exempt_descent(exempt), insert(ins),
        gated_(p_, K_) {
    assert(q_ >= 0.0 && q_ < 1.0);
  }

  unsigned initial_height(State&, Rng& rng) const {
    if (insert == LeakInsert::kFlat) return 1u;
    if (insert == LeakInsert::kGeometric) return leak_geometric_height(q, rng);
    GatedPolicy::State gs;
    return gated_.initial_height(gs, rng);
  }
  unsigned on_access(State&, unsigned height, Rng& rng) const {
    GatedPolicy::State gs;
    return gated_.on_access(gs, height, rng);
  }
  bool on_pass(State&, unsigned, Rng& rng) const {
    return q > 0.0 && rng.bernoulli(q);
  }

 private:
  GatedPolicy gated_;
};

// ---------------------------------------------------------------------------
// Weight floor
// ---------------------------------------------------------------------------

struct FloorConfig {
  bool enabled = false;
  // floor(t) = max(0, floor(log_b(t / n)) - c0). c0 only controls how many
  // items a raise touches; it does not affect correctness.
  int c0 = 0;
};

// ---------------------------------------------------------------------------
// Epochal reseeding
// ---------------------------------------------------------------------------

struct ReseedConfig {
  bool enabled = false;
  // Shadow (double-buffered) rebuild; requires `enabled`. At the first charge
  // of every epoch an inactive second list is started. After every charge
  // the next ceil(d/N) + 1 keys of the active list (d = items, N = capacity),
  // in key order, are copied into it with fresh insertion-law heights and
  // policy state. Searches and promotions use only the active list. At the
  // epoch boundary the inactive list replaces the active one in O(1) and the
  // old nodes are recycled into the next shadow. The epoch schedule, and the
  // law of every epoch's starting configuration, are those of the eager
  // rebuild; only the timing of the construction work changes.
  //
  // Dynamic case. A new key that lands ahead of the copy cursor is picked up
  // by the cursor. A new key behind the cursor makes this epoch's shadow
  // stale: it stops advancing, and the boundary falls back to the eager
  // rebuild (shadow_fallbacks()). A shadow that is merely incomplete at the
  // boundary (only possible when a doubling ends the epoch early) is finished
  // eagerly by the operation that hits the boundary (shadow_catchups()).
  bool shadow = false;
};

// ---------------------------------------------------------------------------
// Clocked ceiling
// ---------------------------------------------------------------------------

// At every charge (insert or access), Lambda = floor(log_b ops) + c1 with
// b = 1/p and ops the number of charges so far, including this one. Any height
// produced by an insertion or by an on_access promotion is clamped to
// min(h, max(Lambda, 1)). Lambda only grows, so no existing height is ever
// above it; heights are never lowered. Floor raises are not capped.
//
// Top-up (optional): whenever Lambda rises from l-1 to l, every item at
// height exactly l-1 is raised to l with probability p, independently, by one
// sweep of the chain at level index l-2 (the nodes of height >= l-1). The
// sweep costs 1 per node visited plus 1 per splice (topup_work()).
struct CeilingConfig {
  bool enabled = false;
  int c1 = 2;
  bool topup = false;
};

// The epoch schedule of the reseeding definition, kept separate from the
// structure so that a benchmark can replay it on a sequence (for per-epoch
// benchmarks) without running a skip list.
//
// d = distinct items introduced so far, N = least power of two >= d (N = 1
// while d = 0). An epoch ends after N promotion charges (every insertion and
// every access is one charge), or immediately before an insertion that would
// double N, whichever comes first. The restart is lazy: it happens when the
// next charge arrives, after any capacity increase that charge forces, so
// there are no empty epochs and no duplicate rebuilds.
class EpochSchedule {
 public:
  // Registers one charge; `introduces_item` is true for an insertion of a key
  // not seen before. Returns true when the structure must be reseeded and
  // rebuilt before this charge is served.
  bool charge(bool introduces_item) {
    bool doubled = false;
    if (introduces_item) {
      ++distinct_;
      while (capacity_ < distinct_) {
        capacity_ *= 2;
        doubled = true;
      }
    }
    const bool restart =
        charges_ > 0 && (doubled || charges_ >= capacity_);
    if (epochs_ == 0) epochs_ = 1;  // the first charge opens the first epoch
    if (restart) {
      ++epochs_;
      charges_ = 0;
    }
    ++charges_;
    return restart;
  }

  std::uint64_t distinct() const { return distinct_; }
  std::uint64_t capacity() const { return capacity_; }
  std::uint64_t charges_in_epoch() const { return charges_; }
  // Epochs opened so far, including the current one (0 before any charge).
  std::uint64_t epochs() const { return epochs_; }

 private:
  std::uint64_t distinct_ = 0;
  std::uint64_t capacity_ = 1;
  std::uint64_t charges_ = 0;
  std::uint64_t epochs_ = 0;
};

// ---------------------------------------------------------------------------
// The structure
// ---------------------------------------------------------------------------

template <typename Policy, typename Key = std::uint64_t>
class SkipList {
 public:
  SkipList(Policy policy, std::uint64_t seed, FloorConfig floor = {},
           ReseedConfig reseed = {}, CeilingConfig ceiling = {})
      : policy_(policy),
        floor_cfg_(floor),
        reseed_cfg_(reseed),
        ceiling_cfg_(ceiling),
        rng_(seed),
        head_(new Node(Key{}, kMaxLevel)) {
    // The floor and reseeding are not yet specified together. Checked in
    // Release builds too, since the benchmarks run with NDEBUG.
    if (floor.enabled && reseed.enabled) {
      std::fprintf(stderr, "SkipList: weight floor and reseeding are "
                           "mutually exclusive\n");
      std::abort();
    }
    if (ceiling.enabled && reseed.enabled) {
      std::fprintf(stderr, "SkipList: ceiling and reseeding are mutually "
                           "exclusive\n");
      std::abort();
    }
    // Demotion would break the floor's invariant H_y >= floor_.
    if (DemotingPolicy<Policy> && floor.enabled) {
      std::fprintf(stderr, "SkipList: the weight floor needs heights that "
                           "never decrease\n");
      std::abort();
    }
    if (reseed.shadow && !reseed.enabled) {
      std::fprintf(stderr, "SkipList: the shadow rebuild requires "
                           "reseeding\n");
      std::abort();
    }
  }

  ~SkipList() {
    delete_chain(head_);
    delete_chain(shadow_head_);
    delete_chain(pool_);
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Inserts `key` if absent. Returns the cost of the search it performed.
  SearchCost insert(const Key& key) {
    const std::uint64_t overhead0 = overhead_work();
    tick();
    if (reseed_cfg_.enabled) {
      // Whether this insertion introduces an item decides whether it can
      // double the capacity, so look before charging. This membership probe is
      // bookkeeping, not part of the served search, and is not counted.
      SearchCost scratch;
      const bool is_new = descend(key, nullptr, &scratch) == nullptr;
      reseed_charge(is_new);
      // A new key behind the shadow's copy cursor would be missed.
      if (is_new && shadow_building_ && shadow_cursor_ != head_ &&
          key < shadow_cursor_->key) {
        shadow_stale_ = true;
      }
    }
    std::vector<Node*> update(kMaxLevel, head_);
    SearchCost cost;
    Node* found = descend(key, &update, &cost);
    if (found) {
      advance_shadow();
      record_op(cost, overhead0);
      return cost;
    }

    Node* node = new Node(key, 1);
    node->height =
        cap(clamp(floor_ + policy_.initial_height(node->state, rng_)));
    node->next.assign(node->height, nullptr);
    for (unsigned i = 0; i < node->height; ++i) {
      node->next[i] = update[i]->next[i];
      update[i]->next[i] = node;
    }
    if (node->height > level_) level_ = node->height;
    ++size_;
    advance_shadow();
    record_op(cost, overhead0);
    return cost;
  }

  // Searches for `key`. On a hit the policy may raise the item's height; the
  // returned cost is the cost of the search as served, i.e. before promotion.
  SearchCost access(const Key& key, bool* found_out = nullptr) {
    const std::uint64_t overhead0 = overhead_work();
    tick();
    if (reseed_cfg_.enabled) reseed_charge(false);
    std::vector<Node*> update(kMaxLevel, head_);
    SearchCost cost;
    Node* found;
    if constexpr (DemotingPolicy<Policy>) {
      found = descend_leak(key, &update, &cost);
    } else {
      found = descend(key, &update, &cost);
    }
    if (found_out) *found_out = (found != nullptr);
    if (found) {
      const unsigned new_h =
          cap(clamp(policy_.on_access(found->state, found->height, rng_)));
      splice_up(found, new_h, update);
    }
    if constexpr (DemotingPolicy<Policy>) {
      // Levels emptied by demotions are dropped, so the vertical span is
      // measured from the highest occupied level, as with monotone policies.
      while (level_ > 1 && head_->next[level_ - 1] == nullptr) --level_;
    }
    advance_shadow();
    record_op(cost, overhead0);
    return cost;
  }

  // Read-only search: no promotion and no clock tick, so it does not perturb
  // the configuration. This is how the separation benchmark measures the cost
  // a search *would* pay against the shape the sequence has produced.
  SearchCost probe(const Key& key) {
    SearchCost cost;
    descend(key, nullptr, &cost);
    return cost;
  }

  bool contains(const Key& key) {
    SearchCost cost;
    return descend(key, nullptr, &cost) != nullptr;
  }

  // Height of `key`, or 0 if absent.
  unsigned height_of(const Key& key) {
    SearchCost cost;
    Node* n = descend(key, nullptr, &cost);
    return n ? n->height : 0u;
  }

  // The policy's per-item state for `key`, or nullptr if absent. Read-only,
  // for tests and plots that compare a height with its rank.
  const typename Policy::State* state_of(const Key& key) {
    SearchCost cost;
    Node* n = descend(key, nullptr, &cost);
    return n ? &n->state : nullptr;
  }

  std::size_t size() const { return size_; }
  unsigned level() const { return level_; }
  unsigned floor_level() const { return floor_; }
  std::uint64_t raises() const { return raises_; }
  std::uint64_t raise_work() const { return raise_work_; }

  // Reseeding: epochs opened (0 with reseeding off), rebuilds performed
  // (epochs - 1 once the first charge has arrived), and the cumulative rebuild
  // work, one unit per node plus one per level it is spliced into.
  std::uint64_t epochs() const { return schedule_.epochs(); }
  std::uint64_t rebuilds() const { return rebuilds_; }
  std::uint64_t rebuild_work() const { return rebuild_work_; }

  // Shadow rebuild: cumulative construction work (1 per node copied into the
  // shadow plus 1 per level it is spliced into, the same unit as
  // rebuild_work()), boundaries served by a swap, boundaries at which the
  // shadow had to be finished eagerly, and boundaries that fell back to the
  // eager rebuild because a new key landed behind the copy cursor. Eager
  // fallbacks are counted in rebuild_work(), catch-ups in shadow_work().
  std::uint64_t shadow_work() const { return shadow_work_; }
  std::uint64_t shadow_swaps() const { return shadow_swaps_; }
  std::uint64_t shadow_catchups() const { return shadow_catchups_; }
  std::uint64_t shadow_fallbacks() const { return shadow_fallbacks_; }

  // Largest work of any single insert or access so far: its search cost
  // (SearchCost::total()) plus the rebuild, shadow, floor-raise and top-up
  // work that operation performed. max_op_overhead() is the same maximum
  // without the search cost.
  std::uint64_t max_op_work() const { return max_op_work_; }
  std::uint64_t max_op_overhead() const { return max_op_overhead_; }

  // Current ceiling max(Lambda, 1), or kMaxLevel with the ceiling off.
  unsigned ceiling_level() const { return ceiling_; }
  std::uint64_t topup_work() const { return topup_work_; }

  // Demotions performed so far (demoting policies only; 0 otherwise).
  std::uint64_t demotions() const { return demotions_; }

  // Number of items whose height is exactly h; O(items at level h).
  std::size_t count_at_height(unsigned h) const {
    if (h < 1 || h > kMaxLevel) return 0;
    std::size_t c = 0;
    for (Node* n = head_->next[h - 1]; n; n = n->next[h - 1]) {
      if (n->height == h) ++c;
    }
    return c;
  }
  const EpochSchedule& schedule() const { return schedule_; }

  // Structural invariants, O(n * kMaxLevel); for tests. Level 0 holds exactly
  // size() nodes in strictly increasing key order, every level i holds exactly
  // the level-0 nodes of height > i in the same order, each node has `height`
  // forward pointers, and level() is at least the maximum height.
  bool well_formed() const {
    std::size_t count = 0;
    unsigned max_h = 0;
    for (Node* n = head_->next[0]; n; n = n->next[0]) {
      ++count;
      if (n->height < 1 || n->next.size() != n->height) return false;
      if (n->next[0] && !(n->key < n->next[0]->key)) return false;
      max_h = std::max(max_h, n->height);
    }
    if (count != size_ || level_ < max_h) return false;
    for (unsigned i = 1; i < kMaxLevel; ++i) {
      Node* a = head_->next[i];
      Node* b = head_->next[0];
      while (b && b->height <= i) b = b->next[0];
      while (a || b) {
        if (a != b) return false;
        a = a->next[i];
        b = b->next[0];
        while (b && b->height <= i) b = b->next[0];
      }
    }
    return true;
  }

  // Minimum height over all items; O(n). For the depth measurements.
  unsigned min_height() const {
    unsigned m = kMaxLevel;
    for (Node* n = head_->next[0]; n; n = n->next[0]) {
      if (n->height < m) m = n->height;
    }
    return size_ ? m : 0u;
  }

  // Forward pointers held by items when every level is stored: the sum of
  // the heights, head excluded. O(n).
  std::uint64_t pointer_count() const {
    std::uint64_t c = 0;
    for (Node* n = head_->next[0]; n; n = n->next[0]) c += n->height;
    return c;
  }

  // Pointers stored by the compressed representation (submission,
  // def:compression): a node that points to the same node at several
  // consecutive levels stores one pointer plus a span counter. So this counts,
  // per node, the distinct non-null successors among next[0..height-1]; the
  // successors at consecutive levels are nondecreasing in key, so the count is
  // the number of runs. lem:pointercount bounds it by 2n with the head
  // included or not. O(sum of heights).
  std::uint64_t compressed_pointer_count(bool include_head = true) const {
    std::uint64_t c = 0;
    auto count_node = [&c](const Node* n) {
      const Node* prev = nullptr;
      for (const Node* s : n->next) {
        if (s && s != prev) ++c;
        prev = s;
      }
    };
    if (include_head) count_node(head_);
    for (Node* n = head_->next[0]; n; n = n->next[0]) count_node(n);
    return c;
  }

  // Visits every item in key order as fn(key, height, state). Read-only, for
  // measurements that need every height at once without a search per key.
  template <typename Fn>
  void for_each_item(Fn&& fn) const {
    for (const Node* n = head_->next[0]; n; n = n->next[0]) {
      fn(n->key, n->height, n->state);
    }
  }

 private:
  struct Node {
    Key key;
    unsigned height;
    std::vector<Node*> next;
    typename Policy::State state{};
    Node(const Key& k, unsigned h) : key(k), height(h), next(h, nullptr) {}
  };

  static void delete_chain(Node* n) {
    while (n) {
      Node* next = n->next[0];
      delete n;
      n = next;
    }
  }

  // All non-search work performed so far, for the per-operation maximum.
  std::uint64_t overhead_work() const {
    return rebuild_work_ + shadow_work_ + raise_work_ + topup_work_;
  }

  void record_op(const SearchCost& cost, std::uint64_t overhead0) {
    const std::uint64_t over = overhead_work() - overhead0;
    max_op_overhead_ = std::max(max_op_overhead_, over);
    max_op_work_ = std::max(max_op_work_, cost.total() + over);
  }

  // One reseeding charge, before the operation is served: handle an epoch
  // boundary if the schedule says so, then start the next shadow if none is
  // being built (the first charge of every epoch, including the first).
  void reseed_charge(bool is_new) {
    if (schedule_.charge(is_new)) {
      if (reseed_cfg_.shadow && shadow_building_ && !shadow_stale_) {
        swap_in_shadow();
      } else {
        if (shadow_building_) {
          discard_shadow();
          ++shadow_fallbacks_;
        }
        rebuild();
      }
    }
    if (reseed_cfg_.shadow && !shadow_building_) begin_shadow();
  }

  // Empties the inactive list: its head links nothing, the copy cursor is
  // before the first active key. O(kMaxLevel).
  void begin_shadow() {
    if (!shadow_head_) shadow_head_ = new Node(Key{}, kMaxLevel);
    for (unsigned i = 0; i < kMaxLevel; ++i) shadow_head_->next[i] = nullptr;
    shadow_last_.assign(kMaxLevel, shadow_head_);
    shadow_cursor_ = head_;
    shadow_level_ = 0;
    shadow_size_ = 0;
    shadow_building_ = true;
    shadow_stale_ = false;
  }

  // Copies the active key after the cursor into the shadow, with a fresh
  // insertion-law height and policy state, appending it at every level.
  // Returns false when the shadow already holds every active key.
  bool shadow_step() {
    Node* src = shadow_cursor_->next[0];
    if (!src) return false;
    Node* node;
    if (pool_) {
      node = pool_;
      pool_ = pool_->next[0];
      node->key = src->key;
      node->state = typename Policy::State{};
    } else {
      node = new Node(src->key, 1);
    }
    const unsigned h = clamp(policy_.initial_height(node->state, rng_));
    node->height = h;
    node->next.assign(h, nullptr);
    for (unsigned i = 0; i < h; ++i) {
      shadow_last_[i]->next[i] = node;
      shadow_last_[i] = node;
    }
    if (h > shadow_level_) shadow_level_ = h;
    ++shadow_size_;
    shadow_work_ += 1u + h;
    shadow_cursor_ = src;
    return true;
  }

  // After every served charge: ceil(d/N) + 1 keys.
  void advance_shadow() {
    if (!shadow_building_ || shadow_stale_) return;
    const std::uint64_t N = schedule_.capacity();
    const std::uint64_t d = size_;
    const std::uint64_t quota = (d + N - 1) / N + 1;
    for (std::uint64_t i = 0; i < quota; ++i) {
      if (!shadow_step()) break;
    }
  }

  // Epoch boundary with a valid shadow: finish it if a doubling cut the
  // epoch short, then make it the active list. The old active list's level-0
  // chain becomes the node pool for the next shadow (O(1)).
  void swap_in_shadow() {
    if (shadow_cursor_->next[0]) {
      ++shadow_catchups_;
      while (shadow_step()) {
      }
    }
    assert(shadow_size_ == size_);
    ++rebuilds_;
    ++shadow_swaps_;
    // Leftover pool nodes exist only if the item set shrank; free them.
    delete_chain(pool_);
    pool_ = head_->next[0];
    std::swap(head_, shadow_head_);
    level_ = shadow_level_ ? shadow_level_ : 1u;
    shadow_building_ = false;
  }

  // Abandons a stale shadow: its nodes join the pool (O(1)).
  void discard_shadow() {
    if (shadow_size_ > 0) {
      shadow_last_[0]->next[0] = pool_;
      pool_ = shadow_head_->next[0];
    }
    shadow_head_->next[0] = nullptr;
    shadow_building_ = false;
  }

  static unsigned clamp(unsigned h) {
    if (h < 1u) return 1u;
    return h > kMaxLevel ? kMaxLevel : h;
  }

  // Clocked ceiling; the identity when it is off. Callers never lower an
  // existing height (splice_up ignores new_h <= old_h).
  unsigned cap(unsigned h) const { return h < ceiling_ ? h : ceiling_; }

  // Lowest level the search descends to. Every item has height >= floor_, so
  // levels 0..floor_-1 hold the same nodes in the same order and the descent
  // may stop at level floor_-1 (pointer compression elides the rest).
  unsigned bottom() const { return floor_ > 0 ? floor_ - 1 : 0; }

  // Walks the search path. If `update` is non-null it receives the rightmost
  // node strictly before `key` at every level visited (the splice points);
  // levels below the bottom copy the bottom's predecessor, which is exact
  // because those levels are identical. Returns the node holding `key`, or
  // nullptr.
  //
  // A successful search stops at the level where it first meets the key, i.e.
  // at the item's top level, so its vertical cost is H_max - H_x + 1 as in the
  // paper's cost model. That is the whole point of promoting hot items: they
  // are found high. Splice points for levels >= H_x are all set by then, which
  // is exactly what a subsequent promotion needs.
  Node* descend(const Key& key, std::vector<Node*>* update, SearchCost* cost) {
    Node* x = head_;
    const unsigned top = level_ ? level_ : 1u;
    const unsigned bot = bottom();
    for (unsigned i = top; i-- > bot;) {
      while (x->next[i] && x->next[i]->key < key) {
        x = x->next[i];
        ++cost->horizontal;
      }
      if (update) (*update)[i] = x;
      ++cost->vertical;
      if (x->next[i] && x->next[i]->key == key) return x->next[i];
    }
    if (update) {
      for (unsigned i = 0; i < bot; ++i) (*update)[i] = x;
    }
    return nullptr;
  }

  // descend() for a demoting policy (served searches only): the same walk
  // and the same cost, and after the scan at each level i >= 1 the items it
  // stepped over, excluding the descent point, go to the policy's on_pass.
  // The floor is never enabled here (constructor check), so the bottom is 0.
  Node* descend_leak(const Key& key, std::vector<Node*>* update,
                     SearchCost* cost) {
    Node* x = head_;
    const unsigned top = level_ ? level_ : 1u;
    for (unsigned i = top; i-- > 0;) {
      Node* const start = x;
      while (x->next[i] && x->next[i]->key < key) {
        x = x->next[i];
        ++cost->horizontal;
      }
      (*update)[i] = x;
      ++cost->vertical;
      Node* const hit =
          (x->next[i] && x->next[i]->key == key) ? x->next[i] : nullptr;
      // x stays the node the descent continues from: a demoted x keeps its
      // link at level i-1. Only the level-i splice predecessor can move.
      if (i >= 1) (*update)[i] = leak_scan(i, start, x);
      if (hit) return hit;
    }
    return nullptr;
  }

  // The items strictly between `start` and `end` on level index i all have
  // height i+1; each one on_pass selects loses its level-i link. Without the
  // descent-point exemption, `end` (also of height i+1 when it is not
  // `start`) is offered last. Returns the level-i predecessor of the search
  // key afterwards: `end`, or the last survivor before it if it was demoted.
  Node* leak_scan(unsigned i, Node* start, Node* end) {
    if (start == end) return end;  // no step at this level
    Node* prev = start;
    for (Node* y = start->next[i]; y != end;) {
      Node* const following = y->next[i];
      assert(y->height == i + 1);
      if (policy_.on_pass(y->state, y->height, rng_)) {
        prev->next[i] = following;
        y->next.resize(i);
        y->height = i;
        ++demotions_;
      } else {
        prev = y;
      }
      y = following;
    }
    if (!exempt_descent_of(policy_)) {
      assert(end->height == i + 1);
      if (policy_.on_pass(end->state, end->height, rng_)) {
        prev->next[i] = end->next[i];
        end->next.resize(i);
        end->height = i;
        ++demotions_;
        return prev;
      }
    }
    return end;
  }

  // Links `node` into levels [node->height, new_h) using `pred` as the
  // predecessor at each of those levels.
  void splice_up(Node* node, unsigned new_h, const std::vector<Node*>& pred) {
    const unsigned old_h = node->height;
    if (new_h <= old_h) return;
    node->next.resize(new_h, nullptr);
    for (unsigned i = old_h; i < new_h; ++i) {
      node->next[i] = pred[i]->next[i];
      pred[i]->next[i] = node;
    }
    node->height = new_h;
    if (new_h > level_) level_ = new_h;
  }

  // Operation clock. Advances the floor when log_b(t/n) has grown.
  void tick() {
    ++ops_;
    if (ceiling_cfg_.enabled) {
      const double lg = std::log(static_cast<double>(ops_)) /
                        std::log(1.0 / policy_.p);
      // The epsilon keeps exact powers of b from rounding down.
      const long lambda =
          static_cast<long>(std::floor(lg + 1e-9)) + ceiling_cfg_.c1;
      const unsigned next = static_cast<unsigned>(
          std::clamp<long>(lambda, 1, static_cast<long>(kMaxLevel)));
      // The first tick initializes the ceiling; it is not a rise.
      if (ceiling_cfg_.topup && ceiling_started_) {
        for (unsigned l = ceiling_ + 1; l <= next; ++l) top_up(l);
      }
      ceiling_ = next;
      ceiling_started_ = true;
    }
    if (!floor_cfg_.enabled || size_ == 0) return;
    const double b = 1.0 / policy_.p;
    const double lg = std::log(static_cast<double>(ops_) /
                               static_cast<double>(size_)) /
                      std::log(b);
    const long target =
        std::max<long>(0, static_cast<long>(std::floor(lg)) - floor_cfg_.c0);
    // The floor TRACKS log_b(t/n_t); it is deliberately NOT a running maximum.
    // A running maximum breaks the paper's Lemma 8.2(i): after a burst of
    // insertions t/n_t collapses while the maximum does not, new items then
    // enter at a floor far above log_b(t/n_t), and the max-height bound fails
    // outright -- measured at 98% violation where the lemma allows 0.2%.
    // See combined/main.tex Remark 8.5 and analysis/bounded-depth.md.
    if (target > static_cast<long>(floor_)) {
      raise_floor(static_cast<unsigned>(target));
    } else if (target < static_cast<long>(floor_)) {
      // Lowering costs nothing and lowers no height: the invariant
      // H_y >= floor_ survives a fortiori. It only means the search descends
      // further, which is correct, not an optimization we are giving up.
      floor_ = static_cast<unsigned>(target);
    }
  }

  // One left-to-right sweep of the bottom level. `last[i]` is the most recent
  // node present at level i, i.e. the predecessor for anything spliced into
  // level i at the current position.
  void raise_floor(unsigned new_floor) {
    ++raises_;
    std::vector<Node*> last(kMaxLevel, head_);
    for (Node* n = head_->next[0]; n; n = n->next[0]) {
      ++raise_work_;
      if (n->height < new_floor) {
        const unsigned target = clamp(new_floor + rng_.geometric0(policy_.p));
        raise_work_ += target - n->height;
        splice_up(n, target, last);
      }
      for (unsigned i = 0; i < n->height; ++i) last[i] = n;
    }
    floor_ = new_floor;
  }

  // Ceiling top-up for a rise of Lambda to l (l >= 2): walk the chain at level
  // index l-2, which holds every node of height >= l-1. `last` is the most
  // recent node already present at level index l-1, the predecessor for a
  // splice there. Each node of height exactly l-1 is raised to l w.p. p.
  void top_up(unsigned l) {
    if (l < 2 || l > kMaxLevel) return;
    Node* last = head_;
    for (Node* n = head_->next[l - 2]; n; n = n->next[l - 2]) {
      ++topup_work_;
      if (n->height >= l) {
        last = n;
      } else if (rng_.bernoulli(policy_.p)) {
        n->next.resize(l, nullptr);
        n->next[l - 1] = last->next[l - 1];
        last->next[l - 1] = n;
        n->height = l;
        last = n;
        ++topup_work_;
      }
    }
    // Only a top-up that actually spliced something can raise level_.
    if (head_->next[l - 1] && l > level_) level_ = l;
  }

  // Epoch restart: redraw every item's height and policy state from the
  // insertion law (initial_height resets the exact and Morris counters, so they
  // become epoch-local), then relink every level in one left-to-right sweep of
  // level 0. Key order is untouched; only heights change.
  void rebuild() {
    ++rebuilds_;
    Node* n = head_->next[0];
    std::vector<Node*> last(kMaxLevel, head_);
    for (unsigned i = 0; i < kMaxLevel; ++i) head_->next[i] = nullptr;
    unsigned top = 0;
    while (n) {
      Node* const following = n->next[0];
      const unsigned h = clamp(policy_.initial_height(n->state, rng_));
      n->height = h;
      n->next.assign(h, nullptr);
      for (unsigned i = 0; i < h; ++i) {
        last[i]->next[i] = n;
        last[i] = n;
      }
      rebuild_work_ += 1u + h;
      if (h > top) top = h;
      n = following;
    }
    level_ = top ? top : 1u;
  }

  Policy policy_;
  FloorConfig floor_cfg_;
  ReseedConfig reseed_cfg_;
  CeilingConfig ceiling_cfg_;
  unsigned ceiling_ = kMaxLevel;
  bool ceiling_started_ = false;
  std::uint64_t topup_work_ = 0;
  EpochSchedule schedule_;
  Rng rng_;
  Node* head_;
  unsigned level_ = 1;
  unsigned floor_ = 0;
  std::size_t size_ = 0;
  std::uint64_t ops_ = 0;
  std::uint64_t raises_ = 0;
  std::uint64_t raise_work_ = 0;
  std::uint64_t rebuilds_ = 0;
  std::uint64_t rebuild_work_ = 0;
  // Shadow rebuild state. shadow_head_ heads the inactive list (it doubles as
  // the retired list's head right after a swap, until begin_shadow clears
  // it); shadow_last_[i] is its rightmost node at level i; shadow_cursor_ is
  // the last active node copied (head_ before the first copy); pool_ is a
  // level-0 chain of retired nodes awaiting reuse.
  Node* shadow_head_ = nullptr;
  std::vector<Node*> shadow_last_;
  Node* shadow_cursor_ = nullptr;
  Node* pool_ = nullptr;
  unsigned shadow_level_ = 0;
  std::size_t shadow_size_ = 0;
  bool shadow_building_ = false;
  bool shadow_stale_ = false;
  std::uint64_t shadow_work_ = 0;
  std::uint64_t shadow_swaps_ = 0;
  std::uint64_t shadow_catchups_ = 0;
  std::uint64_t shadow_fallbacks_ = 0;
  std::uint64_t max_op_work_ = 0;
  std::uint64_t max_op_overhead_ = 0;
  std::uint64_t demotions_ = 0;
};

using ClassicSkipList = SkipList<ClassicPolicy>;
using GatedSkipList = SkipList<GatedPolicy>;
using ExactGateSkipList = SkipList<ExactGatePolicy>;
using MorrisGateSkipList = SkipList<MorrisGatePolicy>;
using ExactEstimatorSkipList = SkipList<ExactEstimatorPolicy>;
using MorrisEstimatorSkipList = SkipList<MorrisEstimatorPolicy>;
using MoveToTopLeakSkipList = SkipList<MoveToTopLeakPolicy>;
using GatedLeakSkipList = SkipList<GatedLeakPolicy>;

}  // namespace bsl
