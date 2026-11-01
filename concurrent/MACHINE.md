# The machine behind the concurrent results

Every row of every CSV under `results/` and `results-p50/` carries the
columns below, so the description here can be checked against the data.

**Nodes.** Four bare-metal `compute_zen3` nodes of Chameleon Cloud at
CHI@TACC, used one shard of the protocol each. They are `node-1` to
`node-4` in this repository (the directory under `results/` and
`results-p50/`); the CSV column `host` holds the names the leases gave them.

| | from the CSV columns |
|---|---|
| CPU (`cpu_model`) | AMD EPYC 7763 64-Core Processor, two per node (`sockets` = 2) |
| cores, hardware threads | 128 cores, 256 hardware threads (`cores`, `hw_threads`; `threads_per_core` = 2, SMT on) |
| NUMA | 2 nodes (`numa_nodes`); memory interleaved over both with `numactl --interleave=all` (`numa_policy`) |
| kernel | Linux 5.15.0-185-generic x86_64 (Ubuntu 22.04 image) |
| CPU governor | `performance` |
| pinning (`pin_layout`) | `cores-first;sockets=0>1`: worker i is pinned to CPU i by the benchmark driver itself; thread counts up to 128 fill the physical cores of socket 0 then socket 1, 256 adds the SMT siblings |
| allocator | jemalloc, preloaded (`allocator`) |
| compiler | g++ 11.4.0 (Ubuntu 22.04), the fork's Makefile flags, `has_libpapi=0 has_libnuma=0` |
| SetBench fork | Aksenov's `splaylist` repository at commit `f6b6757`, with `portability.patch` (a no-op on x86) and `harness.patch` (`harness_patch` = 1: threads above 99, a `seed` argument, a Zipf fix in the update driver) applied to the build copy |
| `commit` | the commit of the authors' experiment scripts at build time (`efb6935` for the main sweep and cold start, `b3d3055`-dirty for the p = 1/2 rerun); the fork itself is pinned by its commit above, not by this column |

**Build flags (`build_xargs`).** Main sweep and cold start: `-DBSL_P=0.25
-DBSL_K=1`. The p = 1/2 rerun under `results-p50/`: `-DBSL_P=0.5 -DBSL_K=3`,
which are the adapter's compile-time defaults, so its `build_xargs` column
is empty and the `p` and `K` columns (and `ds_stats`) record the values.

**Timing.** Read-only mix (`100-0-0`): 20 s warmup, 20 s measured
(`warmup_s`, `duration_s`). Update mix (`80-10-10`): 10 s and 10 s. Cold
start: no warmup; each window `[a, b]` in seconds since the prefill is its
own process, with windows 0-1, 1-2, 2-5, 5-10, 10-20, 20-60 and 60-120
(`window_start_s`, `window_end_s`).

**Repetitions.** 10 per configuration in the main sweep and the cold start
(`rep` 0 to 9, each with its own `seed`), 5 in the p = 1/2 rerun.

**Thread counts.** 1, 2, 4, 8, 16, 32, 48, 64, 96, 128 and 256.

**Sweep.** Structures `bsl_gated` (the paper's rule), `bsl_classic` (the
control, same code), `bsl_exact`, `bsl_morris`, and the fork's
`flexlist_locks` (splay-list), `cbtree-concurrent`,
`cbtree-concurrent-fair`, `skiplist_locks`, `skiplist_lazy`,
`bronson_pext_bst_occ`; distributions uniform, 90/10, 95/5, 99/1 and Zipf 1
over shuffled keys; key ranges 10^6 and 10^5; mixes read-only and
80/10/10 (the CBTrees cannot erase, so they are absent from the update
mix). 19 800 throughput rows and 7 000 cold-start rows in `results/`,
2 200 rows in `results-p50/`.

**Dates (`started_utc`).** Main sweep 2026-10-26 to 10-28; cold start
2026-10-28 to 10-29; p = 1/2 rerun 2026-10-29.

**What is not reproducible.** Throughput (`ops`, `throughput_ops_s`,
`wall_s`, `peak_rss_mb`) depends on the machine and the moment. The
`path_length` column (mean search path length per operation) and the
`ds_stats` counters are determined by the seed and the structure, up to the
interleaving of concurrent promotions, and are exact for one thread.
