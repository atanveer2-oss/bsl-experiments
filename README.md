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
EPYC 7763 nodes of Chameleon Cloud, and every row those runs produced.
Throughput is not reproducible bit for bit; the path-length column is
seeded and exact. The fork itself is not vendored: `concurrent/README.md`
gives the commit to clone.

## Where each figure and table comes from

| In the paper | File | Command (from the part's directory) |
|---|---|---|
| Table 1 | `structural/results/structural_*.csv` | `python3 scripts/plot_structural.py results --tables` |
| Figure 1, the height gap | `structural/results/structural_gap.pdf`, from `structural_items_*.csv` | `python3 scripts/plot_structural.py results` |
| Box figure, per-search and per-run ratios | `structural/results/structural_box.pdf`, from `structural_hist_*.csv` and `structural_*.csv` | `python3 scripts/plot_structural_box.py results --dists zipf1.5,uniform --textwidth 4.2` |
| Barrier figure | `structural/results/reseed_barrier_tail.pdf`, from `reseed_barrier_R2000.csv` | `python3 scripts/plot_reseed.py results --only=reseed_barrier_tail.pdf --arms=persistent,reseeded --layout=row --ns=1000` |
| Throughput figure | `concurrent/results/node-*/throughput_tacc_2026-10-26.csv` and `concurrent/results-p50/node-*/throughput_tacc_2026-10-29.csv` | `python3 protocol/plot_submission.py results results-p50 --dists 99-1,zipf1 --textwidth 4.2 --linear-y --max-threads 256 --out results/throughput_gated_classic.pdf` |
| Throughput table (search path, throughput at 64 and 256 threads) | the same CSVs | `python3 protocol/throughput_table.py results results-p50` |
| Throughput, path length and cold-start panels | `concurrent/results/*.pdf`, from the same CSVs and `coldstart_tacc_2026-10-28.csv` | `python3 protocol/plot_throughput.py results --out results` |

Each part's `README.md` says what the arms, workloads and columns are;
`concurrent/MACHINE.md` describes the nodes.

MIT licence, 2026, Adil Tanveer (Illinois Institute of Technology). See `LICENSE`.
