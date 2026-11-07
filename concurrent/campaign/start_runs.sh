#!/usr/bin/env bash
# Start the protocol on every provisioned node, inside tmux, one shard each.
#
#   experiments/chameleon/start_runs.sh [--host cc@IP ...] [-- RUNNER_ARGS...]
#
# Node i of N runs `run_protocol.py --shard i/N` (the throughput sweep), then
# `run_protocol.py --cold-start --shard i/N`, in a tmux session named bsl,
# logging to ~/bsl-run.log. RUNNER_ARGS go to both, e.g.
#   -- --struct-param bsl_gated:p=0.5,K=1 --skip-missing
# If the session already exists on a node it is left alone. Rerunning after
# a reboot or a killed session resumes: the runner skips rows already in its
# CSV. Watch a node with: ssh -t cc@IP tmux attach -t bsl   (detach: C-b d)
set -euo pipefail

hosts=()
hosts_file="${CHAMELEON_HOSTS_FILE:-$HOME/.config/chameleon/bsl-hosts}"
remote_dir="bsl" work="bsl-build"
ssh_opts=(-o StrictHostKeyChecking=accept-new -o ServerAliveInterval=30)
while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) hosts+=("$2"); shift ;;
    --hosts-file) hosts_file="$2"; shift ;;
    --remote-dir) remote_dir="$2"; shift ;;
    --work) work="$2"; shift ;;
    --ssh-key) ssh_opts+=(-i "$2"); shift ;;
    --) shift; break ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "start_runs.sh: unknown option $1 (runner arguments go after --)" >&2; exit 2 ;;
  esac
  shift
done
runner_args=""
[[ $# -gt 0 ]] && runner_args="$(printf '%q ' "$@")"

if [[ ${#hosts[@]} -eq 0 ]]; then
  [[ -f "$hosts_file" ]] || { echo "start_runs.sh: no --host and no $hosts_file" >&2; exit 2; }
  while read -r name addr _; do
    [[ -n "${addr:-}" && "$name" != \#* ]] && hosts+=("cc@$addr")
  done < "$hosts_file"
fi
n=${#hosts[@]}
[[ $n -gt 0 ]] || { echo "start_runs.sh: no hosts" >&2; exit 2; }

for i in "${!hosts[@]}"; do
  h="${hosts[$i]}"
  run="cd ~/$remote_dir/experiments/chameleon && export BSL_SETBENCH_WORK=\$HOME/$work && \
python3 run_protocol.py --shard $i/$n $runner_args 2>&1 | tee -a ~/bsl-run.log; \
python3 run_protocol.py --cold-start --shard $i/$n $runner_args 2>&1 | tee -a ~/bsl-run.log; \
echo PROTOCOL-FINISHED | tee -a ~/bsl-run.log"
  if ssh "${ssh_opts[@]}" "$h" "tmux has-session -t bsl 2>/dev/null"; then
    echo "$h: tmux session bsl already running, left alone"
    continue
  fi
  # Record the exact command the node was started with (appended, timestamped), so a
  # change of flags mid-campaign is in the node's own record; collect.sh fetches it.
  ssh "${ssh_opts[@]}" "$h" "printf '%s shard %s/%s: %s\n' \"\$(date -u +%FT%TZ)\" $i $n $(printf '%q' "$run") >> ~/bsl-run-commands.log"
  ssh "${ssh_opts[@]}" "$h" "tmux new-session -d -s bsl $(printf '%q' "$run")"
  echo "$h: started shard $i/$n (ssh -t $h tmux attach -t bsl)"
done
echo "progress: ssh cc@IP tail -n 3 ~/bsl-run.log; results: experiments/chameleon/collect.sh"
