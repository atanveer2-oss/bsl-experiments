# Concurrent measurement

The paper's skip list behind the adapter interface of Aksenov's SetBench
fork (the repository that carries the splay-list and the CBTree), the
protocol it was measured under on Chameleon Cloud, and the rows those runs
produced.

## What is here

| Path | What |
|---|---|
| `ds/bsl_gated/bsl_skiplist.h` | the concurrent structure and its four promotion rules (gated, classic, exact, Morris) |
| `ds/bsl_gated/bsl_adapter.h` | the SetBench adapter, shared by the four variants |
| `ds/bsl_gated/`, `ds/bsl_classic/`, `ds/bsl_exact/`, `ds/bsl_morris/` | one `adapter.h` per variant; the fork's Makefile builds a `bench_` and a `write_` binary for each directory under `ds/` |
| `stress/stress.cpp` | concurrent stress driver with per-key accounting, for any adapter |
| `stress/store_audit.cpp`, `run-store-audit.sh` | disassembly audit of the stores on the search path |
| `portability.patch` | builds the fork on Linux/aarch64 as well as x86-64 (a no-op on x86), plus one fix to the fork's assertion build |
| `harness.patch` | threads above 99, a `seed` argument, a Zipf fix in the update driver; no algorithm code |
| `build.sh`, `Dockerfile` | the Linux build, native or in an Ubuntu 22.04 container |
| `run-validation.sh`, `run-smoke.sh` | the correctness campaign and a short local measurement |
| `RESULTS-local.md` | the correctness record: what was checked and what came out |
| `protocol/PROTOCOL.md` | the measurement protocol and the reasons for each choice |
| `protocol/run_protocol.py` | the resumable runner that produced every CSV row |
| `protocol/node_build.sh` | the node-side build (packages, `build.sh`, `harness.patch`, build record, smoke test) |
| `protocol/estimate.py`, `protocol/progress.py` | node-hours and wall-clock for a parameter set; progress and preliminary medians from collected rows |
| `protocol/plot_throughput.py`, `protocol/plot_submission.py` | the figures; `protocol/throughput_table.py` prints the paper's throughput table (search path, throughput at 64 and 256 threads); `protocol/results_json.py` summarises the rows as JSON |
| `results/node-1` to `node-4` | `throughput_tacc_2026-10-26.csv` and `coldstart_tacc_2026-10-28.csv`, one shard per node |
| `results-p50/node-1` to `node-4` | `throughput_tacc_2026-10-29.csv`, the rerun at p = 1/2, K = 3 |
| `results/node-*/BUILD_INFO`, `bsl-run.log`, `bsl-run-commands.log` | per node: the build record, the runner's console output and the exact command started |
| `results-sanity/` | the one-node sanity run of 25 October that checked the setup and chose p = 1/4, K = 1 for the main sweep |
| `driver-logs.tar.gz` | the benchmark driver's full output for every row (the CSVs' `log` column); `campaign/README.md` says how to unpack it |
| `campaign/` | the laptop-side Chameleon scripts (reserve, provision, start, collect, the scheduled steps) and how the campaign ran |
| `results/*.pdf` | throughput, path length and cold-start figures drawn from the rows |
| `MACHINE.md` | the machine, build flags, timing and dates, from the CSV columns |

## The SetBench fork

The fork is not vendored. `build.sh` expects it at `../third_party/splaylist`
relative to this directory, so clone it once at the recorded commit:

```
git clone https://github.com/Aksenov239/splaylist third_party/splaylist
git -C third_party/splaylist checkout f6b6757
```

`build.sh` never modifies that clone. It copies `structs_bench/` into a work
directory (`.work/` here, or `$BSL_SETBENCH_WORK`), applies
`portability.patch` to the copy, adds the four `ds/` directories, and
builds. `node_build.sh` then applies `harness.patch` to the same copy and
rebuilds. The container route, from the directory above this one:

```
docker build -t bsl-setbench concurrent
docker run --rm -v "$PWD:/work" bsl-setbench /work/concurrent/build.sh
```

(`build.sh` resolves the fork and the work directory relative to its own
location, so the mount point does not matter.) The build record of the
measured binaries is in `MACHINE.md`; the compile-time parameters `BSL_P`
and `BSL_K` reach every binary through `XARGS`, for example
`XARGS="-DBSL_P=0.25 -DBSL_K=1" ./build.sh`.

## Protocol

`protocol/PROTOCOL.md` states the protocol and why each choice was made:
distributions uniform, 90/10, 95/5, 99/1 and Zipf 1 over shuffled keys;
key ranges 10^6 and 10^5; a read-only mix and an 80/10/10 update mix; 20 s
warmup and 20 s measured per run (10 s and 10 s for the update mix), 10
repetitions with distinct seeds; throughput and mean search path length
recorded for every run; and a cold-start series, throughput in windows of
elapsed time from a fresh prefill. `protocol/run_protocol.py` runs it and
writes one CSV row per run; every row records the machine, the build, the
command line and the raw output it was parsed from. The sweep was split
over four nodes, so each node directory holds one quarter of the rows and
the plotting scripts pool them:

```
python3 protocol/plot_throughput.py results --out results
python3 protocol/plot_submission.py results results-p50 --dists 99-1,zipf1 --textwidth 4.2 --linear-y --max-threads 256 --out results/throughput_gated_classic.pdf
# the paper's two panels, linear throughput axis, up to 256 hardware threads; without --dists, --linear-y and --max-threads the three distributions are drawn log-log up to 128 threads
```

## Rerunning the protocol on another machine

Run these from the repository root (the directory holding `README.md`,
`structural/` and `concurrent/`), on Linux, after cloning the fork as in
"The SetBench fork" above. The paper's machine is x86-64; aarch64 works
with the system allocator.

**Build.** Our structure's p and K are fixed at compile time, through the
environment variable `XARGS` (compiler flags passed to every binary):

```
XARGS="-DBSL_P=0.25 -DBSL_K=1" bash concurrent/protocol/node_build.sh --install
```

`-DBSL_P=0.25 -DBSL_K=1` is the setting of `results/`; without `XARGS` the
build is p = 1/2, K = 3, the setting of `results-p50/`. `--install` first
installs the Debian/Ubuntu packages listed at the top of the script (it
uses `sudo` unless run as root); leave it out if they are present. The
binaries go to `~/bsl-build` (or `$BSL_SETBENCH_WORK`), and the script
ends with a 2-second smoke test.

**A first, short run** (four structures, two distributions, 1 and 4
threads, one repetition of 1 s + 1 s; a few minutes):

```
python3 concurrent/protocol/run_protocol.py --structures bsl_gated,bsl_classic,skiplist_locks,flexlist_locks \
  --distributions zipf1,uniform --key-ranges 100000 --mixes 100-0-0,80-10-10 \
  --threads 1,4 --reps 1 --warmup 1 --duration 1
```

**The full protocol:**

```
python3 concurrent/protocol/estimate.py --nodes 1         # how long it takes on one machine
python3 concurrent/protocol/run_protocol.py --dry-run | tail -3   # the last commands and the planned row count
python3 concurrent/protocol/run_protocol.py               # resumable: run it again to continue
```

Rows go to a new folder, `concurrent/results-rerun/`, unless
`--results DIR` names another; the committed `results*/` folders are never
written. Each flag of the protocol has a default (see `--help`): five
distributions (uniform; 90/10, 95/5, 99/1, meaning that share of the
operations goes to that share of the keys; Zipf 1), key ranges 10^6 and
10^5, the mixes `100-0-0` (all lookups) and `80-10-10` (80 percent
lookups, 10 inserts, 10 erases), threads 1 to 128, ten repetitions of
20 s + 20 s. `--smt` adds 256 threads, for a machine with 256 hardware
threads (two per core); thread counts above the machine's CPU count are
dropped with a warning. `--cold-start` runs the cold-start series instead:
throughput in windows of time after a fresh prefill, which shows how fast
the structures adapt.

**Two machine settings.** The paper's runs preload the fork's jemalloc
(`--allocator jemalloc`, the default), which is an x86-64 binary: elsewhere
pass `--allocator system`. They interleave memory over the NUMA nodes with
`numactl` (`--numa interleave`, the default); containers usually forbid
that, so pass `--numa none` there. Both choices are recorded in every row.

**In a container**, from the repository root (the image is Ubuntu 22.04
with the build packages):

```
docker build -t bsl-setbench concurrent
docker run --rm -v "$PWD:/work" -e BSL_SETBENCH_WORK=/tmp/bsl-build bsl-setbench bash -c \
  'bash /work/concurrent/protocol/node_build.sh && python3 /work/concurrent/protocol/run_protocol.py \
   --allocator system --numa none --structures bsl_gated,bsl_classic,skiplist_locks,flexlist_locks \
   --distributions zipf1,uniform --key-ranges 100000 --threads 1,4 --reps 1 --warmup 1 --duration 1'
```

The build lives inside the container and is discarded with it; the rows
land in `concurrent/results-rerun/` on the host.

On Chameleon, `campaign/` holds the scripts that did all of this over SSH
on four nodes.

## What the stress tests check

`RESULTS-local.md` records the correctness campaign, run in the container
with assertions on. The stress driver (`stress/stress.cpp`) runs mixed
inserts, erases and lookups from several threads against any adapter and
checks, after the run, that every key's final presence equals the prefill
plus each thread's successful inserts minus its successful erases, that
`contains` agrees, that the key sum and the size match, that the values
returned by `insertIfAbsent` and `erase` are the values stored, and that
the adapter's `validateStructure` passes (which, for the exact and Morris
rules, includes the rule's invariant). It was run on uniform, Zipf and
hot-set workloads, on an always-promote build that forces a promotion on
every hit, and under ThreadSanitizer; the fork's own benchmarks were run
with a validating exit hook; and `run-store-audit.sh` disassembles the
gated rule's `contains` to show that the search path writes nothing but
stack spills and thread-private counters. The same campaign was run
against two of the fork's skip lists as a check on the driver.

## What to expect from a rerun

Throughput is not reproducible bit for bit: it depends on the node, the
kernel, the allocator and the moment. The medians and ranges over the 10
repetitions in the CSVs are the reference. The `path_length` column is
seeded and exact: with one thread, a rerun with the same binary, `seed` and
arguments gives the same value; with several threads it varies only through
the interleaving of concurrent promotions.
