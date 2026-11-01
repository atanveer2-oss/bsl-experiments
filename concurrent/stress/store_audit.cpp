// Store audit: compiles the search path of bsl_gated out of line so that its
// machine code can be read (objdump) and every store instruction in it
// accounted for. Built and disassembled by run-store-audit.sh.
#include "globals_extern.h"
#include "random_fnv1a.h"
#include "plaf.h"
#include "adapter.h"
typedef ds_adapter<long long, void*, RECLAIM<>, ALLOC<>, POOL<> > Adapter;
__attribute__((noinline)) bool bsl_audit_contains(Adapter* a, int tid, long long key) {
    return a->contains(tid, key);
}
int main() {
    Adapter* a = new Adapter(8, 0, 1000, (void*)0, nullptr);
    for (long long k = 1; k < 100; ++k) a->insertIfAbsent(0, k, (void*)k);
    long long hits = 0;
    for (long long k = 1; k < 200; ++k) hits += bsl_audit_contains(a, 1, k);
    return hits == 99 ? 0 : 1;
}
