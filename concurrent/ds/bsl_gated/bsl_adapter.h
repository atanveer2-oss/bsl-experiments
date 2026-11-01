// SetBench adapter for the concurrent self-adjusting skip list
// (bsl_skiplist.h). Each ds/bsl_<rule>/adapter.h defines BSL_RULE and
// includes this file, so every variant shares one implementation and differs
// only in the promotion rule.
//
// Besides the fork's interface this adapter registers an exit hook (on by
// default; BSL_EXIT_REPORT=0 in the environment turns it off) that prints the
// promotion counters and runs validate() once the benchmark's threads have
// been joined. The fork's benchmark.cpp and benchmark_write.cpp never call
// validateStructure(), so without the hook a run of ours would be checked by
// nothing. A failed validation exits with status 3.

#ifndef BSL_ADAPTER_H
#define BSL_ADAPTER_H

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <csignal>
#include <unistd.h>
#include "errors.h"
#include "random_fnv1a.h"
#include "record_manager.h"
#ifdef USE_TREE_STATS
#include "tree_stats.h"
#endif
#include "bsl_skiplist.h"

#ifndef BSL_RULE
#error "define BSL_RULE (bsl_conc::GatedRule, ClassicRule, ExactRule or MorrisRule) before including bsl_adapter.h"
#endif

#ifndef BSL_EXIT_REPORT
#define BSL_EXIT_REPORT 1
#endif

#define BSL_ADAPTER 1

namespace bsl_conc {
// Type-erased hook so the exit handler can reach the last adapter built.
struct ExitHook {
    void* ds;
    void (*report)(void*);
};
inline ExitHook& exitHook() {
    static ExitHook h = {nullptr, nullptr};
    return h;
}
inline void runExitHook() {
    ExitHook& h = exitHook();
    if (h.ds && h.report) h.report(h.ds);
}
}  // namespace bsl_conc

#define RECORD_MANAGER_T record_manager<Reclaim, Alloc, Pool, bsl_conc::Node<K, V, BSL_RULE>>
#define DATA_STRUCTURE_T bsl_conc::SkipList<K, V, RECORD_MANAGER_T, BSL_RULE>

template <typename K, typename V, class Reclaim = reclaimer_debra<K>, class Alloc = allocator_new<K>,
          class Pool = pool_none<K>>
class ds_adapter {
private:
    const V NO_VALUE;
    DATA_STRUCTURE_T* const ds;

    static void exitReport(void* p) {
        DATA_STRUCTURE_T* d = static_cast<DATA_STRUCTURE_T*>(p);
        d->printStats(std::cout);
        const bool ok = d->validate(true);
        std::cout.flush();
        if (!ok) _exit(3);
    }

public:
    ds_adapter(const int NUM_THREADS, const K& KEY_MIN, const K& KEY_MAX, const V& VALUE_RESERVED,
               RandomFNV1A* const unused2)
        : NO_VALUE(VALUE_RESERVED), ds(new DATA_STRUCTURE_T(NUM_THREADS, VALUE_RESERVED, KEY_MIN, KEY_MAX)) {
#if BSL_EXIT_REPORT
        const char* env = std::getenv("BSL_EXIT_REPORT");
        if (!(env && std::strcmp(env, "0") == 0)) {
            bsl_conc::ExitHook& h = bsl_conc::exitHook();
            const bool first = (h.report == nullptr);
            h.ds = ds;
            h.report = &exitReport;
            if (first) std::atexit(&bsl_conc::runExitHook);
        }
#endif
    }

    ~ds_adapter() {
        bsl_conc::ExitHook& h = bsl_conc::exitHook();
        if (h.ds == ds) h.ds = nullptr;
        delete ds;
    }

    DATA_STRUCTURE_T* raw() { return ds; }

    V getNoValue() { return NO_VALUE; }

    long long getPathsLength(const int tid) { return ds->getPathsLength(tid); }

    void initThread(const int tid) { ds->initThread(tid); }
    void deinitThread(const int tid) { ds->deinitThread(tid); }

    // The splay-list's update period; it has no meaning for this rule.
    void setCops(const int tid, int cops) {}

    void warmupEnd() {}

    V insert(const int tid, const K& key, const V& val) {
        setbench_error("insert-replace functionality not implemented for this data structure");
    }
    V insertIfAbsent(const int tid, const K& key, const V& val) { return ds->insertIfAbsent(tid, key, val); }
    V erase(const int tid, const K& key) { return ds->erase(tid, key); }
    V find(const int tid, const K& key) { return ds->find(tid, key); }
    bool contains(const int tid, const K& key) { return ds->contains(tid, key); }
    int rangeQuery(const int tid, const K& lo, const K& hi, K* const resultKeys, V* const resultValues) {
        setbench_error("not implemented");
    }

    void printSummary() { ds->printStats(std::cout); }
    bool validateStructure() { return ds->validate(true); }

    // H_max, the number of levels in use (the fork's skip lists report the
    // 0-based index of their top level instead, i.e. one less).
    int getHeight() { return ds->getHeight(); }

    void printObjectSizes() {
        std::cout << "sizes: node=" << (sizeof(bsl_conc::Node<K, V, BSL_RULE>)) << std::endl;
    }

#ifdef USE_TREE_STATS
    class NodeHandler {
    public:
        typedef bsl_conc::Node<K, V, BSL_RULE>* NodePtrType;
        K minKey;
        K maxKey;
        NodeHandler(const K& _minKey, const K& _maxKey) : minKey(_minKey), maxKey(_maxKey) {}
        class ChildIterator {
        private:
            NodePtrType node;
            bool done;
        public:
            ChildIterator(NodePtrType _node) : node(_node), done(false) {}
            bool hasNext() { return node->next[0].load() != NULL && !done; }
            NodePtrType next() { done = true; return node->next[0].load(); }
        };
        bool isLeaf(NodePtrType node) { return node->next[0].load() == NULL; }
        size_t getNumChildren(NodePtrType node) { return node->next[0].load() == NULL ? 0 : 1; }
        size_t getNumKeys(NodePtrType node) { return node == NULL ? 0 : 1; }
        size_t getSumOfKeys(NodePtrType node) { return (size_t)node->key; }
        ChildIterator getChildIterator(NodePtrType node) { return ChildIterator(node); }
    };
    TreeStats<NodeHandler>* createTreeStats(const K& _minKey, const K& _maxKey) {
        return new TreeStats<NodeHandler>(new NodeHandler(_minKey, _maxKey), ds->getRoot(), false);
    }
#endif
};

#endif  // BSL_ADAPTER_H
