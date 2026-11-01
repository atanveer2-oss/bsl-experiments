#!/usr/bin/env bash
# Node-side half of provision.sh: build the SetBench binaries natively and
# check them. provision.sh runs this over SSH after rsyncing experiments/;
# it can also be run by hand on the node, or in the local container.
#
#   bash experiments/chameleon/node_build.sh [--install] [--papi]
#        [--no-harness-patch] [--commit SHA] [--smoke-only]
#
# Steps:
#   1. (--install) apt packages: build-essential g++ make numactl libnuma-dev
#      python3 rsync tmux, and libpapi-dev with --papi.
#   2. setbench/build.sh into $BSL_SETBENCH_WORK (default ~/bsl-build). It
#      copies the fork, applies portability.patch (a no-op on x86), adds our
#      structures from setbench/ds/ and builds. $XARGS (e.g. "-DBSL_K=2")
#      reaches every binary, as in build.sh.
#   3. Unless --no-harness-patch: apply chameleon/harness.patch to that copy
#      and rebuild (threads up to 256, a `seed` argument, the update driver's
#      Zipf fix; see the patch header).
#   4. Write BUILD_INFO (commit, patch state, compiler, date), which
#      run_protocol.py reads.
#   5. Check that every ds/ directory produced a bench_ and a write_ binary,
#      and run one 2-second smoke test of a baseline.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXPERIMENTS="$(cd "$HERE/.." && pwd)"
WORK="${BSL_SETBENCH_WORK:-$HOME/bsl-build}"
export BSL_SETBENCH_WORK="$WORK"

install=0 papi=0 harness=1 commit="" smoke_only=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --install) install=1 ;;
    --papi) papi=1 ;;
    --no-harness-patch) harness=0 ;;
    --commit) commit="$2"; shift ;;
    --smoke-only) smoke_only=1 ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "node_build.sh: unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

SUDO=""
[[ $(id -u) -ne 0 ]] && SUDO="sudo"

if [[ $install -eq 1 ]]; then
  pkgs=(build-essential g++ make numactl libnuma-dev python3 rsync tmux)
  [[ $papi -eq 1 ]] && pkgs+=(libpapi-dev)
  $SUDO env DEBIAN_FRONTEND=noninteractive apt-get update -q
  $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends "${pkgs[@]}"
fi

MB="$WORK/structs_bench/microbench"
if [[ $smoke_only -eq 0 ]]; then
  echo "== building into $WORK"
  bash "$EXPERIMENTS/setbench/build.sh"
  if [[ $harness -eq 1 ]]; then
    echo "== applying chameleon/harness.patch and rebuilding"
    ( cd "$WORK" && patch -p1 --batch --silent < "$HERE/harness.patch" ) \
      || { echo "node_build.sh: harness.patch failed to apply; the fork or build.sh moved under it" >&2; exit 1; }
    # The Makefile's targets are not file names, so `make all` rebuilds every binary.
    ( cd "$MB" && make -j"$(nproc)" has_libpapi=0 has_libnuma=0 xargs="${XARGS:-}" all >/dev/null )
  fi
  {
    echo "commit=${commit:-unknown}"
    echo "harness_patch=$harness"
    echo "built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host=$(hostname)"
    echo "arch=$(uname -m)"
    echo "compiler=$(g++ --version | head -1)"
    echo "make_flags=has_libpapi=0 has_libnuma=0"
    echo "xargs=${XARGS:-}"
    # third_party/ is gitignored, so the commit does not pin the fork.
    echo "fork_digest=$(cd "$EXPERIMENTS/third_party/splaylist" && find structs_bench/ds structs_bench/common structs_bench/microbench -maxdepth 2 -type f \( -name '*.h' -o -name '*.cpp' -o -name Makefile \) -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16)"
  } > "$WORK/BUILD_INFO"
fi

echo "== checking binaries"
expected=0
missing=()
for adapter in "$WORK"/structs_bench/ds/*/adapter.h; do
  ds="$(basename "$(dirname "$adapter")")"
  expected=$((expected + 2))
  for kind in bench write; do
    f="$MB/bin/${kind}_${ds}.alloc_new.reclaim_none.pool_none.out"
    [[ -x "$f" ]] || missing+=("$(basename "$f")")
  done
done
found=$(find "$MB/bin" -maxdepth 1 \( -name 'bench_*.out' -o -name 'write_*.out' \) | wc -l)
echo "found $found bench_/write_ binaries, expected $expected (2 per ds/ directory)"
if [[ ${#missing[@]} -gt 0 ]]; then
  printf 'missing: %s\n' "${missing[@]}" >&2
  exit 1
fi
for want in bsl_gated bsl_exact bsl_morris; do
  [[ -x "$MB/bin/bench_${want}.alloc_new.reclaim_none.pool_none.out" ]] \
    || echo "note: no bench_${want} binary yet (expected while the structure is in development)"
done

echo "== smoke test: skiplist_locks, 1 s warmup + 1 s measured, 4 threads"
pre=()
jem="$WORK/structs_bench/lib/libjemalloc.so"
if [[ -f "$jem" ]] && [[ -z "$(env LD_PRELOAD="$jem" /bin/true 2>&1)" ]]; then
  pre=(env "LD_PRELOAD=$jem")
else
  echo "note: $jem cannot be preloaded here (expected off x86_64); smoke test uses the system allocator"
fi
if command -v numactl >/dev/null && numactl --interleave=all /bin/true 2>/dev/null; then
  pre+=(numactl --interleave=all)
else
  echo "note: numactl --interleave=all is not permitted here (containers); smoke test runs without it"
fi
out="$("${pre[@]}" "$MB/bin/bench_skiplist_locks.alloc_new.reclaim_none.pool_none.out" \
        prefill 100000 presecs 1 secs 1 threads 4 x 90 y 10)"
echo "$out" | tail -3
echo "$out" | grep -Eq 'ops: [1-9][0-9]*' || { echo "node_build.sh: smoke test produced no operations" >&2; exit 1; }
echo "== ok. BUILD_INFO:"
cat "$WORK/BUILD_INFO"
