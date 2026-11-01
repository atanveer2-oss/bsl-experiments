#!/usr/bin/env bash
# Rebuild the structural harness, rerun the paper's commands into ./rerun/,
# compare every regenerated CSV with the committed one in ./results/, and
# redraw the figures into ./rerun/.
#
#   ./reproduce.sh            the n = 10^3 and 10^4 structural runs (about
#                             3 minutes of CPU: each command runs twice, once
#                             exactly as recorded and once with --hist/--items
#                             for the histogram and per-item files) and the
#                             reseeding barrier run (about 4 minutes on 8 cores)
#   ./reproduce.sh --large    also the four n = 10^5 runs (about an hour)
#   ./reproduce.sh --tails    also the 200-run sets at n = 10^3 and 10^4 that
#                             the bottom row of the box figure is drawn from
#                             (about 50 minutes)
#   QUICK=1 ./reproduce.sh    only the four n = 10^3 runs (about a minute)
#
# The counts are exact and seeded (seed 12345 for structural, 20261003 for
# reseed), so every structural CSV is expected to come out IDENTICAL on any
# machine. The barrier CSV is compared on its first 18 columns: the
# committed file predates six diagnostic columns the harness has since
# added (topup_work and later), and those are not part of any figure.
set -euo pipefail
cd "$(dirname "$0")"

LARGE=0
TAILS=0
for a in "$@"; do
  case "$a" in
    --large) LARGE=1 ;;
    --tails) TAILS=1 ;;
    *) echo "usage: $0 [--large] [--tails]   (QUICK=1 for the n = 10^3 runs only)" >&2; exit 2 ;;
  esac
done
QUICK="${QUICK:-0}"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
rm -rf rerun
mkdir rerun

ST=./build/structural
verdicts=()
check() {  # check <name> [columns]: compare rerun/<name> with results/<name>
  local f="$1" cols="${2:-}" v
  if [ -n "$cols" ]; then
    if cmp -s <(cut -d, -f1-"$cols" "rerun/$f") <(cut -d, -f1-"$cols" "results/$f"); then
      v="IDENTICAL (first $cols columns)"; else v="DIFFERS (first $cols columns)"; fi
  elif cmp -s "rerun/$f" "results/$f"; then v=IDENTICAL; else v=DIFFERS; fi
  printf '%-44s %s\n' "$f" "$v"; verdicts+=("$f $v")
}

# --- n = 10^3: the four commands of results/structural_summary.md, verbatim
echo "== structural, n = 10^3"
for d in zipf1.5 zipf1.0 uniform hot90-10; do
  $ST --keys=1000 --accesses=500000 --dist=$d --runs=10 --sample_every=10000 --seed=12345 \
    > rerun/structural_$d.csv 2> rerun/structural_$d.log
  check structural_$d.csv
done
# the same runs again with --hist (the whole-run histogram of cost_t / a_t,
# for the box figure) and, for Zipf 1.5 and uniform, --items (one row per item
# at t = F, for the height-gap figure). Neither flag changes the main CSV,
# which this pass discards (it was compared above).
for d in zipf1.5 zipf1.0 uniform hot90-10; do
  items=""
  case $d in zipf1.5|uniform) items="--items=rerun/structural_items_$d.csv" ;; esac
  $ST --keys=1000 --accesses=500000 --dist=$d --runs=10 --sample_every=10000 --seed=12345 \
    --hist=rerun/structural_hist_$d.csv $items > /dev/null 2>> rerun/structural_$d.log
  check structural_hist_$d.csv
  [ -n "$items" ] && check structural_items_$d.csv
done

if [ "$QUICK" != 1 ]; then
  # --- n = 10^4: ten runs per distribution, one pass each with --hist (the
  # flag does not change the main CSV). Zipf 1.5 and uniform were first run
  # with 5 and 3 runs (structural_summary.md); the committed files are the
  # 10-run reruns of 2026-11-01, whose first runs are identical.
  echo "== structural, n = 10^4"
  for d in zipf1.5 zipf1.0 hot90-10; do
    $ST --keys=10000 --accesses=2000000 --dist=$d --runs=10 --sample_every=10000 --seed=12345 \
      --hist=rerun/structural_hist_${d}_n10k.csv > rerun/structural_${d}_n10k.csv 2> rerun/structural_${d}_n10k.log
    check structural_${d}_n10k.csv
    check structural_hist_${d}_n10k.csv
  done
  $ST --keys=10000 --accesses=5000000 --dist=uniform --runs=10 --sample_every=50000 --seed=12345 \
    --hist=rerun/structural_hist_uniform_n10k.csv > rerun/structural_uniform_n10k.csv 2> rerun/structural_uniform_n10k.log
  check structural_uniform_n10k.csv
  check structural_hist_uniform_n10k.csv

  if [ "$TAILS" = 1 ]; then
    # --- the deviation tails (bottom row of the box figure): 200 runs per
    # distribution at n = 10^3 and 10^4, final row only. About 50 minutes.
    echo "== structural, 200-run sets for the deviation tails"
    for d in zipf1.5 zipf1.0 uniform hot90-10; do
      $ST --keys=1000 --accesses=500000 --dist=$d --runs=200 --sample_every=500000 --seed=12345 \
        > rerun/structural_${d}_R200.csv 2> rerun/structural_${d}_R200.log
      check structural_${d}_R200.csv
      $ST --keys=10000 --accesses=2000000 --dist=$d --runs=200 --sample_every=2000000 --seed=12345 \
        > rerun/structural_${d}_n10k_R200.csv 2> rerun/structural_${d}_n10k_R200.log
      check structural_${d}_n10k_R200.csv
    done
  fi

  if [ "$LARGE" = 1 ]; then
    # --- n = 10^5: the parameters printed on the first line of each
    # results/structural_<dist>_n100k.log; one pass each, with --hist.
    echo "== structural, n = 10^5 (about an hour)"
    for d in zipf1.5 zipf1.0 hot90-10 uniform; do
      $ST --keys=100000 --accesses=50000000 --dist=$d --runs=10 --sample_every=5000000 --seed=12345 \
        --hist=rerun/structural_hist_${d}_n100k.csv > rerun/structural_${d}_n100k.csv 2> rerun/structural_${d}_n100k.log
      check structural_${d}_n100k.csv
      check structural_hist_${d}_n100k.csv
    done
  fi

  # --- the persistence barrier, R = 2000 runs per n (n = 100 and 1000), the
  # one-gate rule persistent and reseeded. Defaults of bench/reseed.cpp:
  # p = 0.5, seed 20261003, every hardware thread; the per-run seed depends
  # only on the base seed, the run index and n, so the thread count does not
  # change the numbers. --barrier_arms restricts the run to the two arms the
  # committed CSV holds (the clocked-ceiling arms were added to the harness
  # later and are not in the paper).
  echo "== reseed, barrier (about 4 minutes on 8 cores)"
  ./build/reseed --only=barrier --barrier_runs=2000 --suffix=_R2000 \
    --barrier_arms=gated1,gated1+reseed --out=rerun \
    > rerun/reseed_barrier_R2000.md 2> rerun/reseed_barrier_R2000.log
  check reseed_barrier_R2000.csv 18
fi

# --- figures and tables, drawn from rerun/
echo "== figures"
python3 scripts/plot_structural.py rerun                       # cost, KL, depth, space, gap PDFs
python3 scripts/plot_structural.py rerun --tables > rerun/structural_tables.md
if [ "$TAILS" != 1 ]; then cp results/structural_*_R200.csv rerun/ 2>/dev/null || true; fi  # committed tails
python3 scripts/plot_structural_box.py rerun --out rerun/structural_box.pdf
if [ -f rerun/reseed_barrier_R2000.csv ]; then
  # the paper's barrier figure: the two arms side by side (the harness also
  # has a clocked-ceiling arm, not in the paper)
  python3 scripts/plot_reseed.py rerun --only=reseed_barrier_tail.pdf \
    --arms=persistent,reseeded --layout=row --out=rerun/reseed_barrier_tail.pdf
fi

echo
echo "== summary"
printf '%s\n' "${verdicts[@]}"
if printf '%s\n' "${verdicts[@]}" | grep -q DIFFERS; then
  echo "some CSVs differ from results/"; exit 1
fi
echo "every regenerated CSV matches results/"
