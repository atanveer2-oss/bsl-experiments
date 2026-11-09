---
name: reproduce-structural
description: Check that the committed structural results still reproduce (rerun the seeded experiments, compare every CSV byte for byte, redraw the figures). Use whenever the user asks whether results reproduce, are unchanged or still match, after any change to structural/ code, and before a commit.
---

# Reproduce the structural results

The structural runs are exact and seeded, so a rerun on any machine must
give the same bytes. `reproduce.sh` at the repository root does the whole
check; this skill says which variant to run and how to read the output.

## Choose the size

| Command | What it reruns | Time |
|---|---|---|
| `QUICK=1 ./reproduce.sh` | n = 10^3 runs only | about 1 minute |
| `./reproduce.sh` | n = 10^3 and 10^4, the 2000-run barrier set | about 10 minutes |
| `./reproduce.sh --large` | adds n = 10^5 | about 1 hour |
| `./reproduce.sh --tails` | adds the 200-run sets | about 50 minutes |

Use the quick run after a small change, the default before a commit, and
the long ones only when asked (run them in the background).

## Before running

Both `reproduce.sh` (root) and `structural/reproduce.sh` start by deleting
their output folders (`rerun/`, `structural/rerun/`). Those folders hold
only regenerated copies, so this is expected; if anything else was put
there, move it to `scratch/` first.

## Read the output

- Each regenerated CSV prints `IDENTICAL` or `DIFFERS`; the run ends with
  "every regenerated CSV matches results/" when all agree. Anything else
  is a failure: report the file and the first differing lines
  (`diff <(head -50 structural/rerun/X.csv) <(head -50 structural/results/X.csv)`).
- The paper's figures and tables are redrawn into `rerun/paper/`; they are
  not compared byte for byte (PDFs embed dates), so look at them only if a
  CSV differed.
- A `DIFFERS` after a code change means the change altered seeded
  behaviour. Find out why before anything else; never "fix" it by copying
  the new CSV over the committed one (a hook blocks that).

## Requirements

C++20 compiler, CMake 3.20 or later, Python 3 with matplotlib. Without
them, the same check runs in a container:

```bash
docker run --rm -v "$PWD:/work" -w /work debian:bookworm bash -c \
  'apt-get update -qq && apt-get install -y -qq g++ cmake python3 python3-matplotlib >/dev/null && QUICK=1 ./reproduce.sh'
```
