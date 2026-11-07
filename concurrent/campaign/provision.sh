#!/usr/bin/env bash
# Provision Chameleon nodes from the laptop: packages, code, native build,
# checks. Run after reserve.sh has printed the SSH commands.
#
#   experiments/chameleon/provision.sh                    # every host in the hosts file
#   experiments/chameleon/provision.sh --host cc@192.0.2.10 [--host ...]
#
# Options:
#   --host USER@IP       a node (repeatable); default: every line of --hosts-file
#   --hosts-file PATH    written by reserve.sh (default ~/.config/chameleon/bsl-hosts)
#   --remote-dir DIR     where experiments/ goes on the node (default bsl, under ~)
#   --work DIR           build directory on the node (default bsl-build, under ~)
#   --papi               also install libpapi-dev (the drivers do not use PAPI)
#   --no-harness-patch   build the fork exactly as vendored (threads <= 99, fixed seed)
#   --keep-governor      leave the CPU frequency governor as the image set it
#   --ssh-key PATH       private key for ssh and rsync
#   --xargs "FLAGS"      extra compiler flags for every binary, e.g. "-DBSL_K=2"
#                        (our p and K are compile-time; see setbench/build.sh)
#
# Per node: installs build-essential g++ make numactl libnuma-dev python3
# rsync tmux; rsyncs experiments/ (without build/, results/*.csv,
# results/chameleon/, setbench/.work); runs chameleon/node_build.sh there,
# which builds with setbench/build.sh, applies harness.patch, checks the
# binary count and runs a 2-second smoke test; sets the performance governor
# if the node exposes cpufreq; prints the runner's row count.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXPERIMENTS="$(cd "$HERE/.." && pwd)"

hosts=()
hosts_file="${CHAMELEON_HOSTS_FILE:-$HOME/.config/chameleon/bsl-hosts}"
remote_dir="bsl"
work="bsl-build"
papi=0 harness=1 governor=1 xargs=""
ssh_opts=(-o StrictHostKeyChecking=accept-new -o ServerAliveInterval=30)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) hosts+=("$2"); shift ;;
    --hosts-file) hosts_file="$2"; shift ;;
    --remote-dir) remote_dir="$2"; shift ;;
    --work) work="$2"; shift ;;
    --papi) papi=1 ;;
    --no-harness-patch) harness=0 ;;
    --keep-governor) governor=0 ;;
    --ssh-key) ssh_opts+=(-i "$2"); shift ;;
    --xargs) xargs="$2"; shift ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
    *) echo "provision.sh: unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

if [[ ${#hosts[@]} -eq 0 ]]; then
  [[ -f "$hosts_file" ]] || { echo "provision.sh: no --host and no $hosts_file (run reserve.sh first)" >&2; exit 2; }
  while read -r name addr _; do
    [[ -n "${addr:-}" && "$name" != \#* ]] && hosts+=("cc@$addr")
  done < "$hosts_file"
fi
[[ ${#hosts[@]} -gt 0 ]] || { echo "provision.sh: no hosts" >&2; exit 2; }

# Provenance: the commit of experiments/ as rsynced. third_party/ is
# gitignored, so node_build.sh also records a digest of the fork it built.
commit="$(git -C "$EXPERIMENTS" rev-parse HEAD 2>/dev/null || echo unknown)"
if [[ -n "$(git -C "$EXPERIMENTS" status --porcelain -- . 2>/dev/null)" ]]; then
  commit="$commit-dirty"
  echo "provision.sh: experiments/ has uncommitted changes; rows will record $commit"
fi

pkgs="build-essential g++ make numactl libnuma-dev python3 rsync tmux"
[[ $papi -eq 1 ]] && pkgs="$pkgs libpapi-dev"
build_flags="--commit $commit"
[[ $harness -eq 0 ]] && build_flags="$build_flags --no-harness-patch"

for h in "${hosts[@]}"; do
  echo "=================== $h"
  echo "== packages"
  # A node that has just booted may not reach the mirrors yet, and apt-get
  # update exits 0 even when some index fetches fail (2026-10-25: the zen3
  # rehearsal lost its provision to "Unable to locate package numactl").
  # So: retry the update plus install up to 6 times, 30 s apart, and fail
  # the provision if the packages are still missing.
  ssh "${ssh_opts[@]}" "$h" "for try in 1 2 3 4 5 6; do
      sudo env DEBIAN_FRONTEND=noninteractive apt-get update -q >/dev/null 2>&1
      if sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends $pkgs >/dev/null 2>&1; then
        echo installed: $pkgs; break
      fi
      echo \"apt try \$try failed; retrying in 30 s\"; sleep 30
      [[ \$try -eq 6 ]] && { echo 'packages still missing'; exit 100; }
    done" || exit 100

  echo "== rsync experiments/ -> $h:~/$remote_dir/experiments/"
  # rsync creates only the last path component, so make the parent first.
  ssh "${ssh_opts[@]}" "$h" "mkdir -p ~/$remote_dir"
  rsync -az -e "ssh ${ssh_opts[*]}" \
    --exclude '/build/' --exclude '/results/*.csv' --exclude '/results/chameleon/' \
    --exclude '/setbench/.work/' --exclude '__pycache__/' --exclude '.DS_Store' \
    --exclude '/third_party/splaylist/structs_bench/cmake-build-debug/' \
    "$EXPERIMENTS/" "$h:$remote_dir/experiments/"

  echo "== build and check"
  ssh "${ssh_opts[@]}" "$h" "BSL_SETBENCH_WORK=\$HOME/$work XARGS=$(printf '%q' "$xargs") bash $remote_dir/experiments/chameleon/node_build.sh $build_flags"

  if [[ $governor -eq 1 ]]; then
    echo "== CPU frequency governor"
    ssh "${ssh_opts[@]}" "$h" 'f=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
      if [ -e "$f" ]; then
        for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance | sudo tee "$g" >/dev/null; done
        echo "governor: $(cat $f)"
      else
        echo "governor: no cpufreq interface exposed (firmware-controlled); left as is"
      fi'
  fi

  echo "== SMT and pinning layout"
  ssh "${ssh_opts[@]}" "$h" 'lscpu | grep -E "^(Model name|Socket|Core|Thread|CPU\(s\)|NUMA node[0-9]* CPU)"'

  echo "== protocol size on this node"
  ssh "${ssh_opts[@]}" "$h" "cd $remote_dir/experiments/chameleon && BSL_SETBENCH_WORK=\$HOME/$work python3 run_protocol.py --dry-run 2>&1 >/dev/null | tail -2"
done

echo
echo "provisioned ${#hosts[@]} node(s). Start the runs with:"
echo "  experiments/chameleon/start_runs.sh"
