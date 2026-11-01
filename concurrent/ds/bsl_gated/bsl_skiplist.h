// Concurrent self-adjusting skip list for the SetBench fork.
//
// Search, insert and erase are the fork's lock-based lazy skip list
// (ds/skiplist_locks, after Herlihy, Lev, Luchangco and Shavit): wait-free
// searches, per-node spin locks, deletion by marking a node and then
// unlinking it under its predecessors' locks. What is added is the paper's
// promotion rule (submission, Definition 1.1, "def:rule"), applied on a
// successful search:
//
//   read the item's published height H without locking; with probability
//   p^H draw J uniform in {1..K} and G ~ Geom_0(1-p) and raise the item to
//   H + J + G.  Insertion gives height J + G (so at least 1).  Heights never
//   decrease.
//
// A promotion is a level insertion of an existing node: lock the node, check
// that it is unmarked and still at height H, lock its predecessors at levels
// H..H'-1 bottom-up (the insert path's order), validate every one of them,
// link levels H..H'-1 bottom-up and publish the new height last. If anything
// fails validation the promotion is abandoned: heights are an estimate, so a
// lost promotion is allowed; a corrupt list is not.
//
// Deviations from the fork's skiplist_locks, all forced by heights that grow:
//   * erase reads the victim's height under the victim's lock, after marking
//     it (promotions also need that lock and check the mark, so the height is
//     then final), and no longer requires the search to have met the victim
//     at its top level; it requires fullyLinked instead (Herlihy's okToDelete
//     without the level test, which a growing node would fail spuriously);
//   * shared fields are std::atomic with release stores and acquire loads,
//     and the locks release with a release store. The fork relies on x86
//     ordering (plain stores, `*lock = false`); on aarch64 that is a data
//     race. On x86 the generated loads and stores are the same plain moves,
//     so this costs nothing where the paper's numbers are measured.
//
// Memory: every node preallocates its `next` array at the height cap
// BSL_MAX_HEIGHT, so a tower can grow under concurrent readers without being
// reallocated. With the default cap of 64 that is 512 bytes of pointers per
// node, against 208 for the fork's skip lists (cap 26). The paper's space
// bound is about the compressed representation; this is an engineering
// choice for the benchmark, not the representation the bound describes.
//
// Compile-time parameters (pass with -D, e.g. make xargs="-DBSL_K=2"):
//   BSL_P            list parameter p, 0 < p < 1         (default 0.5)
//   BSL_K            jump bound K >= 1                    (default 3)
//   BSL_MAX_HEIGHT   height cap, levels per node, <= 64   (default 64)
//   BSL_MAX_THREADS  largest thread id + 1                (default 1024)
//   BSL_TRACK_NODES  remember every node for validate()   (default: on
//                    when assertions are on, i.e. NDEBUG is not defined)
// Run time: the environment variable BSL_SEED (default 0) shifts every
// thread's generator seed (thread t uses 0xB51C0000 + t + (BSL_SEED << 20)).
//
//   BSL_EARLY_MISS   end an unsuccessful search as soon as the node it is
//                    about to descend from points to the same successor at
//                    the bottom level (default 1). Section 6 of the paper:
//                    the predecessor x and successor y of an absent key
//                    share their pointer on every level up to min(H_x, H_y),
//                    so a search that reaches x at or below that level can
//                    stop. One extra read of x's bottom pointer per descent
//                    on a miss; never triggers on a hit; the read is the one
//                    the plain search would make at level 0, so the answer
//                    and its linearization are unchanged.
//   BSL_EXIT_REPORT  print stats and run validate() at exit (default 1;
//                    the environment variable BSL_EXIT_REPORT=0 also
//                    disables it at run time)
//   BSL_STRESS_ALWAYS_PROMOTE  test only, not the paper's rule: the gated
//                    rule's gate opens on every hit, so every successful
//                    search attempts a promotion (until the cap). Used by
//                    the stress binary stress_bsl_gated_always.out to drive
//                    concurrent promotions as hard as possible.

#ifndef BSL_CONC_SKIPLIST_H
#define BSL_CONC_SKIPLIST_H

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef BSL_P
#define BSL_P 0.5
#endif
#ifndef BSL_K
#define BSL_K 3
#endif
#ifndef BSL_MAX_HEIGHT
#define BSL_MAX_HEIGHT 64
#endif
#ifndef BSL_MAX_THREADS
#define BSL_MAX_THREADS 1024
#endif
#ifndef BSL_EARLY_MISS
#define BSL_EARLY_MISS 1
#endif
#ifndef BSL_TRACK_NODES
#ifdef NDEBUG
#define BSL_TRACK_NODES 0
#else
#define BSL_TRACK_NODES 1
#endif
#endif

namespace bsl_conc {

const int kCap = BSL_MAX_HEIGHT;
const int kMaxThreads = BSL_MAX_THREADS;
const int kPad = 128;
static_assert(BSL_MAX_HEIGHT >= 2 && BSL_MAX_HEIGHT <= 64, "BSL_MAX_HEIGHT must be in [2, 64]");
static_assert(BSL_K >= 1, "BSL_K must be at least 1");

inline void spinHint() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

// Test-and-test-and-set lock, acquire on lock and release on unlock.
struct SpinLock {
    std::atomic<int> word;
    void init() { word.store(0, std::memory_order_relaxed); }
    void lock() {
        for (;;) {
            if (word.load(std::memory_order_relaxed)) { spinHint(); continue; }
            int expected = 0;
            if (word.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                           std::memory_order_relaxed))
                return;
        }
    }
    void unlock() { word.store(0, std::memory_order_release); }
};

// xorshift64*, seeded through splitmix64. One per thread, written only by
// its owner.
struct ThreadRng {
    std::uint64_t s;
    void seed(std::uint64_t x) {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        x = x ^ (x >> 31);
        s = x ? x : 0x9E3779B97F4A7C15ULL;
    }
    std::uint64_t next() {
        std::uint64_t x = s;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        s = x;
        return x * 0x2545F4914F6CDD1DULL;
    }
    // Uniform in [0, 1).
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
};

// The laws the rules draw from, tabulated once per structure (read-only
// after construction).
struct Law {
    double p;
    int K;
    double invLogP;                 // 1 / log p
    std::uint64_t gate[kCap + 1];   // gate[h]: open iff a uniform 64-bit word < gate[h]; P = p^h
    double powB[kCap + 2];          // b^r with b = 1/p, for the exact estimator's rank

    void init(double p_, int K_) {
        p = p_;
        K = K_;
        invLogP = 1.0 / std::log(p);
        for (int h = 0; h <= kCap; ++h) {
            const double v = std::ldexp(std::pow(p, h), 64);
            gate[h] = (v >= 18446744073709551615.0) ? ~0ULL : static_cast<std::uint64_t>(v);
        }
        for (int r = 0; r < kCap + 2; ++r) powB[r] = std::pow(1.0 / p, r);
    }
};

// What a rule sees: the tabulated law and the calling thread's generator.
struct Draws {
    const Law& law;
    ThreadRng& rng;
    Draws(const Law& l, ThreadRng& r) : law(l), rng(r) {}

    // True with probability p^h.
    bool gate(int h) {
        if (h <= 0) return true;
        if (h >= kCap) return false;
        return rng.next() < law.gate[h];
    }
    // G ~ Geom_0(1-p): P[G >= g] = p^g. Inversion, clamped to the cap.
    int geom0() {
        double u = rng.uniform();
        if (u <= 0.0) u = 1.0 / 9007199254740992.0;
        const double g = std::log(u) * law.invLogP;
        return g >= kCap ? kCap : static_cast<int>(g);
    }
    // J uniform on {1..K}.
    int jump() { return law.K == 1 ? 1 : 1 + static_cast<int>(rng.next() % static_cast<std::uint64_t>(law.K)); }
    // floor(log_b f), with the sequential harness's tolerance for exact powers.
    int rank(std::uint64_t f) {
        const double fd = static_cast<double>(f) * (1.0 + 1e-12);
        int r = 0;
        while (r + 1 < kCap + 2 && law.powB[r + 1] <= fd) ++r;
        return r;
    }
};

// ---------------------------------------------------------------------------
// Rules. Interface:
//   struct State { void init(); };                 per-node state
//   static const char* name();
//   template <class D> static int insertHeight(D&); height at insertion, >= 1
//   template <class D> static int onHit(State&, int H, D&);
//       called on a successful search of a live node whose published height
//       is H; returns the requested height (> H) or 0 for no change.
//   static const bool kReadOnlyMiss;  true if a call that requests nothing
//       writes no shared memory
//   template <class D> static bool invariant(State&, int H, D&);
//       the rule's height invariant at quiescence (true if it has none);
//       validate() counts nodes violating it. A promotion lost to contention
//       may leave a violation until the item's next access, so validate()
//       fails on violations only when no promotion was abandoned.
// ---------------------------------------------------------------------------

// The paper's rule, Definition 1.1 (experiments/include/bsl/skiplist.hpp,
// GatedPolicy). No per-node state.
struct GatedRule {
    struct State { void init() {} };
    static const char* name() { return "gated"; }
    static const bool kUsesK = true;
    static const bool kReadOnlyMiss = true;
    template <class D> static int insertHeight(D& d) { return d.jump() + d.geom0(); }
    template <class D> static bool invariant(State&, int, D&) { return true; }
    template <class D> static int onHit(State&, int h, D& d) {
#ifndef BSL_STRESS_ALWAYS_PROMOTE
        if (!d.gate(h)) return 0;
#endif
        return h + d.jump() + d.geom0();
    }
};

// Control arm: the classic skip list (height 1 + G at insert, never changed)
// on exactly the same code, so path lengths compare like with like.
struct ClassicRule {
    struct State { void init() {} };
    static const char* name() { return "classic"; }
    static const bool kUsesK = false;
    static const bool kReadOnlyMiss = true;
    template <class D> static int insertHeight(D& d) { return 1 + d.geom0(); }
    template <class D> static int onHit(State&, int, D&) { return 0; }
    template <class D> static bool invariant(State&, int, D&) { return true; }
};

// The paper's exact estimator (skiplist.hpp, ExactEstimatorPolicy): f counts
// operations on the item, insertion included; after the increment, if
// H <= rank = floor(log_b f), redraw H <- rank + G. Every hit writes f.
// A lost promotion heals itself: H <= rank still holds at the next hit.
struct ExactRule {
    struct State {
        std::atomic<std::uint64_t> f;
        void init() { f.store(1, std::memory_order_relaxed); }
    };
    static const char* name() { return "exact"; }
    static const bool kUsesK = false;
    static const bool kReadOnlyMiss = false;
    template <class D> static int insertHeight(D& d) { return 1 + d.geom0(); }
    template <class D> static int onHit(State& s, int h, D& d) {
        const std::uint64_t f = s.f.fetch_add(1, std::memory_order_relaxed) + 1;
        const int r = d.rank(f);
        if (h > r) return 0;
        return r + d.geom0();
    }
    // H >= rank (Appendix A): after every access the height is at least
    // floor(log_b f).
    template <class D> static bool invariant(State& s, int h, D& d) {
        const int r = d.rank(s.f.load());
        return h >= std::min(r, kCap);
    }
};

// The paper's Morris estimator (skiplist.hpp, MorrisEstimatorPolicy): a
// counter Q, starting at 1, incremented with probability p^Q; if it is
// incremented and H = Q before the increment, H rises by 1 + G. Sequentially
// H >= Q always, so "H = Q" is the same as "H <= Q"; the concurrent version
// tests H <= Q and raises to Q + 1 + G, which is identical sequentially and
// lets a promotion lost to contention be made good at the next increment
// instead of leaving H below Q for good. The increment is a CAS; a thread
// that loses the CAS treats its access as not incrementing.
struct MorrisRule {
    struct State {
        std::atomic<std::uint32_t> Q;
        void init() { Q.store(1, std::memory_order_relaxed); }
    };
    static const char* name() { return "morris"; }
    static const bool kUsesK = false;
    static const bool kReadOnlyMiss = true;   // a failed coin writes nothing
    template <class D> static int insertHeight(D& d) { return 1 + d.geom0(); }
    template <class D> static int onHit(State& s, int h, D& d) {
        std::uint32_t q = s.Q.load(std::memory_order_relaxed);
        if (!d.gate(static_cast<int>(q))) return 0;
        if (!s.Q.compare_exchange_strong(q, q + 1, std::memory_order_relaxed)) return 0;
        if (h > static_cast<int>(q)) return 0;
        return static_cast<int>(q) + 1 + d.geom0();
    }
    // H >= Q (Appendix A).
    template <class D> static bool invariant(State& s, int h, D&) {
        return h >= std::min(static_cast<int>(s.Q.load()), kCap);
    }
};

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------

template <typename K, typename V, class Rule>
struct Node {
    K key;                          // immutable after creation
    V value;                        // immutable after creation
    SpinLock lock;
    std::atomic<int> height;        // published height: levels 0..height-1 are linked
    std::atomic<bool> marked;       // logically deleted
    std::atomic<bool> fullyLinked;  // insertion complete (its linearization point)
    typename Rule::State state;
    std::atomic<Node*> next[kCap];
};

struct alignas(16) ThreadStats {
    long long pathLen;     // the fork's metric: levels visited + forward steps, searches only
    long long levelsDown;  // of pathLen, the levels visited (the vertical span); the rest are forward steps
    long long misses;      // searches that found no item
    long long earlyMiss;   // misses ended above level 0 by the BSL_EARLY_MISS test
    long long gateOpens;   // promotions requested by the rule
    long long promoted;    // promotions completed
    long long aborted;     // promotions abandoned (lost to a concurrent update)
    long long levelsAdded; // sum over completed promotions of H' - H
};

// ---------------------------------------------------------------------------
// The structure
// ---------------------------------------------------------------------------

template <typename K, typename V, class RecordManager, class Rule>
class SkipList {
public:
    typedef Node<K, V, Rule> NodeT;

private:
    struct PerThread {
        ThreadStats st;
        ThreadRng rng;
#if BSL_TRACK_NODES
        std::vector<NodeT*>* created;
#endif
        volatile char pad[kPad];
    };

    volatile char pad0[kPad];
    std::atomic<int> hmax_;   // H_max: number of levels in use, the only global the rule needs
    volatile char pad1[kPad];
    NodeT* head_;
    NodeT* tail_;
    RecordManager* const rm_;
    V noValue_;
    Law law_;
    PerThread* pt_;
    int init_[kMaxThreads];
    volatile char pad2[kPad];

public:
    SkipList(const int numThreads, const V noValue, const K minKey, const K maxKey)
        : rm_(new RecordManager(numThreads)) {
        for (int i = 0; i < kMaxThreads; ++i) init_[i] = 0;
        pt_ = new PerThread[kMaxThreads];
        const char* seedEnv = std::getenv("BSL_SEED");
        seed_ = seedEnv ? std::strtoull(seedEnv, nullptr, 10) : 0;
        for (int i = 0; i < kMaxThreads; ++i) {
            pt_[i].st = ThreadStats();
            pt_[i].rng.seed(0xB51C0000ULL + static_cast<std::uint64_t>(i) + (seed_ << 20));
#if BSL_TRACK_NODES
            pt_[i].created = new std::vector<NodeT*>();
#endif
        }
        law_.init(BSL_P, BSL_K);
        noValue_ = noValue;
        const int tid = 0;
        initThread(tid);
        rm_->endOp(tid);
        head_ = createNode(tid, kCap, minKey - 1, noValue);
        tail_ = createNode(tid, kCap, maxKey + 1, noValue);
        for (int i = 0; i < kCap; ++i) {
            head_->next[i].store(tail_, std::memory_order_relaxed);
            tail_->next[i].store(nullptr, std::memory_order_relaxed);
        }
        head_->fullyLinked.store(true, std::memory_order_relaxed);
        tail_->fullyLinked.store(true, std::memory_order_relaxed);
        hmax_.store(1, std::memory_order_release);
    }

    ~SkipList() {
        rm_->printStatus();
        delete rm_;
#if BSL_TRACK_NODES
        for (int i = 0; i < kMaxThreads; ++i) delete pt_[i].created;
#endif
        delete[] pt_;
    }

    // --- searches -----------------------------------------------------------

    bool contains(const int tid, const K& key) { return lookup(tid, key) != nullptr; }

    V find(const int tid, const K& key) {
        NodeT* n = lookup(tid, key);
        return n ? n->value : noValue_;
    }

    // --- updates ------------------------------------------------------------

    V insertIfAbsent(const int tid, const K& key, const V& value) {
        assert(tid >= 0 && tid < kMaxThreads);
        PerThread& me = pt_[tid];
        Draws d(law_, me.rng);
        const int h = clampHeight(Rule::insertHeight(d));
        NodeT* preds[kCap];
        NodeT* succs[kCap];
        for (;;) {
            int covered = 0;
            const int lFound = locate(key, preds, succs, h, &covered);
            if (lFound != -1) {
                NodeT* found = succs[lFound];
                if (!found->marked.load(std::memory_order_acquire)) {
                    while (!found->fullyLinked.load(std::memory_order_acquire)) spinHint();
                    return found->value;
                }
                continue;  // being erased: wait for it to leave
            }
            int lockLevel = -1;
            bool valid = true;
            for (int level = 0; valid && level < h; ++level) {
                NodeT* pred = preds[level];
                NodeT* succ = succs[level];
                if (level == 0 || pred != preds[level - 1]) pred->lock.lock();
                lockLevel = level;
                valid = !pred->marked.load(std::memory_order_relaxed) &&
                        !succ->marked.load(std::memory_order_relaxed) &&
                        pred->next[level].load(std::memory_order_relaxed) == succ;
            }
            if (!valid) {
                unlockPreds(preds, 0, lockLevel);
                continue;
            }
            NodeT* n = createNode(tid, h, key, value);
            for (int level = 0; level < h; ++level)
                n->next[level].store(succs[level], std::memory_order_relaxed);
            for (int level = 0; level < h; ++level)
                preds[level]->next[level].store(n, std::memory_order_release);
            n->fullyLinked.store(true, std::memory_order_release);
            raiseHmax(h);
            unlockPreds(preds, 0, lockLevel);
#if BSL_TRACK_NODES
            me.created->push_back(n);
#endif
            return noValue_;
        }
    }

    V erase(const int tid, const K& key) {
        assert(tid >= 0 && tid < kMaxThreads);
        NodeT* preds[kCap];
        NodeT* succs[kCap];
        NodeT* victim = nullptr;
        bool isMarked = false;
        int h = 0;
        for (;;) {
            int covered = 0;
            const int lFound = locate(key, preds, succs, h, &covered);
            if (!isMarked) {
                if (lFound == -1) return noValue_;
                victim = succs[lFound];
                if (!victim->fullyLinked.load(std::memory_order_acquire) ||
                    victim->marked.load(std::memory_order_acquire))
                    return noValue_;
                victim->lock.lock();
                if (victim->marked.load(std::memory_order_relaxed)) {
                    victim->lock.unlock();
                    return noValue_;
                }
                victim->marked.store(true, std::memory_order_release);
                isMarked = true;
                // Final: promotions take this lock and give up on a marked node.
                h = victim->height.load(std::memory_order_relaxed);
                if (covered < h) continue;  // locate again over all h levels
            }
            int lockLevel = -1;
            bool valid = true;
            for (int level = 0; valid && level < h; ++level) {
                NodeT* pred = preds[level];
                if (level == 0 || pred != preds[level - 1]) pred->lock.lock();
                lockLevel = level;
                valid = !pred->marked.load(std::memory_order_relaxed) &&
                        pred->next[level].load(std::memory_order_relaxed) == victim;
            }
            if (!valid) {
                unlockPreds(preds, 0, lockLevel);
                continue;
            }
            for (int level = h - 1; level >= 0; --level)
                preds[level]->next[level].store(victim->next[level].load(std::memory_order_relaxed),
                                                std::memory_order_release);
            unlockPreds(preds, 0, lockLevel);
            victim->lock.unlock();
            const V value = victim->value;
            rm_->retire(tid, victim);
            return value;
        }
    }

    // --- validation (quiescent only) ----------------------------------------

    // Checks: level 0 strictly sorted; every level-i list strictly sorted and
    // a subsequence of level i-1; no reachable node is marked or not fully
    // linked; every node is linked at exactly the levels 0..height-1 its
    // published height says; nothing is linked at or above H_max; and, with
    // BSL_TRACK_NODES, every node ever inserted is reachable iff unmarked.
    bool validate(bool verbose = true) {
        std::unordered_map<NodeT*, int> seen;  // node -> number of levels it was met at
        long long size = 0, keySum = 0;
        int maxHeight = 0;
        const int hm = hmax_.load(std::memory_order_acquire);
        std::vector<long long> perLevel(kCap, 0);
        for (int level = 0; level < kCap; ++level) {
            NodeT* prev = head_;
            NodeT* cur = head_->next[level].load(std::memory_order_acquire);
            while (cur != tail_) {
                if (cur == nullptr) return fail(verbose, "null next pointer", level, prev);
                if (!(cur->key > prev->key)) return fail(verbose, "level not strictly sorted", level, cur);
                if (cur->marked.load()) return fail(verbose, "marked node reachable", level, cur);
                if (!cur->fullyLinked.load()) return fail(verbose, "node not fully linked", level, cur);
                const int h = cur->height.load();
                if (h < 1 || h > kCap) return fail(verbose, "height out of range", level, cur);
                if (h <= level) return fail(verbose, "linked above its published height", level, cur);
                if (level >= hm) return fail(verbose, "linked at or above H_max", level, cur);
                if (level == 0) {
                    if (seen.count(cur)) return fail(verbose, "node met twice on level 0", level, cur);
                    seen[cur] = 1;
                    ++size;
                    keySum += static_cast<long long>(cur->key);
                    maxHeight = std::max(maxHeight, h);
                } else {
                    typename std::unordered_map<NodeT*, int>::iterator it = seen.find(cur);
                    if (it == seen.end() || it->second != level)
                        return fail(verbose, "level is not a subsequence of the level below", level, cur);
                    it->second = level + 1;
                }
                ++perLevel[level];
                prev = cur;
                cur = cur->next[level].load(std::memory_order_acquire);
            }
        }
        long long violations = 0;
        ThreadRng scratch;
        scratch.seed(0);
        Draws dv(law_, scratch);
        for (typename std::unordered_map<NodeT*, int>::iterator it = seen.begin(); it != seen.end(); ++it) {
            if (it->second != it->first->height.load())
                return fail(verbose, "published height differs from linked levels", it->second, it->first);
            if (!Rule::invariant(it->first->state, it->first->height.load(), dv)) ++violations;
        }
        lastViolations_ = violations;
        if (violations > 0 && totals().aborted == 0)
            return fail(verbose, "rule invariant violated with no promotion abandoned", 0, nullptr);
        if (maxHeight > hm) return fail(verbose, "a height exceeds H_max", maxHeight, nullptr);
        for (int i = 0; i < kCap; ++i)
            if (tail_->next[i].load() != nullptr) return fail(verbose, "tail has a successor", i, tail_);
#if BSL_TRACK_NODES
        long long tracked = 0, trackedLive = 0;
        for (int t = 0; t < kMaxThreads; ++t) {
            for (size_t j = 0; j < pt_[t].created->size(); ++j) {
                NodeT* n = (*pt_[t].created)[j];
                ++tracked;
                const bool live = !n->marked.load();
                const bool reachable = seen.count(n) != 0;
                if (live) ++trackedLive;
                if (live && !reachable) return fail(verbose, "unmarked node unreachable", 0, n);
                if (!live && reachable) return fail(verbose, "marked node reachable", 0, n);
            }
        }
        if (trackedLive != size) return fail(verbose, "reachable nodes not all tracked", 0, nullptr);
#endif
        lastSize_ = size;
        lastKeySum_ = keySum;
        if (verbose) {
            std::cout << "bsl_validate=OK rule=" << Rule::name() << " size=" << size
                      << " keysum=" << keySum << " hmax=" << hm << " maxheight=" << maxHeight
                      << " invariant_violations=" << violations;
#if BSL_TRACK_NODES
            std::cout << " tracked=" << tracked;
#endif
            std::cout << " levels=";
            for (int i = 0; i < hm; ++i) std::cout << (i ? "," : "") << perLevel[i];
            std::cout << std::endl;
        }
        return true;
    }

    long long lastSize() const { return lastSize_; }
    long long lastKeySum() const { return lastKeySum_; }

    ThreadStats totals() const {
        ThreadStats s = ThreadStats();
        for (int i = 0; i < kMaxThreads; ++i) {
            s.pathLen += pt_[i].st.pathLen;
            s.levelsDown += pt_[i].st.levelsDown;
            s.misses += pt_[i].st.misses;
            s.earlyMiss += pt_[i].st.earlyMiss;
            s.gateOpens += pt_[i].st.gateOpens;
            s.promoted += pt_[i].st.promoted;
            s.aborted += pt_[i].st.aborted;
            s.levelsAdded += pt_[i].st.levelsAdded;
        }
        return s;
    }

    void printStats(std::ostream& os) const {
        const ThreadStats s = totals();
        os << "bsl_stats rule=" << Rule::name() << " p=" << law_.p;
        if (Rule::kUsesK) os << " K=" << law_.K;  // the counter rules jump by 1 + G
        os << " seed=" << seed_ << " cap=" << kCap << " node_bytes=" << sizeof(NodeT)
           << " hmax=" << hmax_.load()
           << " path_total=" << s.pathLen << " path_levels=" << s.levelsDown
           << " path_steps=" << (s.pathLen - s.levelsDown)
           << " misses=" << s.misses << " early_miss=" << s.earlyMiss
           << " early_miss_on=" << BSL_EARLY_MISS
           << " gate_opens=" << s.gateOpens
           << " promotions=" << s.promoted << " aborted=" << s.aborted
           << " levels_added=" << s.levelsAdded << std::endl;
    }

    // --- SetBench plumbing --------------------------------------------------

    void initThread(const int tid) {
        if (init_[tid]) return;
        init_[tid] = 1;
        rm_->initThread(tid);
    }
    void deinitThread(const int tid) {
        if (!init_[tid]) return;
        init_[tid] = 0;
        rm_->deinitThread(tid);
    }
    long long getPathsLength(const int tid) { return pt_[tid].st.pathLen; }
    int getHeight() { return hmax_.load(std::memory_order_relaxed); }
    NodeT* getRoot() { return head_->next[0].load(); }
    NodeT* getHead() { return head_; }
    NodeT* getTail() { return tail_; }

    void printDebuggingDetails() {
        for (int level = 0; level < hmax_.load(); ++level) {
            std::cout << level << ": ";
            for (NodeT* n = head_->next[level].load(); n != tail_; n = n->next[level].load())
                std::cout << n->key << " ";
            std::cout << "\n";
        }
    }

private:
    std::uint64_t seed_ = 0;
    long long lastViolations_ = 0;
    long long lastSize_ = 0;
    long long lastKeySum_ = 0;

    static int clampHeight(int h) { return h < 1 ? 1 : (h > kCap ? kCap : h); }

    NodeT* createNode(const int tid, int h, const K& key, const V& value) {
        NodeT* n = rm_->template allocate<NodeT>(tid);
        n->key = key;
        n->value = value;
        n->lock.init();
        n->height.store(h, std::memory_order_relaxed);
        n->marked.store(false, std::memory_order_relaxed);
        n->fullyLinked.store(false, std::memory_order_relaxed);
        n->state.init();
        for (int i = 0; i < kCap; ++i) n->next[i].store(nullptr, std::memory_order_relaxed);
        return n;
    }

    // The search path every search takes. Read-only on shared memory: the
    // only stores are to the caller's thread-private stats slot (pathLen) and,
    // inside the rule, to the caller's thread-private generator, unless the
    // rule requests a promotion. The loop is the fork's skiplist_locks
    // contains(), so the path length counts the same things: one per level
    // visited plus one per forward step, and a hit stops at the level where
    // the key is first met (the item's top level).
    NodeT* lookup(const int tid, const K& key) {
        assert(tid >= 0 && tid < kMaxThreads);
        PerThread& me = pt_[tid];
        NodeT* pred = head_;
        NodeT* hit = nullptr;
        long long steps = 0;   // the fork's count: the inner loop runs at least once per level,
        long long levels = 0;  // so steps = levels visited + forward steps
        bool early = false;
        for (int level = hmax_.load(std::memory_order_relaxed) - 1; level >= 0; --level) {
            ++levels;
            NodeT* curr = pred;
            while (key > curr->key) {
                ++steps;
                pred = curr;
                curr = pred->next[level].load(std::memory_order_acquire);
            }
            if (key == curr->key) { hit = curr; break; }
#if BSL_EARLY_MISS
            // pred < key < curr at this level. If pred also points to curr at
            // the bottom level, the two are adjacent there and every level
            // between points to curr: the key is absent, and this is the read
            // the descent would have ended on.
            if (level > 0 && pred->next[0].load(std::memory_order_acquire) == curr) { early = true; break; }
#endif
        }
        me.st.pathLen += steps;
        me.st.levelsDown += levels;
        if (hit == nullptr) {
            ++me.st.misses;
            if (early) ++me.st.earlyMiss;
            return nullptr;
        }
        if (!hit->fullyLinked.load(std::memory_order_acquire) ||
            hit->marked.load(std::memory_order_acquire))
            return nullptr;
        const int h = hit->height.load(std::memory_order_acquire);
        Draws d(law_, me.rng);
        int target = Rule::onHit(hit->state, h, d);
        if (target > kCap) target = kCap;
        if (target > h) {
            ++me.st.gateOpens;
            promote(tid, hit, h, target);
        }
        return hit;
    }

    // Level insertion of an existing node from height h to height target.
    // Out of line: it is the cold path, and keeping it separate keeps the
    // search's machine code exactly the read path (see the store audit in
    // RESULTS-local.md).
    __attribute__((noinline)) void promote(const int tid, NodeT* n, const int h, const int target) {
        PerThread& me = pt_[tid];
        NodeT* preds[kCap];
        NodeT* succs[kCap];
        int covered = 0;
        locate(n->key, preds, succs, target, &covered);
        n->lock.lock();
        if (n->marked.load(std::memory_order_relaxed) ||
            n->height.load(std::memory_order_relaxed) != h) {
            n->lock.unlock();
            ++me.st.aborted;
            return;
        }
        int lockLevel = h - 1;
        bool valid = true;
        for (int level = h; valid && level < target; ++level) {
            NodeT* pred = preds[level];
            NodeT* succ = succs[level];
            if (level == h || pred != preds[level - 1]) pred->lock.lock();
            lockLevel = level;
            valid = succ != n &&
                    !pred->marked.load(std::memory_order_relaxed) &&
                    !succ->marked.load(std::memory_order_relaxed) &&
                    pred->next[level].load(std::memory_order_relaxed) == succ;
        }
        if (valid) {
            for (int level = h; level < target; ++level) {
                assert(preds[level]->key < n->key && succs[level]->key > n->key);
                n->next[level].store(succs[level], std::memory_order_relaxed);
                preds[level]->next[level].store(n, std::memory_order_release);
            }
            n->height.store(target, std::memory_order_release);
            raiseHmax(target);
            ++me.st.promoted;
            me.st.levelsAdded += target - h;
        } else {
            ++me.st.aborted;
        }
        unlockPreds(preds, h, lockLevel);
        n->lock.unlock();
    }

    // Records the predecessor and successor of `key` at every level below
    // max(H_max, needed). Returns the highest level at which a node with the
    // key was met, or -1; *covered receives the number of levels recorded.
    int locate(const K& key, NodeT** preds, NodeT** succs, int needed, int* covered) {
        int top = std::max(hmax_.load(std::memory_order_acquire), needed);
        if (top > kCap) top = kCap;
        *covered = top;
        int lFound = -1;
        NodeT* pred = head_;
        for (int level = top - 1; level >= 0; --level) {
            NodeT* curr = pred->next[level].load(std::memory_order_acquire);
            while (key > curr->key) {
                pred = curr;
                curr = pred->next[level].load(std::memory_order_acquire);
            }
            if (lFound == -1 && key == curr->key) lFound = level;
            preds[level] = pred;
            succs[level] = curr;
        }
        return lFound;
    }

    static void unlockPreds(NodeT** preds, int from, int to) {
        for (int level = from; level <= to; ++level)
            if (level == from || preds[level] != preds[level - 1]) preds[level]->lock.unlock();
    }

    void raiseHmax(int h) {
        int cur = hmax_.load(std::memory_order_relaxed);
        while (h > cur && !hmax_.compare_exchange_weak(cur, h, std::memory_order_release,
                                                        std::memory_order_relaxed)) {
        }
    }

    bool fail(bool verbose, const char* what, int level, NodeT* n) {
        if (verbose) {
            std::cout << "bsl_validate=FAIL rule=" << Rule::name() << " reason=\"" << what
                      << "\" level=" << level;
            if (n) std::cout << " key=" << n->key << " height=" << n->height.load()
                             << " marked=" << n->marked.load();
            std::cout << std::endl;
        }
        return false;
    }
};

}  // namespace bsl_conc

#endif  // BSL_CONC_SKIPLIST_H
