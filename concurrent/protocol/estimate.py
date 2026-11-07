#!/usr/bin/env python3
"""Node-hours and wall-clock for a parameter set, before reserving.

Takes the same protocol flags as run_protocol.py (it builds the same plan
with the same code), so the count it prints is the count the runner will
run. Per run it charges warmup + measured time plus a fixed overhead for
process start, prefill and teardown (--overhead, default 5 s; a 10^6-key
prefill is a single-threaded insertion of 10^6 random keys).

  python3 estimate.py                       # the full protocol
  python3 estimate.py --reps 5 --mixes 100-0-0
  python3 estimate.py --nodes 1,3,5

Chameleon charges a lease for its full length times the number of nodes,
whether or not the nodes are busy, so reserve the wall-clock printed here
plus the setup margin, and delete the lease when the runs finish.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_protocol as rp  # noqa: E402

MAX_LEASE_H = 7 * 24  # Chameleon's bare-metal lease limit, extendable


def plan_for(args, cold):
    ns = argparse.Namespace(**vars(args))
    ns.cold_start = cold
    ns.shard = None
    max_threads = 256 if not args.unpatched else rp.UNPATCHED_MAX_THREADS
    return rp.build_plan(ns, max_threads, seeded=not args.unpatched)


def summarise(label, plan, overhead):
    measured = sum(r.seconds for r in plan)
    total = measured + overhead * len(plan)
    print(f"{label}: {len(plan)} runs; {measured / 3600:.1f} h warmup+measured "
          f"+ {overhead * len(plan) / 3600:.1f} h overhead = {total / 3600:.1f} node-hours")
    return total / 3600


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    rp.add_arguments(ap)
    ap.add_argument("--overhead", type=float, default=5.0,
                    help="seconds per run for start, prefill, teardown (default 5)")
    ap.add_argument("--nodes", type=rp.int_list, default=[1, 3],
                    help="node counts to show wall-clock for (default 1,3)")
    ap.add_argument("--setup-hours", type=float, default=1.0,
                    help="per-lease margin for boot, provisioning and build (default 1)")
    ap.add_argument("--unpatched", action="store_true",
                    help="plan as if harness.patch were not applied (threads <= 99)")
    ap.add_argument("--only", choices=["throughput", "cold"],
                    help="estimate one mode only")
    args = ap.parse_args(argv)

    total = 0.0
    sections = []
    if args.only != "cold":
        main_plan = plan_for(args, cold=False)
        by_mix = {}
        for r in main_plan:
            by_mix.setdefault(r.mix, []).append(r)
        for mix, runs in by_mix.items():
            structs = sorted({r.structure for r in runs})
            print(f"  mix {mix}: {len(structs)} structures x "
                  f"{len(args.distributions)} distributions x {len(args.key_ranges)} key ranges x "
                  f"{len({r.threads for r in runs})} thread counts x {args.reps} reps = {len(runs)} runs")
        total += summarise("throughput", main_plan, args.overhead)
        sections.append("throughput")
    if args.only != "throughput":
        cold_plan = plan_for(args, cold=True)
        nwin = max(len(args.windows) - 1, 0)
        span = args.windows[-1] if args.windows else 0
        print(f"  cold start: {len(args.structures)} structures x {len(args.cold_distributions)} "
              f"distributions x {len(args.cold_key_ranges)} key ranges x "
              f"{len(args.cold_threads)} thread counts x {args.reps} reps x {nwin} windows "
              f"(one curve = {sum(args.windows[1:])} s of running for a {span} s span)")
        total += summarise("cold start", cold_plan, args.overhead)
        sections.append("cold start")

    print(f"total ({' + '.join(sections)}): {total:.1f} node-hours")
    for n in args.nodes:
        wall = total / n
        lease = wall + args.setup_hours
        note = "" if lease <= MAX_LEASE_H else f"  (exceeds the {MAX_LEASE_H} h lease limit: extend or add nodes)"
        print(f"  on {n} node{'s' if n > 1 else ''}: {wall:.1f} h wall-clock "
              f"({wall / 24:.2f} days); lease {lease:.0f} h x {n} = {lease * n:.0f} node-hours charged{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
