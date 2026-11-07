#!/usr/bin/env bash
# Copy results and logs back from every node into
# experiments/results/chameleon/<node hostname>/ on the laptop. Safe to run
# while the protocol is still going (it only adds and updates files).
#
#   experiments/chameleon/collect.sh [--host cc@IP ...]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$(cd "$HERE/.." && pwd)/results/chameleon"

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
    -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
    *) echo "collect.sh: unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
if [[ ${#hosts[@]} -eq 0 ]]; then
  [[ -f "$hosts_file" ]] || { echo "collect.sh: no --host and no $hosts_file" >&2; exit 2; }
  while read -r name addr _; do
    [[ -n "${addr:-}" && "$name" != \#* ]] && hosts+=("cc@$addr")
  done < "$hosts_file"
fi

for h in "${hosts[@]}"; do
  label="$(ssh "${ssh_opts[@]}" "$h" hostname -s)"
  mkdir -p "$DEST/$label"
  rsync -az -e "ssh ${ssh_opts[*]}" "$h:$remote_dir/experiments/results/chameleon/" "$DEST/$label/"
  rsync -az -e "ssh ${ssh_opts[*]}" "$h:$work/BUILD_INFO" "$h:bsl-run.log" "$h:bsl-run-commands.log" "$DEST/$label/" 2>/dev/null || true
  # Kernel log snapshot (OOM kills, hardware errors, thermal events) and the memory
  # state at collection time, so a dead or slow node can be explained from the record.
  mkdir -p "$DEST/$label/node"
  stamp="$(date -u +%Y%m%dT%H%M%SZ)"
  ssh "${ssh_opts[@]}" "$h" 'sudo dmesg -T 2>/dev/null || dmesg -T 2>/dev/null || journalctl -k --no-pager 2>/dev/null' \
    > "$DEST/$label/node/dmesg-$stamp.log" 2>/dev/null || true
  ssh "${ssh_opts[@]}" "$h" 'date -u +%FT%TZ; uptime; free -g; df -h ~ 2>/dev/null; tmux has-session -t bsl 2>/dev/null && echo "tmux bsl: running" || echo "tmux bsl: not running"; tail -n 3 ~/bsl-run.log 2>/dev/null' \
    > "$DEST/$label/node/state-$stamp.txt" 2>/dev/null || true
  grep -il 'out of memory\|killed process\|hardware error\|mce:' "$DEST/$label/node/dmesg-$stamp.log" >/dev/null 2>&1 \
    && echo "$h ($label): KERNEL LOG has out-of-memory or hardware lines, see $DEST/$label/node/dmesg-$stamp.log"
  rows=$(cat "$DEST/$label"/throughput_*.csv 2>/dev/null | grep -vc '^site,' || true)
  echo "$h ($label): $rows throughput rows -> $DEST/$label/"
done
echo "plot with: python3 experiments/chameleon/plot_throughput.py $DEST"
