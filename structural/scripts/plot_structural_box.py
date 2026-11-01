#!/usr/bin/env python3
"""Box plots of the per-search ratio cost_t / a_t (Theorem A) and of the
whole-run ratio cost / Ent(sigma) over runs (Theorem C), one panel per
distribution, one box per configuration and n.

Inputs, under a results directory:
  structural_hist_<dist>[_n10k|_n100k|...].csv   from `structural --hist=`
      policy,run,bin_lo,count,ratio_max,dist,n,p: the whole-run histogram of
      cost_t / a_t in 0.05 bins (lower edge), one row per arm, run and
      non-empty bin.
  structural_<dist>[...].csv                      the harness's main output;
      the last sample row of each (policy, run) gives cost_over_entropy.

Top row (Theorem A): the box is the first to third quartile of cost_t / a_t
over all accesses of all runs of the configuration, the whiskers the 1st and
99th percentiles, the dot the largest ratio in any run. Quantiles are read
from the histogram (upper edge of the bin). Bottom row (Theorem C): the box
is the first to third quartile of cost / Ent over the runs, the whiskers the
smallest and largest run, the line the median. Theorem C bounds this ratio
by a constant plus F log(1/delta) / Ent, a constant fraction at fixed delta;
it predicts the boxes stay level as n grows, not that they narrow.

Usage: plot_structural_box.py [results_dir] [--out FILE] [--dists LIST]
                              [--arms LIST] [--ns LIST]
"""
import argparse
import bisect
import csv
import glob
import math
import os
import re
import statistics as st
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))

ARMS = [("classic", "classic"), ("gated-K1", "rule, $K=1$"), ("gated-K3", "rule, $K=3$"),
        ("exact", "exact"), ("morris", "Morris")]
DIST_LABEL = {"zipf1.5": "Zipf 1.5", "zipf1.0": "Zipf 1", "hot90-10": "90/10", "uniform": "uniform"}
# Series colour follows the configuration (fixed order, never cycled).
COLORS = {"classic": "#3b4048", "gated-K1": "#2a78d6", "gated-K3": "#1baf7a",
          "exact": "#e87ba4", "morris": "#6250d6"}
INK_LINE = "#52514e"
N_LABEL = {1000: "$10^{3}$", 10000: "$10^{4}$", 100000: "$10^{5}$", 1000000: "$10^{6}$"}


def read_hist(paths):
    """(dist, n, policy) -> list over runs of (hist dict bin_lo->count, max)."""
    out = defaultdict(lambda: defaultdict(lambda: [defaultdict(int), 0.0]))
    for path in paths:
        with open(path) as f:
            for r in csv.DictReader(f):
                key = (r["dist"], int(r["n"]), r["policy"])
                run = int(r["run"])
                h = out[key][run]
                h[0][float(r["bin_lo"])] += int(r["count"])
                h[1] = max(h[1], float(r["ratio_max"]))
    return out


def pooled_quantiles(runs, qs, bin_width=0.05):
    """Quantiles of the pooled histogram (upper bin edge, capped at the max)."""
    pooled = defaultdict(int)
    mx = 0.0
    for h, m in runs.values():
        for lo, c in h.items():
            pooled[lo] += c
        mx = max(mx, m)
    total = sum(pooled.values())
    res = {}
    acc = 0
    targets = sorted((q, math.ceil(q * total)) for q in qs)
    i = 0
    for lo in sorted(pooled):
        acc += pooled[lo]
        while i < len(targets) and acc >= targets[i][1]:
            res[targets[i][0]] = min(mx, lo + bin_width)
            i += 1
        if i == len(targets):
            break
    for q, _ in targets:
        res.setdefault(q, mx)
    return res, mx


def read_cost_over_ent(paths):
    """(dist, n, policy) -> {run: cost/Ent at the last sample}."""
    last = {}
    for path in paths:
        with open(path) as f:
            for r in csv.DictReader(f):
                if r.get("n") is None or r.get("run") is None:
                    continue  # a row still being written by a running harness
                key = (r["dist"], int(r["n"]), r["policy"], int(r["run"]))
                t = int(r["t"])
                if key not in last or t > last[key][0]:
                    last[key] = (t, float(r["cost_over_entropy"]))
    out = defaultdict(dict)
    for (d, n, pol, run), (_, v) in last.items():
        out[(d, n, pol)][run] = v
    return out


def read_deviations(paths):
    """(dist, n, policy) -> list over runs of cost / F, from the final row of
    each run of a many-run file (the *_R200.csv files)."""
    last = {}
    for path in paths:
        with open(path) as f:
            for r in csv.DictReader(f):
                if r.get("n") is None or r.get("run") is None:
                    continue
                key = (r["dist"], int(r["n"]), r["policy"], int(r["run"]))
                t = int(r["t"])
                if key not in last or t > last[key][0]:
                    last[key] = (t, float(r["cost_total"]) / t)
    out = defaultdict(list)
    for (d, n, pol, _), (_, v) in last.items():
        out[(d, n, pol)].append(v)
    return out


def quartiles(vals):
    v = sorted(vals)
    k = len(v)

    def q(p):
        if k == 1:
            return v[0]
        x = p * (k - 1)
        i = int(math.floor(x))
        j = min(k - 1, i + 1)
        return v[i] + (v[j] - v[i]) * (x - i)
    return v[0], q(0.25), q(0.5), q(0.75), v[-1]


def setup():
    import logging
    import matplotlib
    logging.getLogger("fontTools").setLevel(logging.ERROR)
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib import font_manager
    available = {f.name for f in font_manager.fontManager.ttflist}
    serif = [f for f in ("cmr10", "STIX Two Text", "Times", "DejaVu Serif") if f in available]
    plt.rcParams.update({
        "font.family": "serif", "font.serif": serif or ["serif"], "font.size": 8,
        "axes.titlesize": 8, "axes.labelsize": 8, "xtick.labelsize": 7, "ytick.labelsize": 7,
        "legend.fontsize": 7, "axes.spines.top": False, "axes.spines.right": False,
        "axes.linewidth": 0.6, "xtick.major.width": 0.6, "ytick.major.width": 0.6,
        "axes.axisbelow": True, "pdf.fonttype": 42, "savefig.bbox": "tight",
        "savefig.pad_inches": 0.02, "mathtext.fontset": "cm",
    })
    return plt


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("results", nargs="?", default=os.path.join(HERE, "..", "results"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--dists", default="zipf1.5,zipf1.0,hot90-10,uniform")
    ap.add_argument("--arms", default=",".join(a for a, _ in ARMS))
    ap.add_argument("--ns", default="", help="comma list; default: every n found")
    ap.add_argument("--textwidth", type=float, default=5.5)
    ap.add_argument("--tail-arms", default="classic,gated-K1,exact",
                    help="configurations drawn in the deviation tail (bottom row)")
    args = ap.parse_args(argv)
    dists = args.dists.split(",")
    arms = [(a, l) for a, l in ARMS if a in args.arms.split(",")]

    hist = read_hist(glob.glob(os.path.join(args.results, "structural_hist_*.csv")))
    main_csvs = [p for p in glob.glob(os.path.join(args.results, "structural_*.csv"))
                 if not re.search(r"structural_(hist|items)_", os.path.basename(p))]
    coe = read_cost_over_ent([p for p in main_csvs if "_R200" not in p])
    dev = read_deviations([p for p in main_csvs if "_R200" in p])
    tail_arms = [(a, l) for a, l in arms if a in args.tail_arms.split(",")]
    ns = sorted({n for (_, n, _) in hist}) if not args.ns else [int(x) for x in args.ns.split(",")]
    if not hist:
        print("no structural_hist_*.csv found", file=sys.stderr)
        return 1

    plt = setup()
    fig, axes = plt.subplots(2, len(dists), figsize=(args.textwidth, 2.9), sharey="row")
    axes = axes.reshape(2, len(dists))
    group = len(arms) + 1  # slots per n: one per arm plus a gap
    for col, dist in enumerate(dists):
        axA, axC = axes[0][col], axes[1][col]
        present_ns = [n for n in ns if any((dist, n, a) in hist for a, _ in arms)]
        for gi, n in enumerate(present_ns):
            for ai, (arm, _) in enumerate(arms):
                x = gi * group + ai
                c = COLORS[arm]
                key = (dist, n, arm)
                if key in hist:
                    q, mx = pooled_quantiles(hist[key], [0.01, 0.25, 0.5, 0.75, 0.99])
                    axA.add_patch(plt.Rectangle((x - 0.36, q[0.25]), 0.72, q[0.75] - q[0.25],
                                                facecolor=c, edgecolor=c, lw=0.6, alpha=0.85))
                    axA.plot([x - 0.36, x + 0.36], [q[0.5]] * 2, color="white", lw=0.8)
                    axA.plot([x, x], [q[0.01], q[0.25]], color=c, lw=0.7)
                    axA.plot([x, x], [q[0.75], q[0.99]], color=c, lw=0.7)
                    axA.plot([x], [mx], marker=".", ms=3, color=c, ls="none")
                if key in coe and not dev:
                    lo, q1, med, q3, hi = quartiles(coe[key].values())
                    axC.add_patch(plt.Rectangle((x - 0.36, q1), 0.72, max(q3 - q1, 1e-9),
                                                facecolor=c, edgecolor=c, lw=0.6, alpha=0.85))
                    axC.plot([x - 0.36, x + 0.36], [med] * 2, color="white", lw=0.8)
                    axC.plot([x, x], [lo, q1], color=c, lw=0.7)
                    axC.plot([x, x], [q3, hi], color=c, lw=0.7)
        if dev:
            # Bottom row: the tail of the whole-run deviation per access,
            # P[(cost - median cost) / F >= u] over the R runs, log axis; one
            # colour per configuration, solid for the smaller n, dashed for
            # the larger. Theorem C says this tail is at most e^{-u/c} with a
            # c that depends on neither n nor F.
            umax = 0.0
            for ni, n in enumerate(sorted({nn for (dd, nn, _) in dev if dd == dist})):
                ls = ["-", "--", ":"][ni % 3]
                for arm, _ in tail_arms:
                    vals = dev.get((dist, n, arm))
                    if not vals:
                        continue
                    med = st.median(vals)
                    xs = sorted(v - med for v in vals)
                    R = len(xs)
                    us = [i * 0.02 for i in range(int(max(xs[-1], 0) / 0.02) + 1)] + [xs[-1]]
                    ps = [(R - bisect.bisect_left(xs, u)) / R for u in us]
                    pts = [(u, p) for u, p in zip(us, ps) if u >= 0 and p > 0]
                    if not pts:
                        continue
                    umax = max(umax, pts[-1][0])
                    axC.plot([u for u, _ in pts], [p for _, p in pts], color=COLORS[arm], ls=ls, lw=0.9,
                             label=f"{dict(arms)[arm]}, $n={str(N_LABEL.get(n, n)).strip(chr(36))}$")
            axC.set_yscale("log")
            axC.set_xlim(0, max(0.5, umax) * 1.05)
            axC.set_xlabel("$u$")
            axC.grid(axis="y", color="#e1e0d9", lw=0.5)
        for ax in ((axA,) if dev else (axA, axC)):
            ax.set_xlim(-0.8, len(present_ns) * group - 1.2)
            ax.set_xticks([gi * group + (len(arms) - 1) / 2 for gi in range(len(present_ns))])
            ax.set_xticklabels([N_LABEL.get(n, str(n)) for n in present_ns])
            ax.grid(axis="y", color="#e1e0d9", lw=0.5)
            ax.tick_params(axis="x", length=0)
        axA.set_title(DIST_LABEL.get(dist, dist), loc="left", pad=2)
        if not dev:
            axC.set_xlabel("$n$")
    axes[0][0].set_ylabel("$\\mathrm{cost}_t / a_t$")
    axes[1][0].set_ylabel("$\\mathrm{P}[\\,\\mathrm{dev}/F \\geq u\\,]$" if dev
                          else "$\\mathrm{cost} / \\mathrm{Ent}(\\sigma)$")
    axes[0][0].set_yscale("log")
    handles = [plt.Rectangle((0, 0), 1, 1, facecolor=COLORS[a], edgecolor=COLORS[a]) for a, _ in arms]
    labels = [l for _, l in arms]
    if dev:
        import matplotlib.lines as mlines
        ns_dev = sorted({nn for (_, nn, _) in dev})
        handles += [mlines.Line2D([], [], color=INK_LINE, ls=["-", "--", ":"][i % 3], lw=0.9)
                    for i in range(len(ns_dev))]
        labels += [f"$n={str(N_LABEL.get(n, n)).strip(chr(36))}$ (tail)" for n in ns_dev]
    fig.legend(handles, labels, loc="lower center", ncol=len(labels),
               bbox_to_anchor=(0.5, -0.08), handlelength=1.6, columnspacing=1.2, frameon=False)
    fig.subplots_adjust(hspace=0.35, wspace=0.12)
    out = args.out or os.path.join(args.results, "structural_box.pdf")
    fig.savefig(out)
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
