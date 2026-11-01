#!/usr/bin/env bash
# Correctness campaign for the bsl_* structures, run inside the container
# after build.sh:
#
#   docker run --rm --security-opt seccomp=unconfined \
#     -v "$PWD/experiments:/work" bsl-setbench /work/setbench/run-validation.sh [quick]
#
# (seccomp=unconfined is needed only by the ThreadSanitizer runs: TSan calls
# personality(ADDR_NO_RANDOMIZE), which Docker's default profile blocks.)
#
# Every command, its full output and its outcome go to
# $WORK/logs/validation.log; one line per run goes to $WORK/logs/validation.tsv.
# A run passes when it exits 0 and its output carries the expected verdicts
# (stress_result=PASS for the stress driver, bsl_validate=OK from the exit
# hook for the fork's benchmarks). Exit status is the number of failed runs.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${BSL_SETBENCH_WORK:-$HERE/.work}"
MB="$WORK/structs_bench/microbench"
LOGDIR="$WORK/logs"
QUICK="${1:-}"
ONLY="${ONLY:-ABCDEFG}"   # sections to run, e.g. ONLY=F run-validation.sh
want() { [[ "$ONLY" == *"$1"* ]]; }
mkdir -p "$LOGDIR"
LOG="$LOGDIR/validation.log"
TSV="$LOGDIR/validation.tsv"
: > "$LOG"
printf "group\toutcome\tseconds\tops\tgate_opens\tpromotions\taborted\tcommand\n" > "$TSV"
cd "$MB"
S=alloc_new.reclaim_none.pool_none.out
FAILS=0

run() {  # run <group> <kind: stress|bench> <command...>
  local group="$1" kind="$2"; shift 2
  local t0=$(date +%s)
  local out
  out="$("$@" 2>&1)"
  local rc=$?
  local t1=$(date +%s)
  local ok=1
  [ $rc -eq 0 ] || ok=0
  if [ "$kind" = stress ]; then
    grep -q "stress_result=PASS" <<<"$out" || ok=0
  else
    grep -q "bsl_validate=OK" <<<"$out" || ok=0
  fi
  grep -q "WARNING: ThreadSanitizer" <<<"$out" && ok=0
  grep -q "bsl_validate=FAIL" <<<"$out" && ok=0
  local outcome=PASS
  [ $ok -eq 1 ] || { outcome="FAIL(rc=$rc)"; FAILS=$((FAILS + 1)); }
  # Counters: the last bsl_stats line is the run's total.
  local st=$(grep "bsl_stats" <<<"$out" | tail -1)
  local g=$(sed -n 's/.*gate_opens=\([0-9]*\).*/\1/p' <<<"$st")
  local p=$(sed -n 's/.*promotions=\([0-9]*\).*/\1/p' <<<"$st")
  local a=$(sed -n 's/.*aborted=\([0-9]*\).*/\1/p' <<<"$st")
  local ops=$(sed -n 's/.*total_ops=\([0-9]*\).*/\1/p' <<<"$out")
  [ -n "$ops" ] || ops=$(sed -n 's/.*ops: \([0-9]*\),.*/\1/p' <<<"$out")
  {
    echo "=== [$group] $outcome ($((t1 - t0)) s)"
    echo "\$ $*"
    echo "$out"
    echo
  } >> "$LOG"
  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$group" "$outcome" "$((t1 - t0))" "${ops:-}" "${g:-}" "${p:-}" "${a:-}" "$*" >> "$TSV"
  echo "[$group] $outcome $((t1 - t0))s ops=${ops:-} promotions=${p:-} aborted=${a:-} :: $*"
}

if [ "$QUICK" = quick ]; then SECS=2; LONG=20; else SECS=5; LONG=30; fi

if want A; then
# A. Stress driver, update mixes (25% insert, 25% erase, 50% contains), per-key check.
for ds in bsl_gated; do
  for keys in 10000 100000; do
    for dist in "dist uniform" "dist zipf alpha 1" "dist hot hotops 90 hotkeys 10"; do
      for t in 1 2 4 6; do
        run "A-update-$ds" stress ./bin-asserts/stress_$ds.out threads $t secs $SECS keys $keys ins 25 del 25 $dist
      done
    done
  done
done
# A'. The same driver on the control arm and on the fork's skip lists (a check on the driver).
for ds in bsl_classic skiplist_locks skiplist_lazy; do
  for dist in "dist uniform" "dist zipf alpha 1"; do
    run "A-control-$ds" stress ./bin-asserts/stress_$ds.out threads 6 secs $SECS keys 10000 ins 25 del 25 $dist
  done
done
fi

if want B; then
# B. The fork's own update benchmark (benchmark_write), assertion build; the
#    exit hook validates. write 0.2 writeset 0.25 is the authors' mix
#    (count_stats_write.sh): 20% of operations update a quarter of the keys.
for prefill in 10000 100000; do
  # Not "zipf 1": benchmark_write.cpp draws Zipf ranks over [1, prefill] but
  # indexes a vector of the ~prefill/2 prefilled keys (out of bounds, UB).
  for dist in "x 100 y 100" "x 90 y 10" "x 99 y 1"; do
    for t in 1 2 4 6; do
      run "B-fork-write" bench ./bin-asserts/write_bsl_gated.$S prefill $prefill secs $SECS presecs 1 $dist write 0.2 writeset 0.25 threads $t
    done
  done
done
fi

if want C; then
# C. Read-only, hot keys: promotions under contention, fork benchmark and driver.
for prefill in 10000 100000 1000000; do
  for dist in "zipf 1" "x 90 y 10" "x 99 y 1"; do
    run "C-readonly-fork" bench ./bin-asserts/bench_bsl_gated.$S prefill $prefill secs $((SECS * 2)) presecs 1 $dist threads 6
  done
done
for keys in 100 1000; do
  run "C-readonly-driver" stress ./bin-asserts/stress_bsl_gated.out threads 6 secs $SECS keys $keys ins 0 del 0 prefill 1 dist zipf alpha 1
done
fi

if want D; then
# D. Always-promote (test-only rule): every hit attempts a promotion.
run "D-always-update" stress ./bin-asserts/stress_bsl_gated_always.out threads 6 secs $LONG keys 100 ins 10 del 10 dist hot hotops 90 hotkeys 10
run "D-always-update" stress ./bin-asserts/stress_bsl_gated_always.out threads 6 secs $LONG keys 1000 ins 10 del 10 dist zipf alpha 1
run "D-always-readonly" stress ./bin-asserts/stress_bsl_gated_always.out threads 6 secs $SECS keys 100000 ins 0 del 0 prefill 1 dist uniform
fi

if want E; then
# E. ThreadSanitizer (ours, then the fork's skip list as a positive control:
#    its plain-field races should be reported, so that run is expected to FAIL).
if [ -z "$QUICK" ]; then
  run "E-tsan" stress ./bin-asserts/stress_bsl_gated_tsan.out threads 6 secs 10 keys 1000 ins 20 del 20 dist zipf alpha 1
  run "E-tsan" stress ./bin-asserts/stress_bsl_gated_always_tsan.out threads 6 secs 10 keys 200 ins 10 del 10 dist hot hotops 90 hotkeys 10
  run "E-tsan-control-expected-FAIL" stress ./bin-asserts/stress_skiplist_locks_tsan.out threads 4 secs 3 keys 1000 ins 20 del 20 dist zipf alpha 1
  FAILS=$((FAILS - 1))  # the control is supposed to fail; counted back below if it did not
  grep -q "E-tsan-control-expected-FAIL	PASS" "$TSV" && FAILS=$((FAILS + 2))
fi
fi

if want F; then
# F. Long stress: 6 threads, small key range, checked after every round.
#    Under reclaimer_none (the fork's setting) erased nodes are never freed,
#    and at 544 bytes a node an unpaced update mix fills the 7.7 GB VM in
#    about a minute (it did, twice). `insrate 2500` paces each thread to at
#    most 2500 successful inserts and 2500 successful erases per second
#    (about 2.5 GB over a 5-minute run), so every key is still erased and
#    reinserted many times a second while the set stays near half full.
if [ -z "$QUICK" ]; then
  run "F-long" stress ./bin-asserts/stress_bsl_gated.out threads 6 secs 30 rounds 10 keys 1000 ins 10 del 10 insrate 2500 dist zipf alpha 1
  run "F-long" stress ./bin-asserts/stress_bsl_gated.out threads 6 secs 30 rounds 10 keys 100 ins 10 del 10 insrate 2500 dist uniform
  run "F-long-always" stress ./bin-asserts/stress_bsl_gated_always.out threads 6 secs 30 rounds 10 keys 1000 ins 10 del 10 insrate 2500 dist hot hotops 90 hotkeys 10
fi
fi

if want G; then
# G. The counter-based rules (bsl_exact, bsl_morris), after the gated rule
#    passed A-F. validate() also checks the rule's invariant (H >= rank(f),
#    H >= Q) and fails on a violation unless a promotion was abandoned;
#    with one thread none can be, so the 1-thread runs check it strictly.
for ds in bsl_exact bsl_morris; do
  for dist in "dist uniform" "dist zipf alpha 1" "dist hot hotops 90 hotkeys 10"; do
    for t in 1 2 4 6; do
      run "G-update-$ds" stress ./bin-asserts/stress_$ds.out threads $t secs $SECS keys 10000 ins 25 del 25 $dist
    done
  done
  for t in 1 6; do
    run "G-fork-write-$ds" bench ./bin-asserts/write_$ds.$S prefill 100000 secs $SECS presecs 1 x 90 y 10 write 0.2 writeset 0.25 threads $t
    run "G-readonly-fork-$ds" bench ./bin-asserts/bench_$ds.$S prefill 100000 secs $SECS presecs 1 zipf 1 threads $t
  done
  if [ -z "$QUICK" ]; then
    run "G-tsan-$ds" stress ./bin-asserts/stress_${ds}_tsan.out threads 6 secs 10 keys 1000 ins 20 del 20 dist zipf alpha 1
    run "G-long-$ds" stress ./bin-asserts/stress_$ds.out threads 6 secs 30 rounds 4 keys 1000 ins 10 del 10 insrate 2500 dist zipf alpha 1
  fi
done
fi

echo
echo "failed runs: $FAILS   (log: $LOG, table: $TSV)"
exit $FAILS
