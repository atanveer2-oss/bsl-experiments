---
name: add-promotion-policy
description: Add a new promotion rule (a way of choosing and changing node heights) to the structural skip list harness as a policy, with a unit test and a run arm, without changing any committed result. Use when asked to implement, add, try or compare a new promotion rule, estimator, policy or variant of an existing one in structural/.
---

# Add a promotion policy

The skip list in `structural/include/bsl/skiplist.hpp` is templated on a
policy. Every rule is a policy, so every rule runs the same search and
splice code and draws random numbers the same way; that is what makes the
comparisons fair. Never add a second data structure for a rule.

## 1. Write the policy

Add a `struct` to `skiplist.hpp` next to the related rules, implementing
the interface documented at the top of the "Promotion policies" section:

```cpp
struct MyRulePolicy {
  struct State { /* per-item state, empty if none */ };
  double p;                                   // b = 1/p
  explicit MyRulePolicy(double p_) : p(p_) {}
  unsigned initial_height(State&, Rng& rng) const;          // height at insert
  unsigned on_access(State&, unsigned height, Rng& rng);    // new height, >= height
};
```

- Heights never decrease unless the rule is a demoting one; demoting rules
  add `on_pass` (see "Demoting policies" in the same file).
- Use only `rng` for randomness (`rng.geometric0(p)`, `rng.uniform01()`;
  see `rng.hpp`), so runs stay seeded and reproducible.
- If the rule replaces an existing one, keep the old one under a
  `*Legacy`/`*-legacy` name as a baseline; do not edit it.
- Write a comment above the struct that states the rule in words, as the
  existing policies do.

## 2. Test it

Add checks to `structural/bench/estimators.cpp` (rules with an invariant,
such as "height >= rank") or `structural/bench/sanity.cpp` (structural
properties), using the existing `check(cond, "what")` helper. Test what the
rule promises: its invariant at every step, its height distribution against
the law it should follow, and that `list.well_formed()` holds. Then:

```bash
cmake --build structural/build -j8 && ctest --test-dir structural/build --output-on-failure
```

## 3. Add a run arm behind a flag

In `structural/bench/structural.cpp`, arms are listed in `main` as
`run_arm("label", Policy(...), floor, cfg, run)`. Each arm is seeded on its
own, but a new arm adds rows to the output, so **put it behind a flag** the
way `--floor` gates the floor arms: add a field to `Config`, parse it in
`parse_args`, and wrap the new `run_arm` in `if (cfg.my_rule)`. The default
output then stays byte-identical to the committed CSVs.

## 4. Confirm nothing committed changed

Use the `benchmark-runner` agent (or run `QUICK=1 ./reproduce.sh`): every
CSV must still be IDENTICAL. Then run the new arm into `scratch/`
(gitignored; `rerun/` is wiped by every reproduction):

```bash
./structural/build/structural --keys=1000 --accesses=500000 --dist=zipf1.5 \
  --runs=10 --my_rule=1 > scratch/structural_myrule.csv
```

Report the arm's numbers next to `classic` and `gated-K3` from the same
run, and state the command. New results go into `structural/results/`
only when the user asks, with the command added to
`structural/results/structural_summary.md`.
