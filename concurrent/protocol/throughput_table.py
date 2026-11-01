#!/usr/bin/env python3
"""Print the submission's throughput table (LaTeX rows) from the Chameleon CSVs.

Usage: throughput_table.py [results_dir ...]   (default: results/chameleon
and results/chameleon-p50 next to this script's repository root)

Rows: the rule at p = 1/4, K = 1, the Morris and exact estimators and the
classic skip list (one implementation), the splay-list and the CBTree.
Columns, per distribution: mean search path at 64 threads (levels visited
plus forward steps for the four skip lists of one implementation; the
CBTree's count (*) is tree nodes and the splay-list's (dagger) descends
from a fixed top level, so neither is comparable) and the median throughput over
the ten runs, in millions of operations per second, at 64 and 256 threads.
Read-only workload, 10^6 keys.
"""
import csv, glob, os, statistics, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
ROWS = [("bsl_gated", "0.25", "1", r"\Cref{def:rule}, $p=1/4$, $K=1$"),
        ("bsl_morris", "0.25", "1", "Morris estimator"),
        ("bsl_exact", "0.25", "1", "exact estimator"),
        ("bsl_classic", "0.25", "1", "classic"),
        ("flexlist_locks", "", "", r"splay-list \cite{SplayList2020}"),
        ("cbtree-concurrent", "", "", r"CBTree \cite{AfekEtAl2012CBTree}")]
DISTS = ["zipf1", "99-1", "uniform"]
THREADS = [64, 256]


def main(argv):
    dirs = argv or [os.path.join(ROOT, "results", "chameleon"),
                    os.path.join(ROOT, "results", "chameleon-p50")]
    rows = [r for d in dirs for f in glob.glob(os.path.join(d, "*", "throughput_tacc_*.csv"))
            for r in csv.DictReader(open(f))]
    thr, path = {}, {}
    for r in rows:
        if r["write_frac"] not in ("0", "0.0") or r["key_range"] != "1000000":
            continue
        k = (r["structure"], r["p"], r["K"], r["distribution"], int(r["threads"]))
        thr.setdefault(k, []).append(float(r["throughput_ops_s"]) / 1e6)
        try:
            path.setdefault(k, []).append(float(r["path_length"]))
        except ValueError:
            pass
    for s, p, K, label in ROWS:
        cells = []
        for d in DISTS:
            # The path is reported for the four skip lists that share one
            # implementation (levels visited plus forward steps). The
            # splay-list and the CBTree count their own, different, units.
            pl = path.get((s, p, K, d, 64))
            mark = "" if s.startswith("bsl_") else ("^{*}" if "cbtree" in s else "^{\\dagger}")
            cells.append(("%.1f" % statistics.mean(pl)) + mark if pl else "--")
            for t in THREADS:
                v = thr.get((s, p, K, d, t))
                cells.append("%.0f" % statistics.median(v) if v else "--")
        print(f"{label} & " + " & ".join(f"${c}$" for c in cells) + r" \\")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
