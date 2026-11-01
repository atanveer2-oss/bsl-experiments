#!/usr/bin/env bash
# Build the SetBench microbenchmarks inside the Linux container.
# Applies the portability patch to a COPY, never to the vendored tree, and
# copies our own structures (experiments/setbench/ds/*) into the copy's
# structs_bench/ds/ so the Makefile's ds/*/adapter.h glob builds them too.
#
#   build.sh          fresh copy, every structure (baselines and ours), plus
#                     the assert-enabled builds and the stress driver
#   build.sh bsl      refresh only our structures and the stress driver in an
#                     existing work tree, and rebuild just those (fast loop)
#
# Outputs, under $WORK/structs_bench/microbench/:
#   bin/{bench,write}_<ds>.alloc_new.reclaim_none.pool_none.out   (-DNDEBUG, as the fork)
#   bin-asserts/{bench,write}_<bsl ds>...out                      (assertions on)
#   bin-asserts/stress_<ds>.out                                    (assertions on)
#
# Extra compiler flags reach every binary through XARGS (the Makefile's xargs),
# e.g. XARGS="-DBSL_K=2" build.sh bsl.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../third_party/splaylist"
WORK="${BSL_SETBENCH_WORK:-$HERE/.work}"
MODE="${1:-all}"
JOBS="$(nproc)"
XARGS="${XARGS:-}"
MB="$WORK/structs_bench/microbench"

# Our structures, and the baselines the stress driver is also built for (as a
# check on the driver itself).
OURS=$(cd "$HERE/ds" && ls -d */ | tr -d / | tr '\n' ' ')
STRESS_BASELINES="skiplist_locks skiplist_lazy"
SUFFIX=alloc_new.reclaim_none.pool_none.out

copy_ours() {
  for d in $OURS; do
    rm -rf "$WORK/structs_bench/ds/$d"
    cp -r "$HERE/ds/$d" "$WORK/structs_bench/ds/$d"
  done
  cp "$HERE/stress/stress.cpp" "$MB/bsl_stress.cpp"
}

if [ "$MODE" = all ]; then
  # Recreate the build copy but keep $WORK/logs: the validation and smoke
  # campaigns write their evidence there, and a rebuild must not erase it.
  if [ -d "$WORK/logs" ]; then
    keep="$(mktemp -d "${TMPDIR:-/tmp}/bsl-logs.XXXXXX")"
    mv "$WORK/logs" "$keep/logs"
  fi
  rm -rf "$WORK" && mkdir -p "$WORK"
  if [ -n "${keep:-}" ]; then mv "$keep/logs" "$WORK/logs" && rmdir "$keep"; fi
  cp -r "$SRC/structs_bench" "$WORK/"
  ( cd "$WORK" && patch -p1 --batch --silent < "$HERE/portability.patch" ) \
    || { echo "patch failed -- the fork may have moved under it"; exit 1; }
  # The fork ships 14 prebuilt x86-64 ubench_* binaries from an older harness
  # in microbench/bin. They are not built from this tree and do not run on
  # aarch64; drop them so bin/ holds only what this script built.
  rm -rf "$MB/bin"
  copy_ours
  cd "$MB"
  make -j"$JOBS" has_libpapi=0 has_libnuma=0 xargs="$XARGS" all
elif [ "$MODE" = bsl ]; then
  [ -d "$MB" ] || { echo "no work tree at $WORK; run build.sh first"; exit 1; }
  copy_ours
  cd "$MB"
  targets=""
  for d in $OURS; do targets="$targets bench_$d.$SUFFIX write_$d.$SUFFIX"; done
  make -j"$JOBS" has_libpapi=0 has_libnuma=0 xargs="$XARGS" $targets
else
  echo "usage: build.sh [all|bsl]"; exit 2
fi

# Assertion-enabled builds of ours, in their own directory.
targets=""
for d in $OURS; do targets="$targets bench_$d.$SUFFIX write_$d.$SUFFIX"; done
make -j"$JOBS" has_libpapi=0 has_libnuma=0 use_asserts=1 bin_dir=bin-asserts xargs="$XARGS" $targets

# Stress driver (assertions on), same flags as the Makefile otherwise.
CXXFLAGS="-std=c++14 -O2 -g -fno-omit-frame-pointer -fopenmp -DMAX_THREADS_POW2=256 -DCPU_FREQ_GHZ=2.1"
CXXFLAGS="$CXXFLAGS -DMEMORY_STATS=if(1) -DMEMORY_STATS2=if(0) -DPREFILL_INSERTION_ONLY"
CXXFLAGS="$CXXFLAGS -DALLOC_TYPE=new -DRECLAIM_TYPE=none -DPOOL_TYPE=none $XARGS"
INCS="-I./ -I../ $(find ../common -type d | sed s/^/-I/)"
mkdir -p bin-asserts
pids=""
for d in $OURS $STRESS_BASELINES; do
  g++ bsl_stress.cpp -o "bin-asserts/stress_$d.out" -I"../ds/$d" -DDS_TYPENAME="$d" $CXXFLAGS $INCS -lpthread -ldl &
  pids="$pids $!"
done
# Test-only variants of ours: every hit promotes (BSL_STRESS_ALWAYS_PROMOTE),
# and ThreadSanitizer builds (-O1, as TSan recommends).
g++ bsl_stress.cpp -o bin-asserts/stress_bsl_gated_always.out -I../ds/bsl_gated -DDS_TYPENAME=bsl_gated \
  -DBSL_STRESS_ALWAYS_PROMOTE $CXXFLAGS $INCS -lpthread -ldl &
pids="$pids $!"
for d in bsl_gated bsl_exact bsl_morris skiplist_locks; do
  [ -d "../ds/$d" ] || continue
  g++ bsl_stress.cpp -o "bin-asserts/stress_${d}_tsan.out" -I"../ds/$d" -DDS_TYPENAME="$d" \
    ${CXXFLAGS/-O2/-O1} -fsanitize=thread $INCS -lpthread -ldl &
  pids="$pids $!"
done
g++ bsl_stress.cpp -o bin-asserts/stress_bsl_gated_always_tsan.out -I../ds/bsl_gated -DDS_TYPENAME=bsl_gated \
  -DBSL_STRESS_ALWAYS_PROMOTE ${CXXFLAGS/-O2/-O1} -fsanitize=thread $INCS -lpthread -ldl &
pids="$pids $!"
for p in $pids; do wait "$p"; done
# Over a Docker bind mount on macOS a binary written over an existing file can
# come back without its execute bit; make sure every output is runnable.
chmod +x bin/*.out bin-asserts/*.out 2>/dev/null || true

echo
echo "built $(ls bin | wc -l) binaries in $MB/bin and $(ls bin-asserts | wc -l) in $MB/bin-asserts"
