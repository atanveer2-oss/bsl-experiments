# Concurrent self-adjusting skip list in SetBench: local validation and smoke run

Step 3 of `analysis/handoff-experiments.md`: the paper's skip list behind the
SetBench adapter interface, built and validated in the local Linux container.
Everything here is a correctness record and a sanity check. **No number in
this file is a result for the paper**; Chameleon gives those.

## Machine and code

| | |
|---|---|
| host | Apple M2 (`sysctl -n machdep.cpu.brand_string`), macOS 27.0.1 |
| VM | Colima 0.8.1, aarch64, 6 vCPU, 7.7 GiB, kernel 6.5.0-10-generic |
| container | `bsl-setbench` (Ubuntu 22.04), g++ 11.4.0 |
| commit | `9802a44` (`git rev-parse --short HEAD`) plus the uncommitted changes listed below; work started at `a00b641` |
| fork | `third_party/splaylist` as vendored, unmodified; patched only in the build copy |

The M2 has 4 performance and 4 efficiency cores and the VM's 6 vCPUs float
over them, so throughput here mixes core types and is not stable from run to
run.

## Files

| Path | What |
|---|---|
| `ds/bsl_gated/bsl_skiplist.h` | the structure and the four rules (gated, classic, exact, Morris) |
| `ds/bsl_gated/bsl_adapter.h` | the SetBench adapter, shared by every variant |
| `ds/bsl_gated/adapter.h` | the paper's rule (Definition 1.1) |
| `ds/bsl_classic/adapter.h` | control arm: classic heights on the same code |
| `ds/bsl_exact/adapter.h`, `ds/bsl_morris/adapter.h` | the counter-based rules of `ExactEstimatorPolicy`, `MorrisEstimatorPolicy` |
| `stress/stress.cpp` | concurrent stress driver with per-key accounting, for any adapter |
| `stress/store_audit.cpp`, `run-store-audit.sh` | disassembly audit of the search path's stores |
| `build.sh` | now copies `ds/*` into the build copy, builds assertion and TSan variants and the driver |
| `portability.patch` | one more hunk (keygen.h, below) |
| `run-validation.sh`, `run-smoke.sh` | the campaigns recorded here |
| `../../.gitignore` | `experiments/setbench/.work/` |
| `README.md` | the binary count corrected (26 from the fork, not 40) |

Logs of every run (command line and full output) are in
`.work/logs/` (gitignored): `validation-run1.*`, `validation-run2.*`,
`validation-runF.*` (inserts-only pacing), `validation-FG.*` (final long stress and the counter rules), `validation-quick-final.*`, `smoke.csv`, `smoke.log`.

## Reproduce

```
cd <the repository that holds experiments/>
docker build -t bsl-setbench experiments/setbench
docker run --rm -v "$PWD/experiments:/work" bsl-setbench /work/setbench/build.sh
docker run --rm -v "$PWD/experiments:/work" bsl-setbench /work/setbench/run-store-audit.sh
docker run --rm --security-opt seccomp=unconfined -m 6g --memory-swap 6g \
  -v "$PWD/experiments:/work" bsl-setbench /work/setbench/run-validation.sh
docker run --rm -v "$PWD/experiments:/work" bsl-setbench /work/setbench/run-smoke.sh
```

`seccomp=unconfined` is needed only by the ThreadSanitizer runs (TSan calls
`personality(ADDR_NO_RANDOMIZE)`, which Docker's default profile blocks). The
memory cap makes an out-of-memory run kill its container rather than the VM
(see "Memory" below; without it the VM had to be force-restarted once).

## Build

The work directory now defaults to `experiments/setbench/.work`, so binaries
survive between `docker run`s. Baseline first, before any change of ours:
**zero errors**, but the "40 binaries" recorded on 2026-10-19 was wrong. The
fork ships 14 prebuilt x86-64 `ubench_*` binaries (an older harness) in
`microbench/bin`, `build.sh` copied them along, and `ls bin | wc -l` counted
them. Only 26 binaries (13 structures, bench and write each) are built from
the tree. `build.sh` now deletes the vendored ones before `make`.

With our four structures the build makes 34 binaries in `bin/` (the fork's
flags, `-DNDEBUG`) and, in `bin-asserts/`, assertion-enabled bench and write
binaries of ours, the stress driver for ours and for `skiplist_locks` and
`skiplist_lazy`, an always-promote stress variant and TSan builds.

One fork fix was needed and is now hunk 4 of `portability.patch`:
`microbench/keygen.h:97` asserts against `maxKey`, which is out of scope (it is
`data->maxKey`). The line exists only when `NDEBUG` is off, so the fork's
`use_asserts=1` build had never compiled. `-DNDEBUG` binaries are unchanged.

Parameters, compile time (`XARGS="-DBSL_K=2" build.sh`, or the Makefile's
`xargs=`): `BSL_P` (default 0.5), `BSL_K` (3), `BSL_MAX_HEIGHT` (64, at most
64), `BSL_MAX_THREADS` (1024). Run time: `BSL_SEED` (default 0) shifts every
thread's generator; `BSL_EXIT_REPORT=0` disables the exit report. The binary
names are `bench_bsl_gated.alloc_new.reclaim_none.pool_none.out` and
`write_bsl_gated...`, as for every fork structure.

## Design

**Baseline: `skiplist_locks`.** It is the lazy lock-based skip list of Herlihy,
Lev, Luchangco and Shavit (per-node locks, wait-free searches, a node is
marked and then unlinked under its predecessors' locks), and it is the skip
list the splay-list authors benchmark (`count_stats.sh`). Despite its name,
`skiplist_lazy` never unlinks anything: erase CASes the value to `noValue`,
and its `contains` does not even check that (see "Fork findings"). Search,
insert and erase are kept, including the validation conditions and the lock
order (predecessors bottom-up, i.e. in decreasing key order).

**Promotion** (on a successful `contains` or `find` of a live node):

1. read the published height H without a lock; the rule decides (gated: with
   probability p^H, via a per-thread xorshift64* seeded from the thread id and
   `BSL_SEED`, padded to its own cache lines) and draws H' = H + J + G;
2. locate predecessors and successors at every level below max(H_max, H');
3. lock the node; give up if it is marked or its height is no longer H;
4. lock the predecessors at levels H..H'-1 bottom-up (skipping repeats), and
   validate each: predecessor unmarked, successor unmarked, predecessor's
   `next` still the successor, and the successor is not the node itself;
   give up on any failure, with nothing linked yet;
5. link levels H..H'-1 bottom-up (node's `next` first, then the predecessor's,
   by a release store), publish the height last (release), raise H_max;
6. unlock.

Lock order is the insert path's: the node (key k), then predecessors with keys
below k in decreasing order. Erase was changed in one place: it reads the
victim's height under the victim's lock, after marking it (promotions take the
same lock and give up on a marked node, so the height is final there), and
re-locates if that height exceeds the levels it located. It no longer requires
meeting the victim at its top level, a test a growing node would fail
spuriously, and requires `fullyLinked` instead. A marked node is never
promoted. A lost promotion is counted (`aborted`) and the access then simply
did nothing.

**Memory ordering.** Shared fields are `std::atomic`: acquire loads, release
stores and a lock that releases. The fork relies on x86 ordering (plain fields,
`releaseLock` is `*lock = false`), which is a data race on aarch64 (TSan
reports 60 races on `skiplist_locks` in 3 s). On x86 our loads and stores
compile to the same plain moves, so this costs nothing where the paper's
numbers are measured.

**Height cap and memory.** Each node preallocates `next` at `BSL_MAX_HEIGHT`
= 64 levels, so a tower grows under concurrent readers without reallocation:
**544 bytes a node** (552 with the exact rule's counter), against 232 for the
fork's `skiplist_locks` (26 levels). At 10^6 keys that is about 520 MiB of
nodes. It is an engineering choice for the benchmark, not the representation
the paper's space bound (Proposition 6.3) is about. The extra size also
changes cache and TLB behaviour against the baseline, a confound to state
next to any throughput comparison with `skiplist_locks`; `bsl_classic`
removes it.

**H_max**, the only global, is an atomic raised by CAS-max after an insert or
promotion links a tower taller than it; searches load it (relaxed) to choose
their start level; the head's tower is preallocated at the cap. Like the
fork's `realMaxLvl` it never decreases, so under updates it tracks the
tallest tower ever inserted (about log2 of the number of inserts), not the
live maximum.

**A non-promoting search writes no shared memory.** By reading the code: the
path is `lookup()`; it loads `H_max`, `key`, `next[]`, `fullyLinked`,
`marked` and `height`; it stores only `pathLen` in the caller's padded
per-thread slot (the fork's metric, which `skiplist_locks` also writes) and,
inside the gated rule, the caller's own generator state. Everything else
(`gateOpens`, the lock traffic) is after the gate opened, in the out-of-line
`promote()`. By machine code (`run-store-audit.sh`, `-O2 -DNDEBUG`, aarch64):
`lookup` compiles to 172 instructions with 10 that write memory: 5 stack
spills (`[sp, ...]`), 2 stores of `me.st.pathLen` (`bsl_skiplist.h:686`, one
per exit path), 2 of the generator state `s = x` (`:143`), and 1 of
`++me.st.gateOpens` (`:696`) immediately before the call to `promote`. No
atomic read-modify-write, no store through a node pointer. (The fork's
`flexlist_locks`, by contrast, writes `succ->contains++` on every successful
search.) This holds for the gated and classic rules; the exact rule writes
its counter on every hit by design, and the Morris rule writes only when its
coin succeeds.

**Path length.** The search loop is `skiplist_locks`' `contains` verbatim, so
`getPathsLength` counts the same thing: one per level visited plus one per
forward step, stopping at the level where the key is first met (the paper's
vertical span plus horizontal steps, with a hit stopping at the item's top
level). Only searches are counted, as in the fork; the second locate a
promotion makes is not. `flexlist_locks` counts the same way; the CBTree's
number is a tree depth and not comparable. `getHeight` returns H_max as a
number of levels (the fork's skip lists return a 0-based top index).

**Exit report.** The fork's `benchmark.cpp` and `benchmark_write.cpp` validate
nothing after a run (the key-sum check exists only in upstream SetBench's
`main.cpp`, under `USE_TREE_STATS`, which the Makefile does not build). So our
adapter registers an exit hook: after the benchmark's threads are joined it
prints `bsl_stats` (promotions, aborts, H_max) and runs `validate()`, and a
failure exits with status 3. `BSL_EXIT_REPORT=0` turns it off.

**`validate()`** (quiescent): level 0 strictly sorted; every level strictly
sorted and a subsequence of the level below; no reachable node marked or not
fully linked; each node linked at exactly levels 0..height-1; nothing at or
above H_max; the tail has no successor; with assertions on (node tracking),
every node ever inserted is reachable if and only if it is unmarked; and the
rule's invariant (H >= rank(f) for exact, H >= Q for Morris), which may be
broken only by an abandoned promotion and is a failure if none was.

**Stress driver** (`stress/stress.cpp`): each thread records per key its
successful inserts minus successful erases; afterwards prefill plus the sum
over threads must be 0 or 1 for every key and equal what `contains` reports;
the key sum and size are compared (SetBench's own check); values returned by
`insertIfAbsent` and `erase` must be the key's; then `validateStructure()`.
Distributions: uniform, Zipf over a shuffled key order, hot set. `rounds R`
repeats the check R times on the same structure. `insrate R` paces each
thread's successful inserts and erases to R per second each (memory bound,
below).

## Validation

All runs in the container, assertion builds, 5 s each unless stated.
"Promotions" are completed promotions; "aborted" are promotions abandoned
under contention. A run passes when it exits 0 and prints the expected
verdicts (`stress_result=PASS`, or `bsl_validate=OK` from the exit hook) and
no TSan report.

Run 1 (the code before `BSL_SEED`, the invariant check and the `noinline`)
and run 2 (final code) of sections A to E gave the same outcomes. Section F
first failed for want of memory twice (below) and was rerun with pacing.
Tables are run 2 except where marked. A last quick pass of A to D on the
final binaries (2 s runs, `run-validation.sh quick` with `ONLY=ABCDE`, which
skips TSan; `validation-quick-final.*`) gave the same outcomes: 66 runs pass,
and the 2 failures are `skiplist_lazy`'s, as in every run (fork finding 2).
The A' rows for `skiplist_lazy` and the TSan control are expected failures.

#### A. Stress driver

| group | structure | threads | keys | mix | dist | s | ops | promotions | aborted | outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| A-update-bsl_gated | bsl_gated | 1 | 10000 | 25% ins, 25% del | uniform | 6 | 14,614,272 | 493,664 | 0 | PASS |
| A-update-bsl_gated | bsl_gated | 2 | 10000 | 25% ins, 25% del | uniform | 5 | 25,354,240 | 857,310 | 113 | PASS |
| A-update-bsl_gated | bsl_gated | 4 | 10000 | 25% ins, 25% del | uniform | 5 | 35,734,272 | 1,208,997 | 592 | PASS |
| A-update-bsl_gated | bsl_gated | 6 | 10000 | 25% ins, 25% del | uniform | 6 | 40,704,256 | 1,377,137 | 1,575 | PASS |
| A-update-bsl_gated | bsl_gated | 1 | 10000 | 25% ins, 25% del | Zipf 1 | 5 | 11,803,392 | 399,472 | 0 | PASS |
| A-update-bsl_gated | bsl_gated | 2 | 10000 | 25% ins, 25% del | Zipf 1 | 5 | 22,045,952 | 743,163 | 2,416 | PASS |
| A-update-bsl_gated | bsl_gated | 4 | 10000 | 25% ins, 25% del | Zipf 1 | 6 | 40,372,480 | 1,350,288 | 14,128 | PASS |
| A-update-bsl_gated | bsl_gated | 6 | 10000 | 25% ins, 25% del | Zipf 1 | 5 | 40,309,504 | 1,335,603 | 25,406 | PASS |
| A-update-bsl_gated | bsl_gated | 1 | 10000 | 25% ins, 25% del | hot 90/10 | 5 | 11,416,576 | 387,409 | 0 | PASS |
| A-update-bsl_gated | bsl_gated | 2 | 10000 | 25% ins, 25% del | hot 90/10 | 5 | 21,227,776 | 718,030 | 164 | PASS |
| A-update-bsl_gated | bsl_gated | 4 | 10000 | 25% ins, 25% del | hot 90/10 | 5 | 40,600,320 | 1,371,832 | 1,102 | PASS |
| A-update-bsl_gated | bsl_gated | 6 | 10000 | 25% ins, 25% del | hot 90/10 | 6 | 41,168,384 | 1,389,902 | 2,425 | PASS |
| A-update-bsl_gated | bsl_gated | 1 | 100000 | 25% ins, 25% del | uniform | 5 | 6,051,584 | 215,192 | 0 | PASS |
| A-update-bsl_gated | bsl_gated | 2 | 100000 | 25% ins, 25% del | uniform | 5 | 12,106,240 | 419,530 | 4 | PASS |
| A-update-bsl_gated | bsl_gated | 4 | 100000 | 25% ins, 25% del | uniform | 5 | 21,242,880 | 728,578 | 19 | PASS |
| A-update-bsl_gated | bsl_gated | 6 | 100000 | 25% ins, 25% del | uniform | 6 | 25,034,496 | 855,731 | 44 | PASS |
| A-update-bsl_gated | bsl_gated | 1 | 100000 | 25% ins, 25% del | Zipf 1 | 5 | 6,475,776 | 229,244 | 0 | PASS |
| A-update-bsl_gated | bsl_gated | 2 | 100000 | 25% ins, 25% del | Zipf 1 | 5 | 12,255,488 | 424,599 | 440 | PASS |
| A-update-bsl_gated | bsl_gated | 4 | 100000 | 25% ins, 25% del | Zipf 1 | 5 | 22,176,000 | 757,460 | 2,632 | PASS |
| A-update-bsl_gated | bsl_gated | 6 | 100000 | 25% ins, 25% del | Zipf 1 | 6 | 20,153,344 | 685,821 | 4,247 | PASS |
| A-update-bsl_gated | bsl_gated | 1 | 100000 | 25% ins, 25% del | hot 90/10 | 5 | 6,176,256 | 218,951 | 0 | PASS |
| A-update-bsl_gated | bsl_gated | 2 | 100000 | 25% ins, 25% del | hot 90/10 | 5 | 11,290,112 | 391,911 | 5 | PASS |
| A-update-bsl_gated | bsl_gated | 4 | 100000 | 25% ins, 25% del | hot 90/10 | 5 | 20,617,216 | 707,885 | 31 | PASS |
| A-update-bsl_gated | bsl_gated | 6 | 100000 | 25% ins, 25% del | hot 90/10 | 5 | 23,343,616 | 798,895 | 88 | PASS |
| A-control-bsl_classic | bsl_classic | 6 | 10000 | 25% ins, 25% del | uniform | 6 | 36,483,840 | 0 | 0 | PASS |
| A-control-bsl_classic | bsl_classic | 6 | 10000 | 25% ins, 25% del | Zipf 1 | 5 | 28,973,056 | 0 | 0 | PASS |
| A-control-skiplist_locks | skiplist_locks | 6 | 10000 | 25% ins, 25% del | uniform | 6 | 51,696,896 |  |  | PASS |
| A-control-skiplist_locks | skiplist_locks | 6 | 10000 | 25% ins, 25% del | Zipf 1 | 5 | 38,336,000 |  |  | PASS |
| A-control-skiplist_lazy | skiplist_lazy | 6 | 10000 | 25% ins, 25% del | uniform | 5 | 47,484,928 |  |  | FAIL(rc=1) |
| A-control-skiplist_lazy | skiplist_lazy | 6 | 10000 | 25% ins, 25% del | Zipf 1 | 5 | 44,750,336 |  |  | FAIL(rc=1) |

#### B. Fork benchmark_write

| group | structure | threads | keys | mix | dist | s | ops | promotions | aborted | outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| B-fork-write | bsl_gated | 1 | 10000 | 20% upd (fork) | uniform | 6 | 22,763,000 | 305,180 | 0 | PASS |
| B-fork-write | bsl_gated | 2 | 10000 | 20% upd (fork) | uniform | 6 | 41,338,000 | 556,772 | 32 | PASS |
| B-fork-write | bsl_gated | 4 | 10000 | 20% upd (fork) | uniform | 7 | 76,654,000 | 1,029,757 | 305 | PASS |
| B-fork-write | bsl_gated | 6 | 10000 | 20% upd (fork) | uniform | 6 | 83,204,000 | 1,130,091 | 705 | PASS |
| B-fork-write | bsl_gated | 1 | 10000 | 20% upd (fork) | 90/10 | 6 | 26,108,000 | 225,887 | 0 | PASS |
| B-fork-write | bsl_gated | 2 | 10000 | 20% upd (fork) | 90/10 | 7 | 47,562,000 | 401,530 | 55 | PASS |
| B-fork-write | bsl_gated | 4 | 10000 | 20% upd (fork) | 90/10 | 6 | 89,769,000 | 752,934 | 442 | PASS |
| B-fork-write | bsl_gated | 6 | 10000 | 20% upd (fork) | 90/10 | 7 | 87,992,000 | 769,050 | 921 | PASS |
| B-fork-write | bsl_gated | 1 | 10000 | 20% upd (fork) | 99/1 | 6 | 34,458,000 | 66,549 | 0 | PASS |
| B-fork-write | bsl_gated | 2 | 10000 | 20% upd (fork) | 99/1 | 6 | 67,967,000 | 126,824 | 197 | PASS |
| B-fork-write | bsl_gated | 4 | 10000 | 20% upd (fork) | 99/1 | 7 | 118,472,000 | 214,155 | 1,000 | PASS |
| B-fork-write | bsl_gated | 6 | 10000 | 20% upd (fork) | 99/1 | 8 | 137,030,000 | 248,441 | 2,404 | PASS |
| B-fork-write | bsl_gated | 1 | 100000 | 20% upd (fork) | uniform | 6 | 8,607,000 | 196,078 | 0 | PASS |
| B-fork-write | bsl_gated | 2 | 100000 | 20% upd (fork) | uniform | 6 | 16,702,000 | 315,234 | 1 | PASS |
| B-fork-write | bsl_gated | 4 | 100000 | 20% upd (fork) | uniform | 7 | 32,805,000 | 539,243 | 2 | PASS |
| B-fork-write | bsl_gated | 6 | 100000 | 20% upd (fork) | uniform | 6 | 35,384,000 | 599,916 | 17 | PASS |
| B-fork-write | bsl_gated | 1 | 100000 | 20% upd (fork) | 90/10 | 6 | 10,613,000 | 145,421 | 0 | PASS |
| B-fork-write | bsl_gated | 2 | 100000 | 20% upd (fork) | 90/10 | 6 | 20,387,000 | 233,761 | 0 | PASS |
| B-fork-write | bsl_gated | 4 | 100000 | 20% upd (fork) | 90/10 | 7 | 38,239,000 | 388,253 | 9 | PASS |
| B-fork-write | bsl_gated | 6 | 100000 | 20% upd (fork) | 90/10 | 6 | 45,480,000 | 459,188 | 28 | PASS |
| B-fork-write | bsl_gated | 1 | 100000 | 20% upd (fork) | 99/1 | 6 | 17,564,000 | 45,937 | 0 | PASS |
| B-fork-write | bsl_gated | 2 | 100000 | 20% upd (fork) | 99/1 | 6 | 28,814,000 | 70,484 | 3 | PASS |
| B-fork-write | bsl_gated | 4 | 100000 | 20% upd (fork) | 99/1 | 7 | 58,073,000 | 124,735 | 29 | PASS |
| B-fork-write | bsl_gated | 6 | 100000 | 20% upd (fork) | 99/1 | 6 | 62,192,000 | 131,307 | 54 | PASS |

#### C. Read-only

| group | structure | threads | keys | mix | dist | s | ops | promotions | aborted | outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| C-readonly-fork | bsl_gated | 6 | 10000 | read-only | Zipf 1 | 11 | 256,284,000 | 40,386 | 3 | PASS |
| C-readonly-fork | bsl_gated | 6 | 10000 | read-only | 90/10 | 11 | 374,135,000 | 39,894 | 4 | PASS |
| C-readonly-fork | bsl_gated | 6 | 10000 | read-only | 99/1 | 11 | 763,641,000 | 30,069 | 3 | PASS |
| C-readonly-fork | bsl_gated | 6 | 100000 | read-only | Zipf 1 | 11 | 112,710,000 | 243,454 | 5 | PASS |
| C-readonly-fork | bsl_gated | 6 | 100000 | read-only | 90/10 | 11 | 106,941,000 | 228,659 | 2 | PASS |
| C-readonly-fork | bsl_gated | 6 | 100000 | read-only | 99/1 | 12 | 420,896,000 | 162,416 | 0 | PASS |
| C-readonly-fork | bsl_gated | 6 | 1000000 | read-only | Zipf 1 | 17 | 51,748,000 | 928,546 | 1 | PASS |
| C-readonly-fork | bsl_gated | 6 | 1000000 | read-only | 90/10 | 16 | 44,362,000 | 830,318 | 1 | PASS |
| C-readonly-fork | bsl_gated | 6 | 1000000 | read-only | 99/1 | 17 | 115,947,000 | 253,170 | 0 | PASS |
| C-readonly-driver | bsl_gated | 6 | 100 | read-only | Zipf 1 | 5 | 329,910,528 | 657 | 2 | PASS |
| C-readonly-driver | bsl_gated | 6 | 1000 | read-only | Zipf 1 | 5 | 198,612,480 | 5,098 | 4 | PASS |

#### D. Always-promote

| group | structure | threads | keys | mix | dist | s | ops | promotions | aborted | outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| D-always-update | bsl_gated_always | 6 | 100 | 10% ins, 10% del | hot 90/10 | 32 | 254,108,416 | 70,755,764 | 24,895,420 | PASS |
| D-always-update | bsl_gated_always | 6 | 1000 | 10% ins, 10% del | Zipf 1 | 32 | 225,767,680 | 75,844,527 | 7,252,529 | PASS |
| D-always-readonly | bsl_gated_always | 6 | 100000 | read-only | uniform | 51 | 1,348,096 | 1,430,549 | 64 | PASS |

#### E. ThreadSanitizer

| group | structure | threads | keys | mix | dist | s | ops | promotions | aborted | outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| E-tsan | bsl_gated_tsan | 6 | 1000 | 20% ins, 20% del | Zipf 1 | 12 | 4,549,120 | 158,295 | 7,683 | PASS |
| E-tsan | bsl_gated_always_tsan | 6 | 200 | 10% ins, 10% del | hot 90/10 | 10 | 2,520,064 | 829,167 | 114,855 | PASS |
| E-tsan-control-expected-FAIL | skiplist_locks_tsan | 4 | 1000 | 20% ins, 20% del | Zipf 1 | 4 | 49,920 |  |  | FAIL(rc=66) |

**Summary of A to F, gated rule**: every run passes. A: 24 configurations
(1/2/4/6 threads, 10^4 and 10^5 keys, uniform, Zipf 1, hot 90/10, 50%
updates), 18.1M promotions, 55K aborted. B: the fork's `benchmark_write`, 24
configurations, 9.1M promotions, all validated by the exit hook. C: read-only
hot keys, 6 threads, 2.8M promotions in the fork's benchmark (10^4 to 10^6
keys), very few aborts, because the p^H decay makes concurrent promotions of
the same item rare. D: the test-only always-promote rule, 147M promotions and
32M aborts in two 30 s runs. E: TSan clean on ours (1.0M promotions, 123K
aborted), while the positive control flags `skiplist_locks`. F: long stress,
below.

### Memory, and the long stress

Under `reclaimer_none` (the fork's setting for every binary) erased nodes are
never freed, in the fork's skip lists as in ours, and at 544 bytes a node an
unpaced update mix fills the VM. The first long run (20% inserts, 1000 keys)
passed its first 30 s round (13.1M successful inserts, about 7.1 GB) and was
OOM-killed in the second (exit 137). The second attempt, at 0.5% updates,
still leaked about 77K nodes a second at 31M operations a second, and drove
the VM into swap until it had to be force-restarted. Fix: `insrate`, a
thread-local pace on successful updates, so memory is bounded by rate,
threads and duration, whatever the throughput. A first paced version paced
only inserts, and the unpaced erases drained the set to 0 to 10 keys (it
passed, but that is not the intended stress); the final version paces both.

Final long stress (`validation-FG.*`, final binaries, erases and inserts
both paced at 2500 per second per thread, 6 threads, ten 30 s rounds with the
full check after each, assertions on):

| run | keys | mix | dist | duration | ops | promotions | aborted | live keys at end | peak RSS | outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| gated | 1000 | 10% ins, 10% del (paced) | Zipf 1 | 10 x 30 s | 8,310,904,064 | 11,936,803 | 348,132 | 506 of 1000 | 2.4 GiB | PASS, 10/10 rounds |
| gated | 100 | 10% ins, 10% del (paced) | uniform | 10 x 30 s | 10,504,645,888 | 12,474,967 | 176,456 | 48 of 100 | 2.4 GiB | PASS, 10/10 rounds |
| always-promote | 1000 | 10% ins, 10% del (paced) | hot 90/10 | 10 x 30 s | 966,775,040 | 82,681,266 | 3,151,126 | 506 of 1000 | 1.0 GiB | PASS, 10/10 rounds |

The earlier, inserts-only-paced version (`validation-runF.*`) also passed all
30 rounds (10.2G, 15.9G and 7.7G operations; 3.0M, 3.0M and 33.2M
promotions), on a set drained to 0 to 10 live keys.

## Fork findings (none fixed in the vendored tree)

1. **No post-run validation** in `benchmark.cpp` or `benchmark_write.cpp`:
   neither calls `validateStructure()` nor checks a key sum. A broken
   structure produces numbers with exit status 0. Ours validates at exit;
   the baselines are checked only by the stress driver.
2. **`skiplist_lazy` reports deleted keys as present.** It deletes by CAS of
   the value to `noValue` and never unlinks, but `contains` returns
   `fullyLinked && !mark` without looking at the value. The driver shows it
   (6 threads, 10^4 keys, 50% updates: 10 000 keys reported against 4 987
   live). Its update-mix numbers measure searches of a set that never
   shrinks. `skiplist_locks` passes the same runs.
3. **Zipf in `benchmark_write` reads out of bounds.** The generator draws ranks
   over `[1, prefill]` but indexes `important`, which holds only the roughly
   prefill/2 keys inserted at prefill. For alpha 1 and 10^5 keys about 6% of
   lookups read past the vector. The authors' `count_stats_write.sh` uses
   this mode. Section B therefore uses 99/1 instead of Zipf. (The Chameleon
   kit's `harness.patch`, committed meanwhile, fixes the same bug and the
   100-thread array limit.)
4. **aarch64 data races in the fork's lock-based structures**: plain fields
   and a lock release without a fence. Correct on x86 (TSO), racy on ARM:
   TSan reports 60 races on `skiplist_locks` in 3 s. Local baseline numbers
   are therefore from technically racy code; the x86 numbers are not
   affected.
5. **Fixed seeds.** The fork's skip lists draw heights from a constant seed, so
   their repetitions replay one structure. That matters for path length: on
   10^5 uniform keys our classic control gives 29.8, 31.2, 31.9, 31.9 and
   33.5 over five seeds, while `skiplist_locks` gives 28.5, which a Python
   simulation of its actual height sequence reproduces (28.8; maximum height
   16, against 16 to 20 for geometric heights). The counting is the same; the
   fork's single structure is a lucky one. Compare ours against
   `bsl_classic` across seeds, not against one `skiplist_locks` structure.

## Smoke measurement

Protocol: the README's, shortened. `benchmark.cpp` (100% lookups), prefill
10^5, warmup 3 s (`presecs 3`), measured 5 s (`secs 5`), uniform
(`x 100 y 100`), 90/10 (`x 90 y 10`) and Zipf 1 (`zipf 1`), threads 1, 2, 4,
6, 3 repetitions, structures interleaved within each repetition. The
splay-list and the CBTree take `cops 100*T`, the value the authors' scaling
scripts use (`count_stats_2.sh`, `count_stats_zipf.sh`). Ours run with
`BSL_SEED` = the repetition number; the fork's structures have fixed seeds,
so their repetitions differ only in timing. Command line, e.g.:

```
BSL_SEED=1 ./bin/bench_bsl_gated.alloc_new.reclaim_none.pool_none.out prefill 100000 presecs 3 secs 5 x 90 y 10 threads 6
./bin/bench_flexlist_locks.alloc_new.reclaim_none.pool_none.out prefill 100000 presecs 3 secs 5 zipf 1 cops 600 threads 6
```

Every command and its output are in `.work/logs/smoke.log`, one row per run
in `.work/logs/smoke.csv`. Throughput is the measured window's operations over
5 s; path length is the benchmark's `sumLengths` (mean per operation over the
measured window). Every run of ours passed the exit validation.

#### uniform

| structure | threads | Mops/s, mean (min-max) | path length, mean (min-max) | promotions in run (mean) |
|---|---|---|---|---|
| bsl_gated | 1 | 1.58 (1.58-1.58) | 31.3 (31.1-31.6) | 204,571 |
| bsl_gated | 2 | 3.03 (2.92-3.13) | 31.6 (30.7-33.1) | 236,525 |
| bsl_gated | 4 | 4.53 (3.53-6.04) | 32.7 (32.2-33.6) | 254,944 |
| bsl_gated | 6 | 3.96 (1.62-5.56) | 32.7 (31.0-35.4) | 241,702 |
| bsl_classic | 1 | 1.66 (1.65-1.67) | 31.7 (29.8-33.5) | 0 |
| bsl_classic | 2 | 2.93 (2.07-3.37) | 31.7 (29.8-33.5) | 0 |
| bsl_classic | 4 | 4.04 (3.60-4.44) | 31.7 (29.8-33.5) | 0 |
| bsl_classic | 6 | 4.77 (1.79-6.51) | 31.7 (29.8-33.5) | 0 |
| skiplist_locks | 1 | 2.04 (2.02-2.06) | 28.5 (28.5-28.5) |  |
| skiplist_locks | 2 | 3.59 (2.55-4.14) | 28.5 (28.5-28.5) |  |
| skiplist_locks | 4 | 4.59 (3.89-5.16) | 28.5 (28.5-28.5) |  |
| skiplist_locks | 6 | 4.92 (2.52-6.15) | 28.5 (28.5-28.5) |  |
| flexlist_locks | 1 | 1.40 (1.37-1.44) | 24.5 (24.5-24.5) |  |
| flexlist_locks | 2 | 2.36 (1.59-2.79) | 24.5 (24.5-24.5) |  |
| flexlist_locks | 4 | 2.71 (1.25-3.77) | 24.5 (24.5-24.6) |  |
| flexlist_locks | 6 | 3.28 (1.39-4.46) | 24.5 (24.5-24.6) |  |
| cbtree-concurrent | 1 | 1.64 (1.61-1.69) | 10.1 (10.1-10.1) |  |
| cbtree-concurrent | 2 | 2.65 (1.62-3.20) | 10.1 (10.1-10.1) |  |
| cbtree-concurrent | 4 | 3.25 (2.02-5.17) | 10.1 (10.1-10.1) |  |
| cbtree-concurrent | 6 | 3.22 (0.63-5.12) | 10.1 (10.1-10.1) |  |

#### 90-10

| structure | threads | Mops/s, mean (min-max) | path length, mean (min-max) | promotions in run (mean) |
|---|---|---|---|---|
| bsl_gated | 1 | 2.25 (1.48-2.68) | 27.3 (25.5-29.4) | 138,145 |
| bsl_gated | 2 | 4.16 (2.30-5.10) | 26.8 (26.2-27.4) | 164,459 |
| bsl_gated | 4 | 5.85 (3.61-7.04) | 26.6 (25.4-28.3) | 181,749 |
| bsl_gated | 6 | 7.86 (3.96-9.92) | 28.5 (27.6-29.1) | 194,064 |
| bsl_classic | 1 | 1.50 (1.00-1.75) | 31.8 (29.9-33.5) | 0 |
| bsl_classic | 2 | 2.91 (1.74-3.57) | 31.8 (29.9-33.5) | 0 |
| bsl_classic | 4 | 3.91 (2.22-4.92) | 31.8 (29.9-33.5) | 0 |
| bsl_classic | 6 | 5.45 (2.16-7.10) | 31.8 (29.9-33.5) | 0 |
| skiplist_locks | 1 | 1.71 (0.80-2.17) | 28.5 (28.5-28.5) |  |
| skiplist_locks | 2 | 2.95 (1.60-4.31) | 28.5 (28.5-28.5) |  |
| skiplist_locks | 4 | 4.43 (2.08-5.86) | 28.5 (28.5-28.5) |  |
| skiplist_locks | 6 | 5.58 (2.28-7.60) | 28.5 (28.5-28.5) |  |
| flexlist_locks | 1 | 1.66 (1.02-1.98) | 23.3 (23.2-23.7) |  |
| flexlist_locks | 2 | 2.41 (1.52-3.77) | 23.6 (23.2-23.9) |  |
| flexlist_locks | 4 | 3.56 (1.85-4.45) | 23.7 (23.5-24.1) |  |
| flexlist_locks | 6 | 4.54 (2.15-5.83) | 23.8 (23.7-24.2) |  |
| cbtree-concurrent | 1 | 2.12 (1.35-2.54) | 9.7 (9.6-9.8) |  |
| cbtree-concurrent | 2 | 3.24 (1.48-4.42) | 9.9 (9.8-10.0) |  |
| cbtree-concurrent | 4 | 4.07 (1.57-6.27) | 10.0 (10.0-10.1) |  |
| cbtree-concurrent | 6 | 4.38 (0.85-6.38) | 10.1 (10.1-10.1) |  |

#### zipf1

| structure | threads | Mops/s, mean (min-max) | path length, mean (min-max) | promotions in run (mean) |
|---|---|---|---|---|
| bsl_gated | 1 | 2.29 (1.71-2.63) | 23.6 (22.9-25.0) | 149,812 |
| bsl_gated | 2 | 4.45 (2.93-5.23) | 20.7 (19.5-22.8) | 180,048 |
| bsl_gated | 4 | 5.90 (3.37-7.25) | 22.3 (21.6-23.6) | 194,772 |
| bsl_gated | 6 | 8.52 (4.01-11.45) | 22.0 (19.1-25.9) | 208,425 |
| bsl_classic | 1 | 1.60 (1.27-1.92) | 31.3 (30.0-32.6) | 0 |
| bsl_classic | 2 | 3.04 (1.58-3.80) | 31.3 (30.0-32.6) | 0 |
| bsl_classic | 4 | 4.13 (2.78-4.91) | 31.3 (30.0-32.6) | 0 |
| bsl_classic | 6 | 5.97 (2.71-7.74) | 31.3 (30.0-32.6) | 0 |
| skiplist_locks | 1 | 2.03 (1.71-2.19) | 28.4 (28.4-28.4) |  |
| skiplist_locks | 2 | 3.25 (1.78-4.30) | 28.4 (28.4-28.4) |  |
| skiplist_locks | 4 | 4.74 (3.45-5.59) | 28.4 (28.4-28.4) |  |
| skiplist_locks | 6 | 5.82 (3.04-7.58) | 28.4 (28.4-28.4) |  |
| flexlist_locks | 1 | 2.31 (1.24-2.85) | 13.8 (13.8-13.8) |  |
| flexlist_locks | 2 | 3.71 (2.18-4.97) | 13.8 (13.8-13.8) |  |
| flexlist_locks | 4 | 6.06 (3.23-7.70) | 13.8 (13.8-13.8) |  |
| flexlist_locks | 6 | 7.87 (3.37-10.26) | 13.8 (13.8-13.8) |  |
| cbtree-concurrent | 1 | 2.44 (1.82-2.75) | 5.9 (5.9-5.9) |  |
| cbtree-concurrent | 2 | 4.70 (2.99-6.10) | 5.9 (5.9-5.9) |  |
| cbtree-concurrent | 4 | 5.05 (2.54-6.84) | 5.9 (5.9-5.9) |  |
| cbtree-concurrent | 6 | 7.41 (1.95-10.55) | 5.9 (5.9-5.9) |  |

### Reading

**Path length** (machine-independent, so this part of the smoke run means
something):

- Uniform: `bsl_gated` 31.3 to 32.7 against `bsl_classic` 31.7 (whose seeds
  span 29.8 to 33.5). The same, as it should be: with uniform access the rule
  has nothing to adapt to.
- 90/10: 26.6 to 28.5 against 31.8, about 15% shorter. Zipf 1: 20.7 to 23.6
  against 31.3, about 30% shorter. Shorter on skewed workloads, in the
  expected direction and growing with skew.
- `skiplist_locks` (28.5) is one fixed-seed structure, below the classic
  control's seed range for the reason in fork finding 5; it is not a
  different algorithm.
- H_max: 25 to 32 for `bsl_gated`, 17 to 21 for `bsl_classic`. Every search
  pays that vertical span; the hot items repay it by being met high. At 10^5
  keys and these run lengths the trade is a net gain only under skew.
- **The splay-list's paths are shorter than ours**: 23.3 to 23.8 at 90/10 and
  13.8 at Zipf 1 (24.5 even on uniform, since it rebuilds towards a balanced
  shape). With p = 1/2, K = 3 and 8 s runs, ours does not match it on this
  metric. Not established here, but plausible contributors: the K = 3 jumps
  (mean jump J + G = 3) push H_max up for everyone, and heights keep growing
  as log t on every item that is touched. A sweep over K and p, and the
  cost-against-elapsed-time curve, should come before any claim against the
  splay-list.
- The CBTree's number is a tree depth (5.9 to 10.1) and is not comparable with
  the skip lists'.

**Throughput proves nothing here.** Six vCPUs of a VM floating over an M2's
performance and efficiency cores give ranges like 1.6 to 5.6 Mops/s inside one
cell; at 6 threads every structure falls within every other's range. The only
readings stable across repetitions are single-threaded: `skiplist_locks` is
faster than our classic control on uniform (2.04 against 1.66 Mops/s), partly
from its luckier structure (path 28.5 against 31.7) and plausibly from our
544-byte nodes and the acquire loads that aarch64 pays for (`ldar`) and x86
does not; and the gated rule is faster than the classic control on skewed
workloads (2.25 against 1.50 at 90/10, 2.29 against 1.60 at Zipf 1),
consistent with its shorter paths. None of this is a scalability statement;
contention effects in this literature live at 20 to 70 threads.

## Counter-based variants

`bsl_exact` and `bsl_morris` exist and are validated. They share
`bsl_skiplist.h` and `bsl_adapter.h` with the gated variant and differ only in
the rule (`ExactRule`, `MorrisRule`), which implements exactly
`ExactEstimatorPolicy` and `MorrisEstimatorPolicy` of
`experiments/include/bsl/skiplist.hpp`:

- **Exact**: a per-node 64-bit counter f (insertion counts as 1), incremented
  on every successful search by a relaxed `fetch_add`; if then
  H <= rank = floor(log_b f), the rule requests rank + G. Insertion height
  1 + G. A lost promotion heals itself, since H <= rank still holds at the
  next hit. Every hit writes the node's cache line: the opposite of the gated
  rule's read-only miss, by design. Node: 552 bytes.
- **Morris**: a per-node counter Q, starting at 1. With probability p^Q the
  thread CASes Q to Q + 1; a thread that loses the CAS treats its access as
  not incrementing. If its CAS succeeded and H <= Q before the increment, it
  requests Q + 1 + G. Sequentially H >= Q always, so "H <= Q" is "H = Q" and
  the request is H + 1 + G, the paper's rule exactly; the "<=" form lets a
  promotion lost to contention be made good at the next increment rather
  than leaving H below Q for good. A failed coin writes nothing.

`validate()` checks the rule's invariant (H >= rank(f), H >= Q) and fails on a
violation unless a promotion was abandoned. Section G of `run-validation.sh`
(`validation-FG.*`):

| runs | configurations | promotions | aborted | invariant violations at end | outcome |
|---|---|---|---|---|---|
| exact, stress driver | 1/2/4/6 threads x uniform, Zipf 1, hot 90/10; 10^4 keys, 50% updates | 12.2M | 51K | 0 in all 12 | PASS |
| exact, fork `benchmark_write` | 1 and 6 threads, 10^5 keys, 90/10, 20% updates | 0.69M | 64 | 0 | PASS |
| exact, fork `benchmark` | 1 and 6 threads, 10^5 keys, Zipf 1, read-only | 0.53M | 20 | 0 | PASS |
| exact, TSan | 6 threads, 10^3 keys, Zipf 1, 40% updates, 10 s | 0.18M | 10.5K | 0 | PASS, no report |
| exact, long | 6 threads, 10^3 keys, Zipf 1, paced updates, 4 x 30 s | 7.0M | 0.78M | 0 | PASS |
| Morris, stress driver | as for exact | 11.5M | 25K | 0 in all 12 | PASS |
| Morris, fork `benchmark_write` | as for exact | 0.60M | 18 | 0 | PASS |
| Morris, fork `benchmark` | as for exact | 0.48M | 2 | 0 | PASS |
| Morris, TSan | as for exact | 0.17M | 5.3K | 0 | PASS, no report |
| Morris, long | as for exact | 6.5M | 71K | 0 | PASS |

The single-threaded runs abandon no promotion, so for them the invariant is
checked strictly. One caveat for the multi-threaded stress-driver runs: the
driver's final single-threaded `contains` sweep touches every key once more
before `validate()`, which heals a lost promotion, so those runs cannot show
a violation. The fork-benchmark runs have no sweep and show none either. An
earlier ad hoc 6-thread Morris run did end with 2 violating nodes after 74K
abandoned promotions, which the check allows.

## Open problems and risks

No problem was left blocked. Tagged P0 (correctness), P1 (performance or
protocol), P2 (cosmetic).

- **P0, none open.** No validation failure of ours in any run. Residual
  risks: (a) the correctness argument is Herlihy et al.'s plus the
  observation that a promotion is a tower insertion under the same locks and
  validation; it is tested, not proved or model-checked; (b) everything ran on
  aarch64, the weaker memory model, so x86 should be the easier target, but
  the x86 build of our code has not been compiled yet (the Chameleon kit's
  `node_build.sh` does that and runs a smoke test); (c) only
  `reclaimer_none` is safe. The structure does not call the record manager's
  `startOp`/`endOp` (nor do the fork's skip lists), so building it with
  `reclaim_debra` would free nodes under concurrent readers. The Makefile
  builds `RECLAIMERS=none` only; do not change that for ours.
- **P1, the splay-list's paths are shorter than ours** in the smoke run
  (13.8 against about 22 at Zipf 1, 23.6 against about 27 at 90/10). Before
  any comparison against the splay-list in the paper: a sweep over K (1, 2, 3)
  and p, the path length against elapsed time from a cold structure (the
  README's own addition), and runs long enough for heights to settle.
- **P1, memory under update mixes.** Erased nodes are never freed (as in every
  fork structure) and ours are 544 bytes. An unpaced 20% insert mix leaked
  about 7 GB in 30 s on 6 threads here. On Chameleon at 128 threads and 40 s
  per run the 80/10/10 mix could reach tens of GB per run for ours (232 bytes
  per node for `skiplist_locks`); check the node's RAM against the insert
  rate before launching the update protocol, or lower `BSL_MAX_HEIGHT` for
  those runs (heights at 10^6 keys stay well under 40).
- **P1, seeds in the Chameleon runner.** `chameleon/run_protocol.py` does not
  set `BSL_SEED`, so all repetitions of ours would replay one height
  sequence. Set `BSL_SEED=<repetition>` for `bsl_*` binaries, as
  `run-smoke.sh` does. The fork's structures have no seed knob, so their
  structural variance (fork finding 5) is invisible in their repetitions.
- **P1, path length against `skiplist_locks`.** Compare ours with
  `bsl_classic` over seeds, not with the single fixed-seed `skiplist_locks`
  structure, which is about 2.4 standard deviations luckier than the mean.
- **P1, lost promotions.** Under contention some promotions are abandoned:
  about 0.3% of requests in the 50%-update stress, a handful per million
  read-only, up to 10 to 30% with the test-only always-promote rule. The
  realized rule under concurrency is therefore the paper's rule thinned by a
  small, contention-dependent probability. Section 8 should state that, or
  claim nothing about it.
- **P1, Morris under concurrency** uses "H <= Q" in place of "H = Q" (identical
  sequentially) and treats a lost CAS as no increment. Section 8 should
  describe the concurrent rule as it is, if it says anything.
- **P1, fork harness bugs** (findings 1 to 3). The upstream SetBench key-sum
  check is not in the fork's drivers; `skiplist_lazy` is wrong under updates
  and should not appear in update-mix figures; `benchmark_write`'s Zipf mode
  is out of bounds (fixed by the Chameleon kit's `harness.patch`, pending
  approval).
- **P1, H_max never shrinks.** Like the fork's `realMaxLvl`, it tracks the
  tallest tower ever built, so under long update runs every search pays for
  towers that are gone. Neither the paper nor the fork does anything about
  it.
- **P2.** `getHeight` returns a number of levels, one more than the fork's
  0-based top index (the benchmark prints `h: 0` regardless). The adapter
  has the fork's unused-parameter warning. The exit report adds two lines to
  the benchmark's output after its results (the authors' parsers read the
  `h: ..., ops: ...` line, which is unchanged); `BSL_EXIT_REPORT=0` removes
  them.

## Lead's verification pass (2026-10-24)

Rebuilt from the files as committed (`build.sh`: 34 binaries, plus 20
assertion, stress and TSan builds), then re-ran `run-store-audit.sh` and
`run-validation.sh quick` with `ONLY=ABCDG` (sections A to D and G at 2 s a
run; no TSan and no long stress). 100 runs. Every run of ours passes: 133M
promotions, 22M abandoned, nearly all of the latter in the always-promote
section D. The 3 failures are the fork's structures: `skiplist_lazy` twice
(finding 2), and `skiplist_locks` once, which lost one inserted key (per-key
accounting: expected 1, found 0; size off by one; the structure otherwise
valid) at 6 threads on Zipf 1. That is the first failure of `skiplist_locks`
in the driver after passing it in every earlier campaign, and it fits finding
4: on aarch64 an unfenced lock release can let a pointer store leave the
critical section, so two inserts under the same predecessor can lose one. It
is the fork's structure, not ours, and the vendored tree is untouched. The
store audit reproduces the count above: 10 memory-writing instructions in
`lookup`, of which 5 stack spills, 2 to `pathLen`, 2 to the generator state
and 1 to `gateOpens` immediately before the call to `promote`.

The raw logs of the campaigns recorded above (`validation-run1.*`,
`validation-run2.*`, `validation-runF.*`, `validation-FG.*`,
`validation-quick-final.*`, `smoke.csv`, `smoke.log`) were erased by this
rebuild: `build.sh all` recreated `.work/`, and the logs lived under it. The
tables in this file are the surviving record of those runs. `build.sh` now
keeps `.work/logs/` across rebuilds. `.work/logs/validation.*` holds the
verification pass, and the smoke measurement was re-run afterwards into
`smoke.csv` and `smoke.log`; its numbers differ from the tables above, since
it is a different run on a machine whose timings are not stable.

## Path split and early miss (2026-10-25)

Two additions to `bsl_skiplist.h`, asked for by the user before the zen3
runs. (1) The exit line splits the fork's path metric: `path_total`,
`path_levels` (levels visited, the vertical span) and `path_steps` (forward
steps), over the whole run including warmup, plus `misses` and
`early_miss`. In the stress runs (10^4 keys, half present, 50 percent
updates) about two thirds of the path is vertical. (2) `BSL_EARLY_MISS`
(default 1): a search about to descend from node x with successor y above
the key reads x's bottom pointer; if it is y, x and y are adjacent at level
1, so the key is absent and the search ends there (Section 6 of the paper:
x points to y on every level up to min(H_x, H_y)). The read is the one the
plain descent would make at level 0, so the answer and its linearization
point are unchanged; a stale read falls through to the normal descent. In
the stress runs 57 to 78 percent of misses end early. The read-only
protocol has no misses (the fork's benchmark looks up inserted keys only),
so this touches the update mix alone.

Validation after the change: `build.sh bsl`, the store audit (`lookup` is
now 200 instructions; its memory-writing instructions are stack spills, the
generator state and the thread's own stats slots, nothing shared), and
`run-validation.sh quick` sections A and G: every run of ours passes (the
per-key accounting checks each `contains` answer, which is what exercises
the early miss). The fork's `skiplist_lazy` fails as always and
`skiplist_locks` lost one insert again (finding 4). One build quirk: over the
Docker bind mount two rebuilt binaries came back without their execute bit
(`rc=126`); `build.sh` now restores it.
