#!/usr/bin/env bash
# Store audit of the search path: compile bsl_gated's contains() out of line
# with the benchmark's flags (-O2 -DNDEBUG), disassemble it, and print every
# instruction that can write memory. Each must be a stack spill or a
# thread-private slot (the path-length counter or the thread's generator);
# the promotion itself is the out-of-line call to promote(). Run inside the
# container after build.sh:
#
#   docker run --rm -v "$PWD/experiments:/work" bsl-setbench /work/setbench/run-store-audit.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${BSL_SETBENCH_WORK:-$HERE/.work}"
MB="$WORK/structs_bench/microbench"
cd "$MB"
cp "$HERE/stress/store_audit.cpp" bsl_store_audit.cpp
mkdir -p bin-asserts
g++ bsl_store_audit.cpp -o bin-asserts/store_audit_bsl_gated.out -I../ds/bsl_gated -DDS_TYPENAME=bsl_gated \
  -DNDEBUG -std=c++14 -O2 -g -fno-omit-frame-pointer -fopenmp -DMAX_THREADS_POW2=256 -DCPU_FREQ_GHZ=2.1 \
  "-DMEMORY_STATS=if(1)" "-DMEMORY_STATS2=if(0)" -DALLOC_TYPE=new -DRECLAIM_TYPE=none -DPOOL_TYPE=none \
  -I./ -I../ $(find ../common -type d | sed s/^/-I/) -lpthread -ldl
BSL_EXIT_REPORT=0 ./bin-asserts/store_audit_bsl_gated.out && echo "audit binary runs: OK"
objdump -d -C -l --no-show-raw-insn bin-asserts/store_audit_bsl_gated.out \
  | awk '/^[0-9a-f]+ <.*GatedRule>::lookup\(int/{f=1} f&&/^$/{exit} f' > bin-asserts/store_audit.dis || true  # awk exits early: SIGPIPE
echo "instructions in SkipList<..., GatedRule>::lookup (the search path behind contains/find): $(grep -cE '^[[:space:]]+[0-9a-f]+:' bin-asserts/store_audit.dis)"
echo "memory-writing instructions (with source lines):"
case "$(uname -m)" in
  aarch64) pat='^[[:space:]]+[0-9a-f]+:[[:space:]]+(st[a-z0-9]*|cas[a-z]*|ldadd[a-z]*|ldset[a-z]*|ldclr[a-z]*|ldeor[a-z]*|swp[a-z]*)[[:space:]]' ;;
  x86_64)  pat='^[[:space:]]+[0-9a-f]+:[[:space:]]+(mov[a-z]*[[:space:]]+[^,]+,.*\(|lock|xchg|cmpxchg|push|add[a-z]*[[:space:]]+[^,]+,.*\(|inc[a-z]*[[:space:]]+.*\()' ;;
esac
awk -v pat="$pat" '/^\//{src=$0} $0 ~ pat {print "  " $0 "    <- " src}' bin-asserts/store_audit.dis
echo "calls:"
grep -E '[[:space:]](bl|call)[[:space:]]' bin-asserts/store_audit.dis | sed 's/^/  /'
