#!/usr/bin/env bash
# Rebuild, from this repository, the directory tree the Chameleon campaign ran
# from, so that the scripts in this directory run unmodified.
#
#   concurrent/campaign/make-original-layout.sh [DEST]   (default: ./original-layout)
#
# The campaign's laptop-side scripts find each other, and the files they copy
# to the nodes, by relative path inside one directory called experiments/:
#
#   experiments/setbench/                 concurrent/ (build.sh, ds/, patches, stress/)
#   experiments/chameleon/                concurrent/protocol/ (runner, node build,
#                                         plots), concurrent/harness.patch and
#                                         concurrent/campaign/
#   experiments/results/chameleon/        the rows of concurrent/results/
#   experiments/results/chameleon-p50/    the rows of concurrent/results-p50/
#   experiments/results/chameleon-zen3-sanity/  the rows of concurrent/results-sanity/
#   experiments/third_party/splaylist     the SetBench fork (clone it; not vendored)
#
# Everything is copied; the repository itself is left as it is.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONCURRENT="$(cd "$HERE/.." && pwd)"
DEST="${1:-$PWD/original-layout}"
E="$DEST/experiments"
[[ -e "$E" ]] && { echo "make-original-layout.sh: $E exists; remove it or pass another DEST" >&2; exit 1; }

mkdir -p "$E/setbench" "$E/chameleon" "$E/results" "$E/third_party"

for f in ds stress build.sh Dockerfile portability.patch harness.patch run-smoke.sh \
         run-store-audit.sh run-validation.sh README.md RESULTS-local.md; do
  cp -R "$CONCURRENT/$f" "$E/setbench/"
done

cp "$CONCURRENT"/protocol/*.py "$CONCURRENT/protocol/node_build.sh" \
   "$CONCURRENT/harness.patch" "$E/chameleon/"
for f in "$HERE"/*; do
  [[ "$(basename "$f")" == make-original-layout.sh ]] || cp "$f" "$E/chameleon/"
done

cp -R "$CONCURRENT/results" "$E/results/chameleon"
rm -f "$E"/results/chameleon/*.pdf
cp -R "$CONCURRENT/results-p50" "$E/results/chameleon-p50"
cp -R "$CONCURRENT/results-sanity" "$E/results/chameleon-zen3-sanity"

cat <<EOF
original layout written to $E
Clone the SetBench fork before building:
  git clone https://github.com/Aksenov239/splaylist "$E/third_party/splaylist"
  git -C "$E/third_party/splaylist" checkout f6b6757
Then work from $E/chameleon (see concurrent/campaign/README.md).
EOF
