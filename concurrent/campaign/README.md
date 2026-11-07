# The Chameleon campaign

The laptop-side scripts that reserved, set up, started and collected the
throughput runs on Chameleon Cloud (CHI@TACC, 25 to 29 October 2026), and
what they recorded. The node-side half (the build, the runner, the plots)
is in `../protocol/`; the protocol itself is `../protocol/PROTOCOL.md`.

You do not need these scripts to check the paper. The rows are in
`../results/`, `../results-p50/` and `../results-sanity/`, and the figures
and table are redrawn from them by the commands in the top-level README.
The scripts are here so that the campaign can be read, and repeated on
Chameleon or adapted to another cloud.

## What is here

| File | Runs on | What it does |
|---|---|---|
| `reserve.sh` | laptop | creates the lease, waits for it, launches the instances, attaches floating IPs, prints the SSH commands; also `status`, `extend`, `delete` |
| `provision.sh` | laptop | per node: installs packages, copies the experiment tree, runs `node_build.sh`, sets the CPU governor, prints the CPU layout and the runner's row count |
| `start_runs.sh` | laptop | starts `run_protocol.py` in `tmux` on every node, one shard of the protocol each |
| `collect.sh` | laptop | copies rows, run logs and per-run driver output back from every node; safe to run at any time |
| `autopilot.sh` | laptop | the scheduled steps of this campaign (sanity run, main launch, periodic collection, the p = 1/2 rerun, final collection), run from cron; a record, not meant to be rerun as it is: its dates, lease names and paths are this campaign's |
| `SANITY-CHECKLIST.md` | (a document) | the go/no-go gates checked between the sanity run and the main launch |
| `make-original-layout.sh` | laptop | rebuilds the directory layout these scripts ran from (see below) |

Two more tools sit with the runner in `../protocol/`, because they import
it: `estimate.py` (node-hours and wall-clock for a parameter set, before
reserving) and `progress.py` (rows done, failures and preliminary medians
from collected results).

## How the campaign ran

Chameleon terms used below: a *lease* reserves bare-metal nodes for a
time window and is charged in node-hours against the project's
*allocation*; `compute_zen3` is the node type (two AMD EPYC 7763, 256
hardware threads); a *floating IP* is the public address of a node.

1. **Sanity run, 25 October, one node.** The reduced protocol on one
   `compute_zen3` node, with five builds of our structure, (p, K) = (1/4, 1),
   (1/4, 3), (1/2, 1), (1/2, 2) and (1/2, 3). It checked row counts,
   validation, memory, plausibility, scaling and run rate against the gates
   in `SANITY-CHECKLIST.md`; every gate passed. Its rows are in
   `../results-sanity/`. It also chose the setting for the main sweep:

   | (p, K) | Zipf 1 path | Zipf 1 Mops/s | 90/10 path | 90/10 Mops/s | uniform Mops/s |
   |---|---|---|---|---|---|
   | 1/4, 1 | 22.2 | 106.3 | 31.0 | 98.0 | 49.7 |
   | 1/4, 3 | 24.7 | 99.3 | 31.9 | 94.5 | 50.3 |
   | 1/2, 1 | 25.3 | 108.8 | 32.2 | 99.8 | 49.3 |
   | 1/2, 2 | 28.1 | 107.0 | 32.5 | 98.9 | 48.6 |
   | 1/2, 3 | 25.8 | 104.7 | 32.3 | 97.2 | 48.1 |

   Our rule, read-only, 64 threads, 10^6 keys, median of 2 repetitions.
   (1/4, 1) has the shortest Zipf 1 search path and throughput within
   3 percent of the best, so the main sweep was built with
   `-DBSL_P=0.25 -DBSL_K=1`.
2. **Main sweep, 26 to 28 October, four nodes, one shard each.** Ten
   structures, five distributions, two key ranges, two mixes, thread counts
   1 to 256, ten repetitions: `../results/node-*/throughput_tacc_2026-10-26.csv`.
3. **Cold start, 28 to 29 October**, on the same nodes after the sweep:
   `../results/node-*/coldstart_tacc_2026-10-28.csv`.
4. **Rerun at p = 1/2, K = 3, 29 October**, our rule and the classic skip
   list only, five repetitions, in the lease time left after the main chain:
   `../results-p50/`.

Each node's `bsl-run-commands.log` holds the exact command started on it,
`bsl-run.log` the runner's console output, and `BUILD_INFO` the build
record (compiler, flags, patch state, a digest of the fork's sources).
`../MACHINE.md` describes the nodes.

## The per-run driver output

Every row of every CSV has a `log` column naming the file that holds the
benchmark driver's full output for that run, relative to the node's
directory. The 29,680 files are packed in `../driver-logs.tar.gz` (3 MB).
Unpack it from `concurrent/`:

```
tar -xzf driver-logs.tar.gz
```

and each node directory gains a `logs/` folder, so a row's `log` value
resolves from its own directory. The unpacked folders are gitignored.

## Running the scripts

The scripts ran from a working tree in which `setbench/` (this repository's
`concurrent/`) and `chameleon/` (the runner and these scripts) sat side by
side under one `experiments/` directory, and they find each other by those
relative paths. Rather than edit a record of what ran, this directory keeps
them as they were and provides the layout:

```
concurrent/campaign/make-original-layout.sh ~/bsl-campaign
git clone https://github.com/Aksenov239/splaylist ~/bsl-campaign/experiments/third_party/splaylist
git -C ~/bsl-campaign/experiments/third_party/splaylist checkout f6b6757
cd ~/bsl-campaign/experiments/chameleon
```

Run the first line from the repository root; the script creates
`~/bsl-campaign/experiments/` and copies into its `chameleon/` folder both
these scripts and the ones in `../protocol/` (`estimate.py`, the runner,
the node build). `node_build.sh` and `run_protocol.py` work in both
layouts. Everything else in this directory is unchanged from the campaign.

**Once, before the first lease:**

1. Membership of a Chameleon project with an allocation, at the CHI@TACC
   site.
2. The OpenStack CLI with the reservation plugin, in its own environment:
   `python3 -m venv ~/.venvs/chameleon && ~/.venvs/chameleon/bin/pip install
   python-openstackclient chameleon-blazarclient`.
3. An application credential (Identity, Application Credentials in the
   CHI@TACC dashboard; tick "unrestricted", which Blazar needs from the
   CLI). Save the RC file it offers as
   `~/.config/chameleon/CHI@TACC-openrc.sh`, mode 600, outside any
   repository: it contains the secret. `reserve.sh` reads it from there,
   from `$CHAMELEON_RC`, or from `--rc`.
4. An SSH key pair registered at the site: `openstack keypair create
   --public-key ~/.ssh/id_ed25519.pub bsl`, then `export
   CHAMELEON_KEY_NAME=bsl`.

**A campaign:**

```
S=bsl_gated,bsl_classic,flexlist_locks,cbtree-concurrent,cbtree-concurrent-fair,skiplist_lazy,skiplist_locks,bronson_pext_bst_occ,bsl_exact,bsl_morris
python3 estimate.py --nodes 4 --smt --update-warmup 10 --update-duration 10 --structures $S
./reserve.sh up --nodes 4 --hours 80              # asks before spending allocation
./provision.sh --xargs "-DBSL_P=0.25 -DBSL_K=1"   # p and K are compile-time
./start_runs.sh -- --smt --update-warmup 10 --update-duration 10 --structures $S
./collect.sh                                      # any time, as often as you like
./reserve.sh delete                               # stop the charge when done
```

These are the main sweep's settings; the exact command started on each
node is in `../results/node-*/bsl-run-commands.log`. `reserve.sh` uses
`compute_zen3` unless given `--node-type`, and writes the nodes' addresses
to `~/.config/chameleon/bsl-hosts`, which the other scripts read. A lease is charged for its full length times its
nodes, busy or not, and bare-metal leases are limited to seven days
(`reserve.sh extend` prolongs one). The runner resumes after any
interruption: started again, it skips every row already in its CSV.

## About the `commit` column

The `commit` field in `BUILD_INFO` and in every row is the commit of the
private working repository the campaign ran from, which readers cannot
see: `efb6935` for the main sweep and cold start, `b3d3055` for the
p = 1/2 rerun (with uncommitted changes, which the suffix `-dirty` marks)
and `45516ed` for the sanity run. It is not a commit of this repository.
The files that were built are in this repository: `../ds/` is byte for
byte what was built at all three commits, and so are `../build.sh`,
`../portability.patch` and `../harness.patch` apart from the harness
patch's header comment. The fork
is pinned by its own commit, `f6b6757`, and by `fork_digest` in
`BUILD_INFO`.
