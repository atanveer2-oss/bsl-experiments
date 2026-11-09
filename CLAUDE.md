# bsl-experiments: instructions for Claude Code

Experiments for the counter-free self-adjusting skip list paper. Read this
first; `docs/HARNESS.md` explains the rest of the `.claude/` setup.

## Layout

| Path | What it is |
|---|---|
| `structural/` | C++20 header-only harness, no dependencies. Counts what the theorems bound: search cost against the entropy benchmark, heights, depth, hazard, bits per item. Exact and seeded. |
| `structural/include/bsl/skiplist.hpp` | The skip list, templated on a **promotion policy**. Every rule (classic, gated, the counter estimators, the legacy rules) is a policy on this one structure. |
| `structural/bench/` | `structural.cpp` (the main runs), `reseed.cpp` (barrier experiment), `sanity.cpp` and `estimators.cpp` (the tests behind `ctest`). |
| `structural/results/` | Committed CSVs and figures. **Read-only** (see Rules). |
| `concurrent/` | Our skip list behind the SetBench adapter interface (`ds/`), patches, stress tests, the protocol runner (`protocol/`), the Chameleon campaign (`campaign/`) and the committed throughput rows (`results*/`). |
| `third_party/splaylist` | Aksenov's SetBench fork, cloned at `f6b6757`; not vendored, gitignored. |

## Commands

```bash
# structural: build and test (about a minute)
cmake -S structural -B structural/build -DCMAKE_BUILD_TYPE=Release
cmake --build structural/build -j8
ctest --test-dir structural/build --output-on-failure

# reproduce: rerun and compare every CSV byte for byte (QUICK=1: n = 10^3 only, a minute)
QUICK=1 ./reproduce.sh

# concurrent: build in the Linux container (the fork pins threads with
# pthread_setaffinity_np, which exists only on Linux)
docker build -t bsl-setbench concurrent
docker run --rm -v "$PWD:/work" bsl-setbench /work/concurrent/build.sh
```

The skills in `.claude/skills/` hold the longer procedures (adding a rule,
reproducing, validating the concurrent code, running on Chameleon).

## Rules

1. **A new rule is a new policy**, never a new data structure. Policies
   implement `initial_height` and `on_access` (interface at the top of
   `skiplist.hpp`), so every rule shares the search and splice code and
   comparisons stay fair. Keep superseded rules as named baselines
   (`*-legacy`) rather than editing them.
2. **Committed results are never edited.** Regenerate into `rerun/` and
   compare; a CSV under `results/` changes only by an intentional rerun
   whose command is recorded in the results summary. A hook enforces this.
   `rerun/` and `structural/rerun/` are scratch: both reproduce scripts
   delete them first. Output of a *new* experiment goes in `scratch/`
   (gitignored, never cleared by a script) until the user asks to commit it.
3. **Seeded runs must reproduce byte for byte.** If a change alters a
   committed CSV, either the change is a bug or the results must be
   regenerated deliberately; say which.
4. **Cost accounting follows the paper**: vertical levels descended plus
   horizontal forward steps; a successful search stops at the item's top
   level. Do not change it to match another paper's metric.
5. **Every number in prose comes from a CSV.** Before stating a number in a
   README or summary, recompute it from the file (the `results-auditor`
   agent does this). Name the file and the command.
6. **Cloud runs cost real allocation.** Nothing that creates or extends a
   Chameleon lease runs without the user's explicit go in this session.
   Credentials live in `~/.config/chameleon/` and never enter the repo;
   hooks and permissions block reading or committing them.
7. **Concurrent code is reviewed for memory ordering.** Shared fields are
   `std::atomic` with acquire/release; the fork's own structures rely on
   x86 ordering and race on ARM (`concurrent/RESULTS-local.md`, finding 4).
   Use the `concurrency-reviewer` agent for any change under `concurrent/ds/`.
8. **Results are machine-specific only where stated.** Structural counts are
   exact anywhere; throughput depends on the machine in
   `concurrent/MACHINE.md` and is never compared across machines.

## Style

Plain declarative prose in READMEs: say what a file is, what it measures,
and the command that produces it. No claims a CSV does not support.
