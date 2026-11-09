---
name: concurrent-validation
description: Build the concurrent skip list inside the SetBench fork in the Linux container and run its correctness campaign (stress tests with per-key accounting, the fork's validation hook, ThreadSanitizer). Use after any change under concurrent/ds/, concurrent/stress/ or the patches, and before any throughput measurement.
---

# Validate the concurrent skip list

Our structure lives in `concurrent/ds/` (four variants: `bsl_gated`,
`bsl_classic`, `bsl_exact`, `bsl_morris`) and is built inside Aksenov's
SetBench fork. The fork needs Linux, so everything here runs in the
container defined by `concurrent/Dockerfile`.

## 0. Prerequisites

```bash
git clone https://github.com/Aksenov239/splaylist third_party/splaylist   # once
git -C third_party/splaylist checkout f6b6757
docker build -t bsl-setbench concurrent
```

## 1. Build

```bash
docker run --rm -v "$PWD:/work" bsl-setbench /work/concurrent/build.sh
```

`build.sh` copies the fork into `concurrent/.work/` (never modifying the
clone), applies `portability.patch`, adds our `ds/` directories and builds
the benchmark binaries and the assertion-enabled stress binaries. Expect
two binaries (`bench_`, `write_`) per `ds/` directory.

## 2. Run the correctness campaign

```bash
docker run --rm --security-opt seccomp=unconfined -v "$PWD:/work" bsl-setbench \
  /work/concurrent/run-validation.sh quick        # minutes; drop "quick" for the full campaign
```

`seccomp=unconfined` is needed only by the ThreadSanitizer runs. The exit
status is the number of failed runs; the per-run table is
`concurrent/.work/logs/validation.tsv`.

## 3. Read the result

- Every run of our four structures must PASS. Any failure there is a bug:
  hand the log entry and the diff to the `concurrency-reviewer` agent.
- Two controls are **expected** to fail and are not ours: `skiplist_lazy`
  (the fork's structure never unlinks; RESULTS-local.md, fork finding 2),
  and on ARM hosts `A-control-skiplist_locks` intermittently (the fork
  relies on x86 ordering; finding 4).
- The store audit (`run-store-audit.sh`) shows that a non-promoting search
  writes nothing shared; rerun it when the search path changes.

Report the counts (passed / failed / expected failures) and, for any
unexpected failure, the command and its output.
