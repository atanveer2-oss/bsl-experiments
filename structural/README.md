# Structural experiments

The in-process harness behind the paper's Table 1 and the structural
figures: a skip list templated on a promotion policy
(`include/bsl/skiplist.hpp`), so every arm shares the search and splice
code and differs only in when a height changes. Nothing here measures time;
the harness counts pointer steps, heights and bits, and the counts are
exact and seeded.

## What each figure and table is drawn from

| In the paper | File | Drawn from | Command |
|---|---|---|---|
| Table 1 (cost over the entropy benchmark and bits per item at n = 10^5) | printed as Markdown | `results/structural_<dist>_n100k.csv` (and `_n10k.csv`, plain `_<dist>.csv` for n = 10^4, 10^3) | `python3 scripts/plot_structural.py results --tables` |
| Figure 1, the height gap `H_x - log_b(1 + f_x)` across items | `results/structural_gap.pdf` | `results/structural_items_zipf1.5.csv`, `results/structural_items_uniform.csv` (one row per item at t = F, from `--items=`) | `python3 scripts/plot_structural.py results` (also writes `structural_cost.pdf`, `structural_kl.pdf`, `structural_depth.pdf`, `structural_space.pdf`) |
| The box figure: top, per-search `cost_t / a_t` (Theorem A) at n = 10^3, 10^4, 10^5; bottom, the tail of the per-run deviation over 200 runs (Theorem C) | `results/structural_box.pdf` | `results/structural_hist_*.csv` (whole-run histograms from `--hist=`) and `results/structural_*_R200.csv` (200 runs, final row each) | `python3 scripts/plot_structural_box.py results --dists zipf1.5,uniform --textwidth 4.2` (the paper shows these two distributions; without `--dists` all four are drawn) |
| The barrier figure, `P[x-block cost / m >= u]` on the sequence of Theorem D | `results/reseed_barrier_tail.pdf` | `results/reseed_barrier_R2000.csv` (R = 2000 runs per n) | `python3 scripts/plot_reseed.py results --only=reseed_barrier_tail.pdf --arms=persistent,reseeded --layout=row --ns=1000` (the paper shows the n = 1000 panel; `--ns=100,1000` draws both) |

`results/structural_summary.md` and `results/reseed_summary.md` are the
records written when the runs were made: the exact commands, the tables and
the reading of them.

## Arms

`classic`: heights drawn at insertion, never changed (the control).
`gated-K1`, `gated-K3`: the paper's rule (Definition 1.1), the
self-referential gate with jump bound K = 1 and K = 3. `exact`, `morris`:
the paper's counter-based estimators (an exact access count, and a Morris
counter). `exact-legacy`, `morris-legacy`: the rules of the earlier RSASL
and RSASLA papers, verbatim. The legacy arms measure no rule this paper
defines or proves; they are named baselines so a reader can see how the
old rules compare. At p = 1/2, where every run here is made, their known
defects (the old exact rule loses `H >= rank` only for p > 1/2, the old
Morris rule fails the crossing tail) do not show, so their numbers are not
evidence about either defect. The barrier run (`reseed`) uses the one-gate
rule, K = 1, persistent and with epochal reseeding.

## Workloads

n keys inserted, then F accesses. Zipf s = 1.5, Zipf s = 1.0, uniform (the
adversarial control, nothing to adapt to) and 90/10 (90 percent of the
accesses to a random 10 percent of the keys, uniform within each part).
Every run draws a fresh random permutation of ranks to keys, so key order
and rank are uncorrelated. Within a run every arm sees the same access
sequence. The barrier sequence: insert n, access each key once, then one
key y m times and another key x m times, with m = n^2 (capped so that F
stays under 5 x 10^6); F = 2n + 2m and the insertions are costed.

## Accounting

Insertions are not costed in the structural runs. Each access costs
vertical span plus horizontal steps as served, before any promotion; a
successful search stops at the item's top level. `Ent(sigma) = F + sum_x
f_x log_b(F / f_x)` over the accesses. `A = sum_t a_t` with `a_t = 1 +
log_b((n + t) / (1 + f_x(t)))`, Theorem A's benchmark with the n insertions
counted as charges and as operations on their items. The columns of the
CSVs are explained in `results/structural_summary.md` ("Columns") and in
the header comment of `bench/structural.cpp`.

## How to run

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
ctest --test-dir build          # sanity and estimators checks
./reproduce.sh                  # rerun, compare with results/, redraw into rerun/
./reproduce.sh --large          # also the n = 10^5 runs (about an hour)
./reproduce.sh --tails          # also the 200-run sets behind the box figure's bottom row (about 50 minutes)
```

`reproduce.sh` runs the commands recorded in `results/structural_summary.md`
and the n = 10^5 commands recorded in the `*_n100k.log` files, then the
R = 2000 barrier run, compares every regenerated CSV with the committed one
(`cmp`, printing IDENTICAL or DIFFERS per file), and redraws the figures
into `rerun/`. The plotting scripts need Python 3 with matplotlib; the
harness needs a C++20 compiler and CMake 3.20 or later, nothing else.

## The machine

The counts are exact and seeded (seed 12345 for `structural`, 20261003 for
`reseed`), so the CSVs do not depend on the machine. The committed CSVs were
produced on an Apple M2 laptop with Apple clang 21 at `-O3`; the six
n = 10^3 and 10^4 commands take about 90 s of CPU in total, the n = 10^5 set
about an hour, the barrier run about 4 minutes on 8 cores.
