---
name: chameleon-campaign
description: Plan and run the throughput protocol on Chameleon Cloud bare-metal nodes (estimate cost, reserve a lease, provision, start, monitor, collect, release). Use when the user wants throughput or scaling numbers on real many-core hardware. Creating or extending a lease spends allocation and always needs the user's explicit go.
---

# Run the throughput protocol on Chameleon

The scripts are in `concurrent/campaign/` (laptop side) and
`concurrent/protocol/` (node side); `concurrent/campaign/README.md` has
the details and the record of the October 2026 campaign. The scripts run
from the layout they were written in, which
`concurrent/campaign/make-original-layout.sh ~/bsl-campaign` recreates;
work from `~/bsl-campaign/experiments/chameleon/`.

## Hard rules

- **Never create, extend or reuse a lease without the user's explicit
  "go" in this session**, after showing them the estimate. `reserve.sh`
  asks before spending; do not pass `--yes` unless the user said go.
- **Delete the lease when the runs finish** (`./reserve.sh delete`). A
  lease is charged for its whole window, busy or idle.
- Credentials stay in `~/.config/chameleon/`. Never print, copy or commit
  the RC file; never read it into the conversation.
- Remove any cron entries you install when the campaign ends.

## Steps

1. **Validate first.** Run the `concurrent-validation` skill; do not
   measure code that has not passed it.
2. **Estimate.** `python3 estimate.py --nodes N [protocol flags]` prints
   node-hours and wall-clock. Show the user the estimate and the dates the
   node type is free; wait for their go.
3. **Rehearse cheaply.** Before a long lease, a short lease on one node
   (any type) running a reduced protocol (`--reps 2`, few thread counts)
   exercises reserve, provision, run, collect and plot end to end. In
   October 2026 this found a provisioning bug for about 4 SUs.
4. **Gate the main run.** Check the rehearsal against
   `SANITY-CHECKLIST.md` (row counts, validation, memory, plausibility,
   scaling, run rate). Report each gate with its number; any failed gate
   means no launch until the user decides.
5. **Launch.** `./reserve.sh up --nodes N --hours H` (it asks), then
   `./provision.sh --xargs "-DBSL_P=... -DBSL_K=..."`, then
   `./start_runs.sh -- <runner flags>`. Each node runs one shard in `tmux`.
6. **Monitor and collect.** `./collect.sh` copies rows and logs back and is
   safe to run any time; `python3 progress.py ../results/chameleon` shows
   progress and failures. The runner resumes after any interruption.
7. **Finish.** Final `./collect.sh`, plots, then `./reserve.sh delete`, and
   remove scheduled jobs. Commit the rows with the node records
   (`BUILD_INFO`, run logs) and update `concurrent/MACHINE.md`.

Report at each step what was done, what it cost so far, and what waits on
the user.
