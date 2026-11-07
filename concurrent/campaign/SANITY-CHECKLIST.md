# Sanity checklist: gates before and during the main Chameleon run

Written 2026-10-25 for the campaign of 2026-10-25 to 10-29. Whoever runs the
decision step (a Claude session at 09:38 CDT on Oct 26, or the user by hand)
walks this list top to bottom. Every gate is a yes or no with a number
behind it, so a reader can see why a launch went ahead or was held.

The hold switch: `autopilot.sh main-launch` exits without reserving anything
when `~/.config/chameleon/main-hold` exists. Write the failed gate and the
number into the file as its first line. Delete the file to release. The
crontab fires main-launch once (10:04 CDT Oct 26); after a hold, launch by
hand with a fresh start time:

    ./autopilot.sh main-launch            # after editing the --start in the script, or
    ./reserve.sh up --nodes 4 --hours 80 --start "<UTC>" --lease bsl-throughput \
        --hosts-file ~/.config/chameleon/bsl-hosts --key-name bsl --yes

## A. Inputs to the decision

- `~/.config/chameleon/zen3-progress-latest.md`, written by `autopilot.sh zen3-collect`
  at 09:33 CDT. Its "planned rows" figure is for the full protocol, not for
  the sweep, so ignore it and count rows directly (gate 2).
- Plots under `experiments/results/chameleon-zen3-sanity/` (same step).
- The node log: `ssh cc@<node> tail -50 ~/bsl-run.log` (node address in
  `~/.config/chameleon/bsl-zen3-sanity-hosts`). The chain ends with the line
  `PROTOCOL-FINISHED`.
- The Cascade Lake reference run, for plausibility:
  `experiments/results/chameleon-sanity-cascadelake-2026-10-24/` (10^5 keys,
  8 s runs, 96 threads, Zipf 1, read-only). Mops/s: gated 212, Morris 232,
  exact 38, classic 164, skiplist_locks 158, splay-list 99, CBTree 197.
  80/10/10 Zipf 1: gated 145, Morris 149, classic 125, skiplist_locks 116,
  splay-list 98.

## B. Gates (all must pass to launch without a hold)

1. **The sweep finished.** `PROTOCOL-FINISHED` is in the node log. If not,
   see how far it got; the ten-structure block must be complete for gates
   2 to 7, the four variant blocks for gate 8. A half-done sweep with
   everything else passing is a reason to delay the launch by hours, not
   to hold indefinitely: write the hold, re-collect when it finishes,
   release.

2. **Row counts.** Expected throughput rows from the sweep:

   | block | rows |
   |---|---|
   | ten structures, read-only | 300 |
   | eight structures, 80/10/10 | 240 |
   | four (p, K) variant builds, gated only | 4 x 60 = 240 |
   | cold start, 4 structures, 5 distributions, 64 threads, 7 windows | 140 |

   Fewer rows in a finished sweep means runs failed. The runner logs each
   failure in the per-run log under the results directory; read the exit
   line of the failed ones.

3. **Validation.** `bsl_rows_without_validate_ok` in the summary is 0. Any
   other value is a hold: a structural failure in our code at scale, and no
   number from that structure is trustworthy until the cause is found in
   `~/bsl-run.log` (the exit-3 run prints which check failed).

4. **Memory.** `peak_rss_gb_max` is under 200 (80 percent of 256 GB) and no
   cell was skipped by the guard (the run log says `skipping` when it
   trips). The 80/10/10 mix at 256 threads is the worst case. If it trips,
   the fix for the main run is a lower cap, `-DBSL_MAX_HEIGHT=48`, written
   into `~/.config/chameleon/main-xargs` with the (p, K) choice, or shorter
   update runs; not a hold on its own if the read-only mix is unaffected.

5. **Plausibility against Cascade Lake.** Read-only Zipf 1 at 64 threads:
   every structure within a factor of 3 of its Cascade Lake number at 96
   threads (the key range differs, 10^6 against 10^5, so the Zen3 numbers
   will be lower; a factor of 3 catches a broken build, not a slower
   machine). No structure at or near zero. Hold if any structure is off by
   more than that; it means a build or a run flag is wrong on this node.

6. **Scaling shape.** For every structure, throughput at 32 threads is
   above throughput at 1 thread, and at 64 above 32. Flattening at 128 and
   256 (SMT) is expected and not a failure. A structure that gets slower
   with more threads below 64 is a hold for that structure (check the
   `numactl` line in the run log: the runner interleaves memory across the
   two sockets; if numactl was missing the numbers are not comparable).

7. **The theory's direction.** On Zipf 1 and 90-10, read-only: gated
   throughput at or above classic, gated path length below classic. On
   uniform: gated within 25 percent of classic (the rule's cost on a
   workload with nothing to learn; Cascade Lake measured 12 to 18 percent).
   A reversal here is not a hold in itself, since it is a result, but it
   must be reported to the user before the launch, because it changes what
   the main run is for.

8. **The (p, K) choice.** From the four variant blocks and the default
   build, tabulate for the gated rule on Zipf 1 and 90-10 read-only at 64
   threads: median path length, median throughput, and the uniform cost
   against classic, per (p, K). With 2 repetitions, differences under 5
   percent are noise. Rule for the choice:
   - lowest Zipf 1 path length, unless its throughput is more than 5 percent
     below the best, or its uniform cost is more than 5 points worse;
   - ties and noise-level differences keep the default (p = 1/2, K = 3),
     because the paper's figures are drawn for it;
   - the choice is written to `~/.config/chameleon/main-xargs` as compiler
     flags (`-DBSL_K=2`, `-DBSL_P=0.25 -DBSL_K=1`), or the file is left
     absent for the default. The choice and the table go into the dashboard
     log and into `analysis/handoff-experiments.md`.

9. **Cold start.** The 64-thread curves show throughput rising over the
   first windows for gated and flat for classic, or the adaptation is too
   fast to see at this thread count (the Cascade Lake run showed adaptation
   under 1 s at 48 threads). Either is fine; the main run has the 1-thread
   setting for this. Zero or absent cold-start rows: see gate 2.

10. **Rate against the lease.** The estimate for the full protocol on four
    nodes is 67.2 h against an 80 h lease (`estimate.py --nodes 4 --smt
    --update-warmup 10 --update-duration 10`), a 16 percent margin. Check
    the sweep's actual seconds per run against the estimate's assumption
    (warmup + duration + 5 s overhead): divide the sweep's wall-clock for
    the ten-structure block by 540. If the real overhead is more than 10 s
    a run, the main run will not fit; cut `--reps` to 8 in
    `autopilot.sh main-launch` before 10:04, or plan an extension
    (`reserve.sh extend --hours 24`) and say so in the dashboard log.

## C. What the launch check at 10:43 CDT confirms

- `autopilot.log` shows main-launch reached "main run started on 4 nodes".
- `~/.config/chameleon/bsl-hosts` lists four nodes with addresses.
- On each node, `tmux has-session -t bsl` succeeds and `~/bsl-run.log` is
  growing. The first rows appear within about a minute of the start.
- The build on the nodes used the intended flags: the first bsl row's
  `ds_stats` line reports `p=` and `K=` and `cap=` (the binary prints its compiled values, and the runner refuses a row whose binary disagrees with the p and K it would record).

## D. Mid-run checks (every 6 hours, from progress-latest.md)

| Check | Pass | Correction |
|---|---|---|
| Rows done grow since last check on every node | yes | A node with a stale log and no `PROTOCOL-FINISHED`: rerun `start_runs.sh` (it resumes from the CSV); if the node is unreachable, start its shard on another node with `--shard i/4` after that node's own shard finishes |
| `hours_left_at_rate` fits before 23:00 UTC Oct 29 | yes | `reserve.sh extend --hours N` (asks the user), or cut the remaining repetitions |
| "Runner logs" line: 0 FAILED runs | yes | A failed run is not in the CSV; its per-run log under `logs/tacc/<mix>/<structure>/` has the command, the full output and the exit code. Read it; a rerun of the same command retries the row |
| `bsl_rows_without_validate_ok` = 0 | yes | Stop trusting that structure; read the exit-3 run log; do not restart it until understood |
| No new guard skips | yes | Note the gap; a shorter-run or lower-cap rerun fills it after the campaign |
| Peak RSS stays under 200 GB | yes | As above |
| Preliminary table moves in the theory's direction (gate 7) | report either way | Nothing to correct; it is the result |

Any correction that changes protocol flags is recorded in the dashboard log
with the time and the shard, so the final CSVs can be read with it.

## E. After the final collect (Oct 29 17:38 CDT)

- Row count against the plan (19 800 throughput rows and 7 000 cold-start
  rows for the default protocol; fewer if repetitions were cut, and the
  cut is recorded).
- Gate 3 again on the whole set.
- Delete the lease once the last collect is in (`reserve.sh delete`), so the
  allocation stops being charged.
- Commit the CSVs, the logs, the plots and the plotting script together.
