#!/usr/bin/env bash
# Smoke measurement (a sanity check, not a result): the README protocol
# shortened to 3 s warmup + 5 s measured, 100% lookup, 10^5 keys, uniform,
# 90/10 and Zipf 1, threads 1 2 4 6, 3 repetitions. Run inside the
# container after build.sh:
#
#   docker run --rm -v "$PWD:/work" bsl-setbench /work/concurrent/run-smoke.sh
#
# Writes $WORK/logs/smoke.csv (one row per run) and $WORK/logs/smoke.log
# (every command line and its full output). The fork's benchmark.cpp prints
# "ops" as the operations completed in the measured window and "sumLengths"
# as the mean path length per operation over that window.
#
# The splay-list and the CBTree take the authors' restructuring period
# `cops`; their scaling scripts (count_stats_2.sh, count_stats_zipf.sh) use
# 100*T and 1000*T. We use cops = 100*T for both. Ours ignore it.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${BSL_SETBENCH_WORK:-$HERE/.work}"
MB="$WORK/structs_bench/microbench"
LOGDIR="$WORK/logs"
mkdir -p "$LOGDIR"
CSV="$LOGDIR/smoke.csv"
LOG="$LOGDIR/smoke.log"
: > "$LOG"
echo "structure,dist,threads,rep,bsl_seed,presecs,secs,ops,mops_per_s,path_length,promotions,hmax" > "$CSV"
cd "$MB"
S=alloc_new.reclaim_none.pool_none.out
PRESECS="${PRESECS:-3}"
SECS="${SECS:-5}"
REPS="${REPS:-3}"
STRUCTS="${STRUCTS:-bsl_gated bsl_classic skiplist_locks flexlist_locks cbtree-concurrent}"
THREADS="${THREADS:-1 2 4 6}"

for rep in $(seq 1 "$REPS"); do
  for dist in uniform 90-10 zipf1; do
    case $dist in
      uniform) dargs="x 100 y 100" ;;
      90-10) dargs="x 90 y 10" ;;
      zipf1) dargs="zipf 1" ;;
    esac
    for t in $THREADS; do
      for ds in $STRUCTS; do
        cargs=""
        case $ds in flexlist*|cbtree*) cargs="cops $((100 * t))" ;; esac
        cmd="./bin/bench_$ds.$S prefill 100000 presecs $PRESECS secs $SECS $dargs $cargs threads $t"
        # Ours draw heights from BSL_SEED; the fork's structures have fixed
        # seeds, so their repetitions differ only in timing.
        out="$(BSL_SEED=$rep $cmd 2>&1)"
        { echo "\$ BSL_SEED=$rep $cmd"; echo "$out"; echo; } >> "$LOG"
        ops=$(sed -n 's/.*ops: \([0-9]*\),.*/\1/p' <<<"$out")
        len=$(sed -n 's/.*sumLengths: \([0-9.e+-]*\).*/\1/p' <<<"$out")
        st=$(grep bsl_stats <<<"$out" | tail -1)
        prom=$(sed -n 's/.*promotions=\([0-9]*\).*/\1/p' <<<"$st")
        hm=$(sed -n 's/.* hmax=\([0-9]*\).*/\1/p' <<<"$st")
        if grep -q "bsl_validate=FAIL" <<<"$out"; then echo "VALIDATION FAILURE: $cmd"; fi
        mops=$(python3 -c "print(f'{${ops:-0}/$SECS/1e6:.3f}')")
        echo "$ds,$dist,$t,$rep,$rep,$PRESECS,$SECS,${ops:-},$mops,${len:-},${prom:-},${hm:-}" >> "$CSV"
        echo "$ds $dist T=$t rep=$rep: $mops Mops/s, path $len ${prom:+promotions $prom}"
      done
    done
  done
done
echo "wrote $CSV"
