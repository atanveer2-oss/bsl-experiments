# Experiments for the counter-free self-adjusting skip list paper

Two parts. `structural/` is a small C++20 harness, with no dependencies,
that counts what the theorems bound: search cost against the entropy
benchmark, the height of an item against the logarithm of its access
count, the top level, the depth, the across-items hazard, and bits per
item, for the paper's promotion rule, its counter-based variants, the
classic skip list on the same code and the rules of two earlier papers.
Its counts are exact and seeded; `structural/reproduce.sh` reruns every
command and checks that the regenerated CSVs are byte-identical to the
committed ones. It also holds the persistence-barrier run, which measures
the tail of the cost of the persistent process against epochal reseeding.

`concurrent/` is the paper's skip list behind the adapter interface of
Aksenov's SetBench fork (the repository that carries the splay-list and the
CBTree), the correctness campaign that checked it, the protocol under which
throughput and search path length were measured on four dual-socket AMD
EPYC 7763 nodes of Chameleon Cloud, every row those runs produced, and
the scripts, logs and per-run driver output of the campaign that ran them
(`concurrent/campaign/`).
Throughput is not reproducible bit for bit; the path-length column is
seeded and exact. The fork itself is not vendored: `concurrent/README.md`
gives the commit to clone.

This branch also carries a configuration for Claude Code, the coding
agent used to build and run these experiments: `CLAUDE.md` and `.claude/`
(settings, hooks, subagents, skills, commands), explained in
`docs/HARNESS.md`.

## Reproduce in one command

```
./reproduce.sh
```

builds the structural harness, reruns the n = 10^3 and 10^4 runs and the
2000-run barrier set, compares every regenerated CSV byte for byte with the
committed one, and draws the paper's three figures and two tables into
`rerun/paper/`. About ten minutes. `./reproduce.sh --large` adds the
n = 10^5 runs (an hour), `--tails` the 200-run sets (50 minutes),
`QUICK=1 ./reproduce.sh` does only the n = 10^3 runs (a minute). It needs
a C++20 compiler, CMake 3.20 or later and Python 3 with matplotlib; with
none of those installed, the same in a container:

```
docker run --rm -v "$PWD:/work" -w /work debian:bookworm bash -c \
  'apt-get update -qq && apt-get install -y -qq g++ cmake python3 python3-matplotlib >/dev/null && ./reproduce.sh'
```

The committed CSVs were produced on an Apple M2 with Apple clang; the
container route (Debian 12, GCC 12, aarch64) reproduces them byte for byte.
The throughput figure and table are redrawn from the committed rows; the
throughput runs themselves need the machine in `concurrent/MACHINE.md`, and
`concurrent/README.md` says how to rerun the protocol on other hardware.

## Where each figure and table comes from

| In the paper | File | Command (from the part's directory) |
|---|---|---|
| The cost table: total cost over the entropy benchmark and bits per item at n = 10^5 | `structural/results/structural_*_n100k.csv` | `python3 scripts/plot_structural.py results --tables` |
| The height-gap figure (drawn, not in the paper) | `structural/results/structural_gap.pdf`, from `structural_items_*.csv` | `python3 scripts/plot_structural.py results` |
| Box figure, per-search and per-run ratios | `structural/results/structural_box.pdf`, from `structural_hist_*.csv` and `structural_*.csv` | `python3 scripts/plot_structural_box.py results --dists zipf1.5,uniform --textwidth 4.2` |
| Barrier figure | `structural/results/reseed_barrier_tail.pdf`, from `reseed_barrier_R2000.csv` | `python3 scripts/plot_reseed.py results --only=reseed_barrier_tail.pdf --arms=persistent,reseeded --layout=row --ns=1000` |
| Throughput figure | `concurrent/results/node-*/throughput_tacc_2026-10-26.csv` and `concurrent/results-p50/node-*/throughput_tacc_2026-10-29.csv` | `python3 protocol/plot_submission.py results results-p50 --dists 99-1,zipf1 --textwidth 4.2 --linear-y --max-threads 256 --out results/throughput_gated_classic.pdf` |
| Throughput table (search path, throughput at 64 and 256 threads) | the same CSVs | `python3 protocol/throughput_table.py results results-p50` |
| Throughput, path length and cold-start panels | `concurrent/results/*.pdf`, from the same CSVs and `coldstart_tacc_2026-10-28.csv` | `python3 protocol/plot_throughput.py results --out results` |

Each part's `README.md` says what the arms, workloads and columns are;
`concurrent/MACHINE.md` describes the nodes. The paper's figure and table
numbers are not repeated here because they move between drafts; the rows
above name each by content.

## Requirements and what a rerun reproduces

`structural/`: a C++20 compiler, CMake 3.20 or later, and Python 3 with
matplotlib for the figures. Every CSV is exact and seeded, so a rerun on
any machine is expected to reproduce it byte for byte (`reproduce.sh`
checks); the default run takes a few minutes, the full set about two hours.
`concurrent/`: Linux with Docker (or a native g++ toolchain), the SetBench
fork at the pinned commit, and a many-core machine for anything beyond
correctness. The committed throughput rows cannot be reproduced bit for bit
anywhere; the figures and table are redrawn from them exactly, the protocol
can be rerun on other hardware with `protocol/run_protocol.py`, and the
correctness campaign (`run-validation.sh`) reruns in the container.

## Changes after the commit the paper cites

The paper cites commit `6d7c430`. In that commit the node-side build,
`concurrent/protocol/node_build.sh`, still looked for `build.sh` and
`harness.patch` where they sat in the working tree the campaign ran from,
so rerunning the throughput protocol stopped at the build step. The
commits after it (`git log 6d7c430..`) fix those paths and the default
folders of the runner and the plot scripts, say in `concurrent/README.md`
how to rerun the protocol, and add the campaign's scripts and records.
Nothing measured changed: the CSVs, figures and tables of `6d7c430`,
all of `structural/` apart from added run logs, and `concurrent/ds/` are
unchanged; the records add the sanity run's rows (`concurrent/results-sanity/`). `git checkout 6d7c430` gives the cited version.

## Licence

MIT licence, 2026, Adil Tanveer (Illinois Institute of Technology). See `LICENSE`.
