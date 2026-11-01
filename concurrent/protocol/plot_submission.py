#!/usr/bin/env python3
"""The one throughput figure of the submission's experiments section.

Default set ("writes"): the gated rule (Definition 1.1, p = 1/4, K = 1), the
exact estimator and the classic skip list on the same code, read-only, key
range 10^6, beside the splay-list and the CBTree from the SetBench fork; the
main CHI@TACC sweep, 10 repetitions. The Morris estimator is left out
because its curve lies on the gated rule's (within 2 percent at every
point); the text says so. The set "p" is the earlier plan: gated against
classic at p = 1/4, K = 1 and at p = 1/2, K = 3 (the rerun, 5 repetitions).
One row of panels, one per distribution,
throughput in Mops/s against threads, both axes logarithmic so that a
constant ratio is a constant vertical offset; the mark is the median over
repetitions and the whisker the range. Threads above 128 (SMT) are left
out, as the caption says.

Usage: plot_submission.py [--out FILE] [--dists uniform,99-1,zipf1]
                          [results_dir ...]
Defaults: results/chameleon and results/chameleon-p50 as inputs,
submission/figures/throughput_gated_classic.pdf as output.
"""
import argparse
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plot_throughput import (AQUA, BLUE, INK, MAGENTA, ORANGE, WIDTH, dist_label,  # noqa: E402
                             find_csvs, read_rows, recessive_grid, setup_matplotlib, warn)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TEXTWIDTH = 5.5  # inches; the article class with 1 in margins has 6.5 in of text width

# Two series sets. "writes" (the default) tests the counter-free motivation:
# the same code with no write on a read (gated), a write on every read
# (exact) and no adaptation (classic), beside the two published counter-based
# structures. "p" is the earlier plan: gated against classic at both p.
SERIES_WRITES = [
    (("bsl_gated", "0.25", "1"), dict(color=BLUE, marker="o", ls="-", mfc=BLUE,
                                     label="Gated rule, $p=1/4$, $K=1$")),
    (("bsl_exact", "0.25", ""), dict(color=MAGENTA, marker="s", ls="--", mfc=MAGENTA,
                                    label="Exact estimator, same code")),
    (("bsl_classic", "0.25", ""), dict(color=INK, marker="D", ls=(0, (4, 1.5)), mfc="white",
                                      label="Classic, same code")),
    (("flexlist_locks", "", ""), dict(color=ORANGE, marker="^", ls="-", mfc=ORANGE,
                                     label="Splay-list")),
    (("cbtree-concurrent", "", ""), dict(color=AQUA, marker="v", ls="-", mfc=AQUA,
                                        label="CBTree")),
]
SERIES_P = [
    (("bsl_gated", "0.25", "1"), dict(color=BLUE, marker="o", ls="-", mfc=BLUE,
                                     label="Gated rule, $p=1/4$, $K=1$")),
    (("bsl_gated", "0.5", "3"), dict(color=BLUE, marker="o", ls="--", mfc="white",
                                    label="Gated rule, $p=1/2$, $K=3$")),
    (("bsl_classic", "0.25", "1"), dict(color=INK, marker="s", ls="-", mfc=INK,
                                       label="Classic, same code, $p=1/4$")),
    (("bsl_classic", "0.5", "3"), dict(color=INK, marker="s", ls="--", mfc="white",
                                      label="Classic, same code, $p=1/2$")),
]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="*",
                    default=[os.path.join(REPO, "experiments", "results", "chameleon"),
                             os.path.join(REPO, "experiments", "results", "chameleon-p50")])
    ap.add_argument("--out", default=os.path.join(REPO, "submission", "figures",
                                                  "throughput_gated_classic.pdf"))
    ap.add_argument("--dists", default="uniform,99-1,zipf1")
    ap.add_argument("--max-threads", type=int, default=128)
    ap.add_argument("--set", choices=["writes", "p"], default="writes")
    args = ap.parse_args(argv)
    dists = args.dists.split(",")
    series = SERIES_WRITES if args.set == "writes" else SERIES_P

    rows = read_rows(find_csvs(args.paths, "throughput"),
                     ["structure", "p", "K", "key_range", "distribution", "mix",
                      "threads", "throughput_ops_s"])
    rows = [r for r in rows if r["mix"] == "100-0-0" and r["key_range"] == "1000000"
            and int(r["threads"]) <= args.max_threads]
    if not rows:
        warn("no read-only rows at key range 10^6 found")
        return 1

    plt = setup_matplotlib()
    fig, axes = plt.subplots(1, len(dists), figsize=(TEXTWIDTH, 1.75), sharey=False)
    axes = list(axes) if len(dists) > 1 else [axes]
    threads_all = sorted({int(r["threads"]) for r in rows})
    for ax, dist in zip(axes, dists):
        drows = [r for r in rows if r["distribution"] == dist]
        for key, style in series:
            s, p, k = key

            def match(r, s=s, p=p, k=k):
                # an empty p or K in the key means "do not filter on it"
                return (r["structure"] == s and (not p or r["p"] == p)
                        and (not k or r["K"] == k))
            xs, med, lo, hi = [], [], [], []
            for t in threads_all:
                v = [float(r["throughput_ops_s"]) / 1e6 for r in drows
                     if match(r) and int(r["threads"]) == t]
                if not v:
                    continue
                m = st.median(v)
                xs.append(t); med.append(m); lo.append(m - min(v)); hi.append(max(v) - m)
            if not xs:
                warn(f"{dist}: no rows for {key}")
                continue
            ax.errorbar(xs, med, yerr=[lo, hi], capsize=1.2, elinewidth=0.6, lw=1.1,
                        markersize=3.4, markeredgewidth=0.8, **style)
        ax.set_xscale("log", base=2)
        ax.set_xticks([1, 8, 32, 128])
        ax.set_xticklabels(["1", "8", "32", "128"])
        ax.set_yscale("log", base=10)
        ax.set_xlabel("threads")
        ax.set_title(dist_label(dist), fontsize=8, loc="left", pad=2)
        recessive_grid(ax)
    axes[0].set_ylabel("throughput (Mops/s)")
    handles, labels = axes[0].get_legend_handles_labels()
    ncol = 3 if len(labels) > 4 else 2
    fig.legend(handles, labels, loc="lower center", ncol=ncol, bbox_to_anchor=(0.5, -0.36),
               handlelength=2.2, columnspacing=1.2)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    fig.savefig(args.out)
    print(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
