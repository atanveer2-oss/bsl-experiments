// Concurrent stress test for any SetBench adapter (ds/<name>/adapter.h).
//
// The fork's benchmark.cpp and benchmark_write.cpp check nothing after a
// run, so this driver supplies the check SetBench's upstream main.cpp makes
// (thread key sums against the structure's) and a stronger one:
//
//   * every thread records, per key, its successful inserts minus its
//     successful erases; after the run, prefill + the sum over threads must
//     be 0 or 1 for every key, and must equal what contains() reports;
//   * so the key sum and the size match as well (printed);
//   * values returned by insertIfAbsent (key present) and erase must be the
//     value stored with that key;
//   * then the adapter's validateStructure() must pass.
//
// Usage (arguments are name/value pairs, all optional):
//   stress threads 6 secs 10 keys 10000 ins 25 del 25 dist uniform|zipf|hot
//          alpha 1.0 hotops 90 hotkeys 10 prefill 0.5 seed 1 rounds 1
// ins/del are percentages of operations (fractions allowed); the rest are
// contains(). With
// dist hot, `hotops`% of operations go to `hotkeys`% of the keys. With
// rounds R > 1 the run is split into R rounds of `secs` seconds each, with a
// full check after every round (the structure persists across rounds).
// insrate R caps each thread's successful inserts, and its successful erases,
// at R per second each (an update over that pace becomes a contains), so
// memory stays bounded however fast the run goes (under reclaimer_none, the
// fork's setting, erased nodes are never freed) and the set's size does a
// random walk around the prefill instead of draining.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "globals_extern.h"
#include "random_fnv1a.h"
#include "plaf.h"
#include "adapter.h"

#define KEY_TYPE long long
#define VALUE_TYPE void*
typedef ds_adapter<KEY_TYPE, VALUE_TYPE, RECLAIM<>, ALLOC<>, POOL<> > Adapter;

#define STR2(x) #x
#define STR(x) STR2(x)

static const int kMaxStressThreads = 64;

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) {
        std::uint64_t x = seed + 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        s = (x ^ (x >> 31)) | 1;
    }
    std::uint64_t next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 0x2545F4914F6CDD1DULL;
    }
    double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

struct Config {
    int threads = 4;
    int secs = 5;
    int keys = 10000;
    double ins = 25;   // percent of operations, fractions allowed (0.5 = one in 200)
    double del = 25;
    std::string dist = "uniform";
    double alpha = 1.0;
    int hotops = 90;
    int hotkeys = 10;
    double prefill = 0.5;
    std::uint64_t seed = 1;
    int rounds = 1;
    double insrate = 0;  // max successful inserts per second per thread; 0 = unlimited
};

struct KeyChooser {
    const Config& cfg;
    std::vector<long long> perm;   // rank -> key, keys are 1..keys
    std::vector<double> cdf;       // Zipf CDF over ranks
    int nHot;
    explicit KeyChooser(const Config& c) : cfg(c) {
        perm.resize(c.keys);
        for (int i = 0; i < c.keys; ++i) perm[i] = i + 1;
        Rng r(c.seed * 7919 + 17);
        for (int i = c.keys - 1; i > 0; --i) std::swap(perm[i], perm[r.next() % (i + 1)]);
        nHot = std::max(1, (int)((long long)c.keys * c.hotkeys / 100));
        if (c.dist == "zipf") {
            cdf.resize(c.keys);
            double s = 0;
            for (int i = 0; i < c.keys; ++i) { s += 1.0 / std::pow(i + 1.0, c.alpha); cdf[i] = s; }
            for (int i = 0; i < c.keys; ++i) cdf[i] /= s;
        }
    }
    long long pick(Rng& r) const {
        if (cfg.dist == "zipf") {
            const double u = r.uniform();
            int i = (int)(std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin());
            if (i >= cfg.keys) i = cfg.keys - 1;
            return perm[i];
        }
        if (cfg.dist == "hot") {
            if ((int)(r.next() % 100) < cfg.hotops) return perm[r.next() % nHot];
            return perm[nHot + r.next() % (cfg.keys - nHot)];
        }
        return perm[r.next() % cfg.keys];
    }
};

// Resident set size in MiB (Linux), to show the long runs stay bounded:
// with reclaimer_none erased nodes are never freed, as in the fork.
static long rssMiB() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "VmRSS:", 6) == 0) { kb = atol(line + 6); break; }
    fclose(f);
    return kb < 0 ? -1 : kb / 1024;
}

static inline VALUE_TYPE valueOf(long long k) { return (VALUE_TYPE)(intptr_t)k; }

struct alignas(128) ThreadResult {
    long long ops = 0, ins = 0, insOk = 0, del = 0, delOk = 0, finds = 0, hits = 0, badValues = 0;
    std::vector<int> net;
};

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "threads") cfg.threads = atoi(v);
        else if (k == "secs") cfg.secs = atoi(v);
        else if (k == "keys") cfg.keys = atoi(v);
        else if (k == "ins") cfg.ins = atof(v);
        else if (k == "del") cfg.del = atof(v);
        else if (k == "dist") cfg.dist = v;
        else if (k == "alpha") cfg.alpha = atof(v);
        else if (k == "hotops") cfg.hotops = atoi(v);
        else if (k == "hotkeys") cfg.hotkeys = atoi(v);
        else if (k == "prefill") cfg.prefill = atof(v);
        else if (k == "seed") cfg.seed = strtoull(v, nullptr, 10);
        else if (k == "rounds") cfg.rounds = atoi(v);
        else if (k == "insrate") cfg.insrate = atof(v);
        else { std::cerr << "bad argument " << k << std::endl; return 2; }
    }
    if (cfg.threads < 1 || cfg.threads >= kMaxStressThreads) { std::cerr << "threads out of range\n"; return 2; }

    std::cout << "stress ds=" << STR(DS_TYPENAME) << " threads=" << cfg.threads << " secs=" << cfg.secs
              << " rounds=" << cfg.rounds << " keys=" << cfg.keys << " ins=" << cfg.ins << " del=" << cfg.del
              << " dist=" << cfg.dist;
    if (cfg.dist == "zipf") std::cout << " alpha=" << cfg.alpha;
    if (cfg.dist == "hot") std::cout << " hotops=" << cfg.hotops << " hotkeys=" << cfg.hotkeys;
    std::cout << " prefill=" << cfg.prefill << " seed=" << cfg.seed << " insrate=" << cfg.insrate << std::endl;

    Adapter* ds = new Adapter(kMaxStressThreads, 0, cfg.keys + 1, (VALUE_TYPE)NULL, nullptr);
    KeyChooser chooser(cfg);

    // Prefill from thread 0, in random order.
    std::vector<int> expected(cfg.keys + 1, 0);
    {
        Rng r(cfg.seed * 31 + 5);
        ds->initThread(0);
        for (int i = 0; i < cfg.keys; ++i) {
            const long long k = chooser.perm[i];
            if (r.uniform() < cfg.prefill) {
                if (ds->insertIfAbsent(0, k, valueOf(k)) != NULL) { std::cout << "FAIL prefill insert reported present\n"; return 1; }
                expected[k] = 1;
            }
        }
    }

    bool allOk = true;
    long long totalOps = 0;
    for (int round = 1; round <= cfg.rounds; ++round) {
        std::vector<ThreadResult> res(cfg.threads + 1);
        for (int t = 1; t <= cfg.threads; ++t) res[t].net.assign(cfg.keys + 1, 0);
        std::atomic<bool> stop(false);
        std::atomic<int> ready(0);
        std::vector<std::thread> th;
        for (int t = 1; t <= cfg.threads; ++t) {
            th.emplace_back([&, t]() {
                ds->initThread(t);
                Rng r(cfg.seed * 1000003ULL + t * 7777ULL + round * 131ULL);
                ThreadResult& me = res[t];
                ready.fetch_add(1);
                while (ready.load() < cfg.threads) {}
                const auto start = std::chrono::steady_clock::now();
                double insAllowed = 1e300;  // successful inserts allowed so far (insrate pacing)
                for (long long i = 0;; ++i) {
                    if ((i & 255) == 0) {
                        if (stop.load(std::memory_order_relaxed)) break;
                        if (cfg.insrate > 0)
                            insAllowed = cfg.insrate * std::chrono::duration<double>(
                                             std::chrono::steady_clock::now() - start).count() + 1;
                    }
                    const long long k = chooser.pick(r);
                    const double op = (double)(r.next() % 100000) / 1000.0;  // [0, 100)
                    const bool overPace = (op < cfg.ins && me.insOk >= insAllowed) ||
                                          (op >= cfg.ins && op < cfg.ins + cfg.del && me.delOk >= insAllowed);
                    if (overPace) {
                        ++me.finds;  // over the update pace: this operation becomes a search
                        if (ds->contains(t, k)) ++me.hits;
                    } else if (op < cfg.ins) {
                        ++me.ins;
                        VALUE_TYPE v = ds->insertIfAbsent(t, k, valueOf(k));
                        if (v == NULL) { ++me.insOk; ++me.net[k]; }
                        else if (v != valueOf(k)) ++me.badValues;
                    } else if (op < cfg.ins + cfg.del) {
                        ++me.del;
                        VALUE_TYPE v = ds->erase(t, k);
                        if (v != NULL) {
                            ++me.delOk; --me.net[k];
                            if (v != valueOf(k)) ++me.badValues;
                        }
                    } else {
                        ++me.finds;
                        if (ds->contains(t, k)) ++me.hits;
                    }
                    ++me.ops;
                }
            });
        }
        while (ready.load() < cfg.threads) {}
        const auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(cfg.secs));
        stop.store(true);
        for (auto& x : th) x.join();
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        ThreadResult sum;
        for (int t = 1; t <= cfg.threads; ++t) {
            sum.ops += res[t].ops; sum.ins += res[t].ins; sum.insOk += res[t].insOk;
            sum.del += res[t].del; sum.delOk += res[t].delOk; sum.finds += res[t].finds;
            sum.hits += res[t].hits; sum.badValues += res[t].badValues;
        }
        totalOps += sum.ops;
        // Per-key accounting.
        long long badKeys = 0, mismatches = 0, expSize = 0, expSum = 0, dsSize = 0, dsSum = 0;
        for (int k = 1; k <= cfg.keys; ++k) {
            int e = expected[k];
            for (int t = 1; t <= cfg.threads; ++t) e += res[t].net[k];
            if (e != 0 && e != 1) { if (badKeys < 5) std::cout << "  key " << k << " net presence " << e << "\n"; ++badKeys; }
            expected[k] = e;
            if (e == 1) { ++expSize; expSum += k; }
        }
        // What the structure holds, read single-threaded through contains().
        // On a self-adjusting structure these searches may promote; that is
        // harmless here and exercises promotion once more before validation.
        for (int k = 1; k <= cfg.keys; ++k) {
            const bool present = ds->contains(0, k);
            if (present) { ++dsSize; dsSum += k; }
            if ((int)present != expected[k]) {
                if (mismatches < 5) std::cout << "  key " << k << " expected " << expected[k] << " found " << present << "\n";
                ++mismatches;
            }
        }
        const bool structural = ds->validateStructure();
        const bool ok = badKeys == 0 && mismatches == 0 && sum.badValues == 0 && expSum == dsSum &&
                        expSize == dsSize && structural;
        allOk = allOk && ok;
        std::cout << "round=" << round << " elapsed=" << elapsed << " ops=" << sum.ops
                  << " mops_per_s=" << (sum.ops / elapsed / 1e6) << " ins=" << sum.ins << " ins_ok=" << sum.insOk
                  << " del=" << sum.del << " del_ok=" << sum.delOk << " finds=" << sum.finds << " hits=" << sum.hits
                  << "\n  threads_keysum=" << expSum << " ds_keysum=" << dsSum << " threads_size=" << expSize
                  << " ds_size=" << dsSize << " bad_keys=" << badKeys << " mismatches=" << mismatches
                  << " bad_values=" << sum.badValues << " structural=" << (structural ? "OK" : "FAIL")
                  << " rss_mib=" << rssMiB() << std::endl;
#ifdef BSL_ADAPTER
        ds->printSummary();
#endif
        if (!ok) break;
    }
    std::cout << "stress_result=" << (allOk ? "PASS" : "FAIL") << " total_ops=" << totalOps << std::endl;
    // Skip the exit hook's second validation of the same state.
    std::cout.flush();
    _exit(allOk ? 0 : 1);
}
