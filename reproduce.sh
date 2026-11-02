#!/usr/bin/env bash
# One command for everything a reader can reproduce from this repository.
#
#   ./reproduce.sh              structural runs at n = 10^3 and 10^4 and the
#                               2000-run barrier set, each regenerated CSV
#                               compared byte for byte with the committed one;
#                               then the paper's three figures and two tables,
#                               drawn into rerun/paper/ (about ten minutes)
#   ./reproduce.sh --large      also the n = 10^5 runs (about an hour more)
#   ./reproduce.sh --tails      also the 200-run sets (about 50 minutes more)
#   QUICK=1 ./reproduce.sh      only the n = 10^3 runs (about a minute)
#
# Needs a C++20 compiler, CMake 3.20 or later, and Python 3 with matplotlib.
# Without any of those installed, the same thing in a container (Linux or
# macOS with Docker):
#
#   docker run --rm -v "$PWD:/work" -w /work debian:bookworm bash -c \
#     'apt-get update -qq && apt-get install -y -qq g++ cmake python3 python3-matplotlib >/dev/null && ./reproduce.sh'
#
# The throughput figure and table are redrawn from the committed CSVs; the
# throughput runs themselves need the machine described in
# concurrent/MACHINE.md (see concurrent/README.md to rerun the protocol).
set -euo pipefail
cd "$(dirname "$0")"
OUT=rerun/paper
rm -rf rerun
mkdir -p "$OUT"

echo "#### structural: build, rerun, compare (structural/reproduce.sh $*)"
(cd structural && ./reproduce.sh "$@")

echo
echo "#### the paper's figures and tables into $OUT/ (regenerated structural CSVs"
echo "     where this run made them, committed ones otherwise; committed throughput rows)"
( cd structural
  # whatever this run did not regenerate (the 200-run sets without --tails,
  # the n = 10^5 runs without --large, everything but n = 10^3 under QUICK=1)
  # is drawn from the committed files, so the figures are always complete
  for f in results/*.csv; do [ -e "rerun/$(basename "$f")" ] || cp "$f" rerun/; done
  python3 scripts/plot_structural_box.py rerun --dists zipf1.5,uniform --textwidth 4.2 --out "../$OUT/structural_box.pdf"
  python3 scripts/plot_reseed.py rerun --only=reseed_barrier_tail.pdf --arms=persistent,reseeded --layout=row --ns=1000 --out="../$OUT/reseed_barrier_tail.pdf"
  python3 scripts/plot_structural.py rerun --tables > "../$OUT/structural_tables.md"
)
( cd concurrent
  python3 protocol/plot_submission.py results results-p50 --dists 99-1,zipf1 --textwidth 4.2 --linear-y --max-threads 256 --out "../$OUT/throughput_gated_classic.pdf"
  python3 protocol/throughput_table.py results results-p50 > "../$OUT/throughput_table.tex"
)
echo
echo "#### done"
ls -1 "$OUT"
echo "The cost table (Table 3 in the submission) is the n = 10^5 block of $OUT/structural_tables.md;"
echo "it comes from the committed n = 10^5 CSVs unless --large reran them."
