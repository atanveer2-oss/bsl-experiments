#!/usr/bin/env bash
# Reserve and launch Chameleon bare-metal nodes for the throughput protocol.
# Runs on the laptop with the OpenStack CLI and the Chameleon Blazar plugin:
#   pip install python-openstackclient chameleon-blazarclient
#
#   reserve.sh up --nodes 3 --hours 70 --key-name KEY     lease, wait, launch, IPs
#   reserve.sh lease --nodes 3 --hours 70                  create the lease only
#   reserve.sh wait                                        wait for ACTIVE
#   reserve.sh launch --key-name KEY                       instances and floating IPs
#   reserve.sh status                                      lease, instances, IPs
#   reserve.sh extend --hours 24                           prolong the lease
#   reserve.sh delete                                      end the lease early
#
# Options:
#   --rc PATH         OpenStack RC file (default $CHAMELEON_RC, else
#                     ~/.config/chameleon/CHI@TACC-openrc.sh). Never put it in the repo.
#   --nodes N         nodes to reserve (default 1)
#   --hours H         lease length, or the extension for `extend`
#   --start TIME      "YYYY-MM-DD HH:MM" in UTC (default: now)
#   --node-type T     default compute_zen3 (CHI@TACC, 2 x AMD EPYC 7763)
#   --image NAME      default CC-Ubuntu22.04
#   --key-name NAME   an OpenStack key pair at the site (default $CHAMELEON_KEY_NAME)
#   --lease NAME      default bsl-throughput
#   --network NAME    default sharednet1
#   --hosts-file P    where node names and IPs are written (default
#                     ~/.config/chameleon/bsl-hosts), read by provision.sh,
#                     start_runs.sh and collect.sh
#   --yes             do not ask before calls that spend allocation
#
# Calls that spend allocation (creating or extending a lease) ask first,
# unless --yes. Everything is idempotent where the API allows: an existing
# lease, instance or attached floating IP is reused, not duplicated.
set -euo pipefail

cmd="${1:-help}"; [[ $# -gt 0 ]] && shift
rc="${CHAMELEON_RC:-$HOME/.config/chameleon/CHI@TACC-openrc.sh}"
nodes=1 hours="" start="" node_type="compute_zen3" image="CC-Ubuntu22.04"
key_name="${CHAMELEON_KEY_NAME:-}" lease="bsl-throughput" network="sharednet1"
hosts_file="${CHAMELEON_HOSTS_FILE:-$HOME/.config/chameleon/bsl-hosts}"
yes=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rc) rc="$2"; shift ;;
    --nodes) nodes="$2"; shift ;;
    --hours) hours="$2"; shift ;;
    --start) start="$2"; shift ;;
    --node-type) node_type="$2"; shift ;;
    --image) image="$2"; shift ;;
    --key-name) key_name="$2"; shift ;;
    --lease) lease="$2"; shift ;;
    --network) network="$2"; shift ;;
    --hosts-file) hosts_file="$2"; shift ;;
    --yes|-y) yes=1 ;;
    *) echo "reserve.sh: unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

usage() { sed -n '2,33p' "$0"; }
die() { echo "reserve.sh: $*" >&2; exit 1; }

confirm() {
  [[ $yes -eq 1 ]] && return 0
  read -r -p "$1 [y/N] " ans
  [[ "$ans" == y || "$ans" == Y || "$ans" == yes ]] || die "cancelled"
}

setup() {
  [[ -f "$rc" ]] || die "no RC file at $rc (see chameleon/README.md, step 3)"
  # shellcheck disable=SC1090
  set +u; source "$rc"; set -u
  command -v openstack >/dev/null || die "openstack CLI not found: pip install python-openstackclient chameleon-blazarclient"
  openstack reservation lease list -f value -c id >/dev/null 2>&1 \
    || die "the Blazar plugin is missing or the credential cannot use it (application credentials must be 'unrestricted')"
}

lease_field() { openstack reservation lease show "$lease" -f value -c "$1" 2>/dev/null || true; }

utc_plus() {  # utc_plus "START|now" HOURS -> "YYYY-MM-DD HH:MM"
  python3 - "$1" "$2" <<'EOF'
import sys, datetime as dt
start, hours = sys.argv[1], float(sys.argv[2])
t = (dt.datetime.now(dt.timezone.utc).replace(tzinfo=None) + dt.timedelta(minutes=1)
     if start == "now" else dt.datetime.strptime(start, "%Y-%m-%d %H:%M"))
print((t + dt.timedelta(hours=hours)).strftime("%Y-%m-%d %H:%M"))
EOF
}

do_lease() {
  local status
  status="$(lease_field status)"
  if [[ -n "$status" ]]; then
    echo "lease $lease exists: status $status, id $(lease_field id) (reusing it)"
    return
  fi
  [[ -n "$hours" ]] || die "--hours is required to create a lease"
  python3 -c "import sys; sys.exit(0 if float('$hours') <= 168 else 1)" \
    || die "bare-metal leases are limited to 7 days (168 h); reserve less and extend"
  local end; end="$(utc_plus "${start:-now}" "$hours")"
  local nh; nh="$(python3 -c "print(f'{$nodes * $hours:g}')")"
  echo "lease $lease: $nodes x $node_type at ${OS_REGION_NAME:-?}, from ${start:-now} to $end UTC"
  confirm "This charges $nh node-hours ($nodes nodes x $hours h) to the project allocation, used or not. Create it?"
  local res="min=$nodes,max=$nodes,resource_type=physical:host,resource_properties=[\"=\", \"\$node_type\", \"$node_type\"]"
  if [[ -n "$start" ]]; then
    openstack reservation lease create --reservation "$res" --start-date "$start" --end-date "$end" "$lease" >/dev/null
  else
    openstack reservation lease create --reservation "$res" --end-date "$end" "$lease" >/dev/null
  fi
  echo "created lease $lease, id $(lease_field id)"
  echo "wait for it with: $0 wait   (or: openstack reservation lease show $lease -c status)"
}

do_wait() {
  local status
  while :; do
    status="$(lease_field status)"
    case "$status" in
      ACTIVE) echo "lease $lease is ACTIVE"; return ;;
      ""|ERROR|TERMINATED|TERMINATING|DELETED) die "lease $lease status '${status:-missing}'" ;;
      *) echo "$(date -u +%H:%M:%S) lease $lease: $status"; sleep 30 ;;
    esac
  done
}

reservation_id() {
  openstack reservation lease show "$lease" -f json | python3 -c '
import json, sys
lease = json.load(sys.stdin)
res = lease.get("reservations", [])
if isinstance(res, str):
    res = json.loads(res) if res.strip().startswith("[") else [json.loads(res)]
for r in res:
    if r.get("resource_type") == "physical:host":
        print(r["id"]); break
else:
    sys.exit("no physical:host reservation in the lease")'
}

server_ips() { openstack server show "$1" -f value -c addresses 2>/dev/null | grep -Eo '[0-9]+(\.[0-9]+){3}' || true; }

do_launch() {
  [[ "$(lease_field status)" == ACTIVE ]] || die "lease $lease is not ACTIVE yet ($0 wait)"
  [[ -n "$key_name" ]] || die "--key-name is required (an OpenStack key pair at this site)"
  openstack keypair show "$key_name" >/dev/null 2>&1 || die "no key pair '$key_name' at this site (README step 4)"
  openstack image show "$image" >/dev/null 2>&1 || {
    echo "images named CC-Ubuntu* at this site:" >&2
    openstack image list -f value -c Name | grep -i '^CC-Ubuntu' | sort >&2 || true
    die "image $image not found; pass --image"
  }
  local rid net; rid="$(reservation_id)"
  net="$(openstack network show "$network" -f value -c id)"
  local i name
  for ((i = 1; i <= nodes; i++)); do
    name="$lease-$i"
    if openstack server show "$name" >/dev/null 2>&1; then
      echo "instance $name exists (reusing it)"
    else
      openstack server create --image "$image" --flavor baremetal --key-name "$key_name" \
        --nic net-id="$net" --hint reservation="$rid" "$name" >/dev/null
      echo "launched $name (bare metal takes about 10 to 20 minutes to boot)"
    fi
  done
  local pending st
  while :; do
    pending=0
    for ((i = 1; i <= nodes; i++)); do
      st="$(openstack server show "$lease-$i" -f value -c status)"
      [[ "$st" == ERROR ]] && die "$lease-$i failed: $(openstack server show "$lease-$i" -f value -c fault)"
      [[ "$st" == ACTIVE ]] || pending=$((pending + 1))
    done
    [[ $pending -eq 0 ]] && break
    echo "$(date -u +%H:%M:%S) waiting for $pending instance(s) to become ACTIVE"
    sleep 30
  done
  mkdir -p "$(dirname "$hosts_file")"
  : > "$hosts_file.tmp"
  local ip
  for ((i = 1; i <= nodes; i++)); do
    name="$lease-$i"
    ip="$(server_ips "$name" | grep -Ev '^(10|192\.168|172\.(1[6-9]|2[0-9]|3[01]))\.' | head -1 || true)"
    if [[ -z "$ip" ]]; then
      # Reuse an unattached floating IP before allocating a new one.
      ip="$(openstack floating ip list --status DOWN -f value -c 'Floating IP Address' | head -1 || true)"
      [[ -n "$ip" ]] || ip="$(openstack floating ip create public -f value -c floating_ip_address)"
      openstack server add floating ip "$name" "$ip"
      echo "attached floating IP $ip to $name"
    fi
    echo "$name $ip" >> "$hosts_file.tmp"
  done
  mv "$hosts_file.tmp" "$hosts_file"
  echo
  echo "nodes (also written to $hosts_file):"
  while read -r name ip; do echo "  ssh cc@$ip    # $name"; done < "$hosts_file"
  echo "next: experiments/chameleon/provision.sh"
}

do_status() {
  echo "lease $lease: status $(lease_field status), id $(lease_field id), ends $(lease_field end_date)"
  openstack server list --name "^$lease-" -c Name -c Status -c Networks 2>/dev/null || true
  if [[ -f "$hosts_file" ]]; then echo "hosts file $hosts_file:"; cat "$hosts_file"; fi
}

do_extend() {
  [[ -n "$hours" ]] || die "--hours is required"
  [[ -f "$hosts_file" ]] && nodes="$(grep -c . "$hosts_file")"
  confirm "Extending $lease by $hours h charges about $(python3 -c "print(f'{$nodes * $hours:g}')") more node-hours (if it has $nodes nodes). Extend?"
  openstack reservation lease update --prolong-for "${hours}h" "$lease"
  echo "lease $lease now ends $(lease_field end_date)"
}

do_delete() {
  [[ -n "$(lease_field id)" ]] || die "no lease $lease"
  confirm "Delete lease $lease? Its instances are terminated; collect results first (collect.sh)."
  openstack reservation lease delete "$lease"
  echo "deleted lease $lease. Floating IPs stay allocated to the project; release them with:"
  echo "  openstack floating ip delete \$(cut -d' ' -f2 $hosts_file)"
}

case "$cmd" in
  up) setup; do_lease; do_wait; do_launch ;;
  lease) setup; do_lease ;;
  wait) setup; do_wait ;;
  launch) setup; do_launch ;;
  status) setup; do_status ;;
  extend) setup; do_extend ;;
  delete) setup; do_delete ;;
  help|-h|--help) usage ;;
  *) usage; exit 2 ;;
esac
