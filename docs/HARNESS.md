# The Claude Code harness for this repository

This repository holds the experiments for the paper on counter-free
self-adjusting skip lists, and it carries a working configuration for
Claude Code, Anthropic's command-line coding agent: the files that tell
the agent how to work here, what it may and may not do, and which
specialised helpers and procedures it can call on. It uses the standard
project layout, so it doubles as an example of how such a harness is put
together. Most of it was tested in live sessions; "How it was tested"
lists what was and what was not.

Three terms used below. Claude models come in tiers: **Opus** (largest,
used for careful review), **Sonnet** (mid-size) and **Haiku** (smallest
and cheapest). A **promotion policy** is the rule that decides when a
skip list item gets a taller tower; each rule the paper studies (such as
`gated-K3`, the paper's rule with K = 3) is one policy on the same skip
list. **Chameleon** is a research cloud of bare-metal machines; a *lease*
reserves nodes for a time window and is charged against the project's
allocation whether the nodes are busy or not.

## The layout

```
CLAUDE.md                     project instructions, read at the start of every session
.claude/
  settings.json               permissions (allow / ask / deny) and hook wiring
  hooks/                      scripts Claude Code runs at fixed points in a session
    session_context.py          at session start: branch, build state, pending reruns
    protect_results.py          before a tool call: committed results are read-only
    block_secrets.py            before a shell command: credentials stay out
    build_and_test.py           after an edit to C++: rebuild and run the tests
    test_hooks.py               tests for the hooks (run from the repository root)
  agents/                     subagents: separate assistants, each with its own model, tools and instructions
    results-auditor.md          recomputes every number in a text from the CSVs (Opus, read-only)
    concurrency-reviewer.md     reviews the concurrent code for races and memory ordering (Opus)
    benchmark-runner.md         builds, reruns into rerun/, compares byte for byte (Sonnet)
    cold-reader.md              a first-time reader for documentation (Haiku, sees one file only)
  skills/                     procedures the agent loads when a request matches their description
    add-promotion-policy/       a new rule as a policy, with a test, without changing results
    reproduce-structural/       rerun and check that every CSV is identical
    concurrent-validation/      container build and correctness campaign
    chameleon-campaign/         cloud runs: estimate, rehearse, gate, launch, collect, release
  commands/                   slash commands the user types
    quick-check.md              /quick-check: build, tests, quick reproduction, five-line report
    audit-numbers.md            /audit-numbers <file>: hand a document to the results-auditor
docs/HARNESS.md               this file
```

## How a session uses it

1. **Start.** Claude Code reads `CLAUDE.md` (the layout, the commands and
   eight numbered rules, such as "a new rule is a new policy" and
   "committed results are never edited") and runs the start hook, which adds a short status
   of the working tree to the agent's context.
2. **A request arrives.** The agent sees one line describing each skill,
   subagent and command. If the request matches a skill ("do the results
   still reproduce?"), it loads that skill's full procedure.
3. **Every tool call is checked twice.** First the permission rules in
   `settings.json`: *deny* wins over *ask*, which wins over *allow*. Reading
   the cloud credentials or the SSH keys, editing committed results, force
   pushes and `rm -rf` are denied; commits, pushes, container runs and
   anything that reserves cloud nodes need the user's approval each time;
   builds, tests and read-only git run without asking. Then the
   pre-tool hooks run, catching what a rule cannot express, such as a
   shell command that copies a new CSV over a committed one.
4. **After an edit** to the C++ harness, the post-edit hook rebuilds and
   runs the unit tests. If either fails, the agent is told at once, with
   the error, and must fix it before going on.
5. **Independent checks** go to subagents, which start with no knowledge
   of the conversation: the auditor recomputes numbers from the raw rows,
   the reviewer reads the concurrent code, the cold reader sees one file.
   Their independence is the point: they do not inherit the main agent's
   assumptions.

## What mirrors how the experiments were done, and what is added here

The experiments were prepared and run with Claude Code between September
and November 2026, in a private working repository whose configuration
also covered the writing of the paper. This harness keeps the experiment
side and writes it in the standard form. In the table, **mirrored** means
the piece puts into a standard file a practice that was actually used in
that work (the files themselves are new); **added** means the piece was
not used then and is here to show the standard structure.

| Piece | Status | What it corresponds to |
|---|---|---|
| `CLAUDE.md` rules | mirrored | The working repository's instructions: rules as policies, paper's cost accounting, credentials outside the repository. |
| `results-auditor` agent | mirrored | Before the experiments section was handed over, two independent Opus agents recomputed every number from the CSVs and checked every setup claim; they reported 12 and 21 items, all fixed (counts from the working repository's log of 1 November). |
| `cold-reader` agent | mirrored | Every new piece of text was cold-read by a Sonnet or Haiku agent given only that text. |
| `benchmark-runner` agent | mirrored | The structural runs were done by a helper agent, checked and committed by the lead session. |
| `concurrency-reviewer` agent | mirrored, as a dedicated agent | The concurrent implementation was written by one helper agent, then reviewed and its stress campaign rerun by the lead session. |
| `chameleon-campaign` skill | mirrored | The October 2026 campaign: estimate, a cheap rehearsal lease that caught a provisioning bug, ten go/no-go gates, then the main run on four nodes. |
| `concurrent-validation`, `reproduce-structural` skills | mirrored | The container build and correctness campaign, and `reproduce.sh`. |
| `add-promotion-policy` skill | mirrored | How the paper's estimators were added (as policies, legacy rules kept as baselines, unit tests first). |
| permission lists in `settings.json` | added | The original work ran in Claude Code's automatic approval mode (a user setting, in which a built-in safety check approves routine actions and stops risky ones); the lists here spell the same policy out explicitly and apply to anyone who opens the repository. |
| hooks, slash commands | added | Not used in the original work; added to show the standard pieces. |

## How it was tested

On 8 November 2026, in this repository, with Claude Code 2.1.310:

- **Hooks, offline.** `python3 .claude/hooks/test_hooks.py` feeds each hook
  the JSON Claude Code sends: 20 of 20 cases pass (blocked and allowed
  edits and commands, a build and test after a header edit). A separate
  run with a deliberately broken header returned the compiler error as a
  blocking message.
- **Live sessions** (`claude -p`, project settings only):
  - a new session reported the start-hook status, the four subagents, the
    four skills, the two commands and the `CLAUDE.md` rules;
  - a command copying a file over a committed CSV was stopped by the
    `protect_results.py` hook, and an edit to a committed summary was
    stopped by a deny rule; the tree was unchanged;
  - `/quick-check` built, ran the tests (2 of 2) and the quick
    reproduction (10 of 10 CSVs identical) using only allowed commands;
  - asked to break a header, it received the build failure from the
    post-edit hook; after the fix the hook stayed silent;
  - asked "do the committed results still reproduce?" without naming a
    skill, a Sonnet session loaded `reproduce-structural` by itself; a
    Haiku session ran the command listed in `CLAUDE.md` directly (one
    session each). Skills are chosen by the model from their
    descriptions, so whether one is used depends on the model and on how
    well the description matches the request;
  - an earlier version of these skills told the agent to put new output
    in `rerun/`, the folder of regenerated copies, which both reproduce
    scripts delete before they start; a test session noticed and refused
    to run. Output of new experiments now goes in `scratch/`, which is
    gitignored and never cleared by a script.
- **A worked example.** `/audit-numbers concurrent/campaign/README.md`
  sent the campaign README to the results-auditor (Opus), which checked 55
  claims against the CSVs in about two minutes and found two errors in
  text written two days before: a table cell read 28.1 where the median
  of the two repetitions is 28.046, so 28.0, and a statement that every CSV row's `log` column
  names its own run's output, which is false for 240 rows of the sanity
  run, whose five builds overwrote each other's log files. Both were
  checked by hand and corrected (commit `65d05e8`).
- **The other two subagents, live.** The `concurrency-reviewer` (Opus)
  read the promotion path of the concurrent skip list and reported, with
  line references, no bugs and two low-severity notes: the code relies on
  memory never being reclaimed (already recorded in
  `concurrent/RESULTS-local.md`), and one statistics counter mixes two
  causes of an abandoned promotion. It could not rerun the container
  tests in that session because Docker was not among the allowed
  commands, and said so. The `cold-reader` (Haiku) read this file and
  listed 15 unclear points, which were fixed.
- **Not exercised live:** the `chameleon-campaign` skill beyond the cost
  estimate (it would spend allocation), and the `concurrent-validation`
  skill as a whole, whose commands were rehearsed in the container on
  6 November.

## Trying it

1. Install Claude Code (see Anthropic's Claude Code documentation; the
   tests used version 2.1.310) and run `claude` in the repository root.
   The first time, it asks whether to trust the folder; accept. Until a folder is
   trusted, Claude Code ignores its allow list (deny rules and hooks still
   apply), so every allowed command would ask.
2. From the repository root, `python3 .claude/hooks/test_hooks.py` checks
   the hooks without the agent (about a minute; it builds the harness once).
3. Inside a session: `/quick-check`, `/audit-numbers <file>`, or a request
   of the kind the skills are written for, such as "add a rule that
   promotes on every second access and compare it with gated-K3"
   (`add-promotion-policy`). A skill can also be called by name, for
   example `/reproduce-structural`.

Requirements are those of the repository, listed in its top-level
`README.md`: a C++20 compiler, CMake 3.20 or later, Python 3 with
matplotlib, and Docker for the concurrent part.
