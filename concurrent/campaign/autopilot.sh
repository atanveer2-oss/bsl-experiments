#!/usr/bin/env bash
# Hands-off driver for the scheduled Chameleon steps of the bsl throughput
# protocol (2026-10-25 to 10-29). Every step is idempotent and logs to
# ~/.config/chameleon/autopilot.log, so it can be run from cron, from a
# Claude session, or by hand, and run again after an interruption.
#
#   autopilot.sh zen3-sanity     22:00 UTC Oct 25: launch the one-node zen3 lease,
#                                provision five (p, K) builds, run the reduced
#                                protocol and the sweep in tmux
#   autopilot.sh zen3-collect    collect the zen3 sanity results into
#                                results/chameleon-zen3-sanity/ and summarise
#   autopilot.sh main-launch     15:00 UTC Oct 26: launch the 4-node lease,
#                                provision (XARGS from ~/.config/chameleon/main-xargs
#                                if that file exists), start the full protocol.
#                                HOLD: if ~/.config/chameleon/main-hold exists the
#                                step logs its first line and exits without
#                                reserving anything. Write the reason into the
#                                file; delete it to release. SANITY-CHECKLIST.md
#                                says when to write it.
#   autopilot.sh collect         collect the main run, write progress-latest.md;
#                                then, only if ~/.config/chameleon/p50-go exists,
#                                run the p50-tail step
#   autopilot.sh p50-tail        on every node whose chain has printed
#                                PROTOCOL-FINISHED and that has no p50 session yet:
#                                build the default p = 1/2, K = 3 binaries into
#                                ~/bsl-build-p50 and run bsl_classic and bsl_gated
#                                at p = 1/2 (5 reps, one shard per node) into
#                                results/chameleon-p50/ on the node, in tmux
#                                session bsl-p50. Uses the lease's slack after the
#                                main chain; never touches a node still running it.
#                                Collects results/chameleon-p50/ from every node
#                                on each call. Switch: ~/.config/chameleon/p50-go
#                                (absent = the collect step does not run this).
#   autopilot.sh final-collect   collect, plot, and leave a marker
#
# Needs: the CLI venv (~/.venvs/chameleon), the RC file the kit reads by
# default, the key pair bsl (~/.ssh/id_ed25519), ssh and rsync.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The venv comes last so python3 is the one with matplotlib; only openstack lives in the venv.
export PATH="/Library/Frameworks/Python.framework/Versions/3.11/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$HOME/.venvs/chameleon/bin"
export CHAMELEON_KEY_NAME=bsl
CFG="$HOME/.config/chameleon"
LOG="$CFG/autopilot.log"
ZH="$CFG/bsl-zen3-sanity-hosts"
MH="$CFG/bsl-hosts"
RESULTS="$HERE/../results"
STRUCTS=bsl_gated,bsl_classic,flexlist_locks,cbtree-concurrent,cbtree-concurrent-fair,skiplist_lazy,skiplist_locks,bronson_pext_bst_occ,bsl_exact,bsl_morris
step="${1:-help}"
mkdir -p "$CFG"
log() { echo "$(date -u +%FT%TZ) [$step] $*" | tee -a "$LOG"; }
run() { log "\$ $*"; "$@" >>"$LOG" 2>&1; local rc=$?; log "rc=$rc"; return $rc; }
cd "$HERE" || exit 1

# One instance of a step at a time (cron and a session may both fire it).
lock="$CFG/autopilot-$step.lock"
if ! mkdir "$lock" 2>/dev/null; then log "another $step is running (lock $lock); exiting"; exit 0; fi
trap 'rmdir "$lock" 2>/dev/null' EXIT

wait_ssh() {  # wait_ssh HOSTS_FILE: until port 22 answers on every node, at most 40 min
  local f="$1" t=0
  while read -r name addr _; do
    [[ -n "${addr:-}" && "$name" != \#* ]] || continue
    until nc -z -w 5 "$addr" 22 2>/dev/null; do
      sleep 20; t=$((t + 20))
      [[ $t -lt 2400 ]] || { log "port 22 on $addr not open after 40 min"; return 1; }
    done
    log "ssh port open on $name ($addr)"
  done < "$f"
}

node_of() { awk '!/^#/ && $2 {print $2; exit}' "$1"; }

case "$step" in
  zen3-sanity)
    log "start"
    run ./reserve.sh up --nodes 1 --hours 16.9 --start "2026-10-25 22:00" --lease bsl-zen3-sanity \
        --hosts-file "$ZH" --key-name bsl --yes || exit 1
    wait_ssh "$ZH" || exit 1
    run ./provision.sh --hosts-file "$ZH" || exit 1
    for b in "k1:-DBSL_K=1" "k2:-DBSL_K=2" "p25k1:-DBSL_P=0.25 -DBSL_K=1" "p25k3:-DBSL_P=0.25 -DBSL_K=3"; do
      run ./provision.sh --hosts-file "$ZH" --xargs "${b#*:}" --work "bsl-build-${b%%:*}" || exit 1
    done
    node="$(node_of "$ZH")"
    common="--distributions uniform,90-10,zipf1 --key-ranges 1000000 --mixes 100-0-0,80-10-10 --threads 1,32,64,128,256 --smt --reps 2 --warmup 5 --duration 10 --update-warmup 5 --update-duration 5"
    chain="cd ~/bsl/experiments/chameleon && export BSL_SETBENCH_WORK=\$HOME/bsl-build && \
python3 run_protocol.py --structures $STRUCTS $common 2>&1 | tee -a ~/bsl-run.log; \
for w in k1 k2 p25k1 p25k3; do BSL_SETBENCH_WORK=\$HOME/bsl-build-\$w python3 run_protocol.py --structures bsl_gated $common 2>&1 | tee -a ~/bsl-run.log; done; \
python3 run_protocol.py --cold-start --structures bsl_gated,bsl_classic,flexlist_locks,cbtree-concurrent --cold-threads 64 --reps 1 2>&1 | tee -a ~/bsl-run.log; \
echo PROTOCOL-FINISHED | tee -a ~/bsl-run.log"
    if ssh -o StrictHostKeyChecking=accept-new "cc@$node" "tmux has-session -t bsl 2>/dev/null"; then
      log "tmux session bsl already running on $node"
    else
      run ssh -o StrictHostKeyChecking=accept-new "cc@$node" "printf '%s sweep: %s\n' \"\$(date -u +%FT%TZ)\" $(printf '%q' "$chain") >> ~/bsl-run-commands.log"
      run ssh -o StrictHostKeyChecking=accept-new "cc@$node" "tmux new-session -d -s bsl $(printf '%q' "$chain")" || exit 1
    fi
    log "zen3 sanity runs started on $node"
    ;;
  zen3-collect)
    log "start"
    run ./collect.sh --hosts-file "$ZH"
    if [[ -d "$RESULTS/chameleon/bsl-zen3-sanity-1" ]]; then
      mkdir -p "$RESULTS/chameleon-zen3-sanity"
      rsync -a "$RESULTS/chameleon/bsl-zen3-sanity-1/" "$RESULTS/chameleon-zen3-sanity/bsl-zen3-sanity-1/" && rm -rf "$RESULTS/chameleon/bsl-zen3-sanity-1"
    fi
    python3 progress.py "$RESULTS/chameleon-zen3-sanity" > "$CFG/zen3-progress-latest.md" 2>&1
    run python3 plot_throughput.py "$RESULTS/chameleon-zen3-sanity"
    log "summary in $CFG/zen3-progress-latest.md"
    ;;
  main-launch)
    log "start"
    if [[ -e "$CFG/main-hold" ]]; then
      log "HOLD in place ($CFG/main-hold): $(head -1 "$CFG/main-hold" 2>/dev/null); not launching"
      exit 0
    fi
    run ./reserve.sh up --nodes 4 --hours 80 --start "2026-10-26 15:00" --lease bsl-throughput \
        --hosts-file "$MH" --key-name bsl --yes || exit 1
    wait_ssh "$MH" || exit 1
    xargs_file="$CFG/main-xargs"
    if [[ -s "$xargs_file" ]]; then
      log "XARGS from $xargs_file: $(cat "$xargs_file")"
      run ./provision.sh --hosts-file "$MH" --xargs "$(cat "$xargs_file")" || exit 1
    else
      run ./provision.sh --hosts-file "$MH" || exit 1
    fi
    run ./start_runs.sh --hosts-file "$MH" -- --smt --update-warmup 10 --update-duration 10 --structures "$STRUCTS" || exit 1
    log "main run started on $(grep -vc '^#' "$MH") nodes"
    ;;
  p50-tail)
    log "start"
    [[ -f "$MH" ]] || { log "no $MH; nothing to do"; exit 0; }
    hosts=(); while read -r name addr _; do [[ -n "${addr:-}" && "$name" != \#* ]] && hosts+=("$addr"); done < "$MH"
    n=${#hosts[@]}
    for i in "${!hosts[@]}"; do
      h="cc@${hosts[$i]}"
      mkdir -p "$RESULTS/chameleon-p50/$(awk -v a="${hosts[$i]}" '$2==a{print $1}' "$MH")"
      rsync -az -e "ssh -o StrictHostKeyChecking=accept-new" "$h:bsl/experiments/results/chameleon-p50/" \
        "$RESULTS/chameleon-p50/$(awk -v a="${hosts[$i]}" '$2==a{print $1}' "$MH")/" 2>/dev/null || true
      if ssh -o StrictHostKeyChecking=accept-new "$h" "tmux has-session -t bsl-p50 2>/dev/null || test -f ~/p50-started"; then
        log "$h: p50 run already started ($(ssh -o StrictHostKeyChecking=accept-new "$h" 'tail -n 1 ~/bsl-p50.log 2>/dev/null' | cut -c1-120))"; continue
      fi
      if ! ssh -o StrictHostKeyChecking=accept-new "$h" "grep -q PROTOCOL-FINISHED ~/bsl-run.log 2>/dev/null"; then
        log "$h: main chain still running; p50 waits"; continue
      fi
      log "$h: main chain finished; building p = 1/2 binaries"
      run ./provision.sh --host "$h" --work bsl-build-p50 || { log "$h: p50 provision failed"; continue; }
      chain="cd ~/bsl/experiments/chameleon && export BSL_SETBENCH_WORK=\$HOME/bsl-build-p50 && \
python3 run_protocol.py --structures bsl_classic,bsl_gated --shard $i/$n --smt --update-warmup 10 --update-duration 10 --reps 5 --results ~/bsl/experiments/results/chameleon-p50 --new 2>&1 | tee -a ~/bsl-p50.log; \
echo P50-FINISHED | tee -a ~/bsl-p50.log"
      run ssh -o StrictHostKeyChecking=accept-new "$h" "printf '%s p50 shard %s/%s: %s\n' \"\$(date -u +%FT%TZ)\" $i $n $(printf '%q' "$chain") >> ~/bsl-run-commands.log; touch ~/p50-started"
      run ssh -o StrictHostKeyChecking=accept-new "$h" "tmux new-session -d -s bsl-p50 $(printf '%q' "$chain")" || { log "$h: could not start the p50 session"; continue; }
      log "$h: p50 run started, shard $i/$n"
    done
    ;;
  collect|final-collect)
    log "start"
    [[ -f "$MH" ]] || { log "no $MH yet; nothing to collect"; exit 0; }
    run ./collect.sh --hosts-file "$MH"
    python3 progress.py "$RESULTS/chameleon" > "$CFG/progress-latest.md" 2>&1
    log "$(head -1 "$CFG/progress-latest.md")"
    if [[ "$step" == final-collect ]]; then
      run python3 plot_throughput.py "$RESULTS/chameleon"
      date -u +%FT%TZ > "$CFG/final-collect-done"
    fi
    if [[ -f "$CFG/p50-go" ]]; then log "p50-go is set; running p50-tail"; "$HERE/autopilot.sh" p50-tail; fi
    ;;
  *)
    sed -n '2,20p' "$0"; exit 2 ;;
esac
log "done"
