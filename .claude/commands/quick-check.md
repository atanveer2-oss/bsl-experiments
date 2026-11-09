---
description: Build the structural harness, run the unit tests and the quick reproduction, and report in five lines.
---

Current state of the tree:
!`git status --short | head -20`

Do the quick check of the structural part, in this order, stopping at the
first failure:

1. `cmake -S structural -B structural/build -DCMAKE_BUILD_TYPE=Release` (only
   if `structural/build/CMakeCache.txt` is missing), then
   `cmake --build structural/build -j8`.
2. `ctest --test-dir structural/build --output-on-failure`.
3. `QUICK=1 ./reproduce.sh` and collect its IDENTICAL / DIFFERS lines.

Report in at most five lines: build (ok / failed), tests (passed n of m),
reproduction (n CSVs identical, list any that differ), uncommitted changes
that could explain a difference, and the next step if anything failed. Do
not edit any file.
