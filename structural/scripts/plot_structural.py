#!/usr/bin/env python3
"""Figures and tables for the structural part of the experiments section.

Reads the CSVs that build/structural writes into results/:

  structural_<dist>.csv        n = 1000, one per distribution
                               (zipf1.5, zipf1.0, uniform, hot90-10)
  structural_zipf1.5_n10k.csv  the n = 10 000 confirmation (tables only)
  structural_uniform_n10k.csv  the n = 10 000 hazard check (tables only)

and writes one PDF per figure into the same directory:

  structural_cost.pdf   total cost / Ent(sigma) against accesses, one panel
                        per distribution, mean over runs with an sd band
  structural_kl.pdf     KL(OPT || OBS) against accesses, log y; geometric
                        mean over runs with a multiplicative sd band
  structural_depth.pdf  top level hmax (left) and depth (right) against
                        accesses on log x, one row per distribution, with the
                        guide line log_b t
  structural_space.pdf  bits per item against accesses (log x), compressed
                        (solid) and uncompressed (dashed), for the arms whose
                        space differs: classic, gated-K3, exact, morris;
                        Zipf 1.5 and uniform

With --tables it also prints the final-value tables (mean and sd over runs)
as Markdown on stdout, for results/structural_summary.md.

Figures are 3.3 in wide (one column), carry no titles (captions belong in
the paper), and share plot_reseed.py's style (Computer Modern, palette,
markers that survive greyscale). If a CSV or a column a figure needs is
missing, the script says so and skips that figure or series; it never fills
in data.

Usage: plot_structural.py [results_dir] [--tables] [--arms=a,b,c] [--out=DIR]

--arms restricts every figure to the named arms (the submission's figures
leave the legacy rules out); --out writes the PDFs to another directory.
"""
import bisect
import csv
import math
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plot_reseed import WIDTH, recessive_grid, setup_matplotlib  # noqa: E402

DISTS = ["zipf1.5", "zipf1.0", "uniform", "hot90-10"]
DIST_LABEL = {
    "zipf1.5": r"Zipf $s=1.5$",
    "zipf1.0": r"Zipf $s=1.0$",
    "uniform": "uniform",
    "hot90-10": "90/10",
}
EXTRA = {
    "zipf1.5_n10k": "Zipf $s=1.5$, $n=10^4$",
    "zipf1.0_n10k": "Zipf $s=1.0$, $n=10^4$",
    "hot90-10_n10k": "90/10, $n=10^4$",
    "uniform_n10k": "uniform, $n=10^4$",
    "zipf1.5_n100k": "Zipf $s=1.5$, $n=10^5$",
    "zipf1.0_n100k": "Zipf $s=1.0$, $n=10^5$",
    "hot90-10_n100k": "90/10, $n=10^5$",
    "uniform_n100k": "uniform, $n=10^5$",
}

# Categorical slots 1-7 of the dataviz reference palette, fixed order
# (validated: adjacent CVD separation >= 9.1, normal-vision >= 19.6). Slots
# 1-3 are plot_reseed.py's. The legacy rules are dashed with open markers.
ARMS = ["classic", "gated-K1", "gated-K3", "exact", "morris",
        "exact-legacy", "morris-legacy"]
ARM_STYLE = {
    "classic": dict(color="#2a78d6", marker="o", ls="-", mfc=None),
    "gated-K1": dict(color="#eb6834", marker="s", ls="-", mfc=None),
    "gated-K3": dict(color="#1baf7a", marker="^", ls="-", mfc=None),
    "exact": dict(color="#eda100", marker="D", ls="-", mfc=None),
    "morris": dict(color="#e87ba4", marker="v", ls="-", mfc=None),
    "exact-legacy": dict(color="#008300", marker="D", ls="--", mfc="white"),
    "morris-legacy": dict(color="#6250d6", marker="v", ls="--", mfc="white"),
}
REF_GREY = "0.35"
NEED = ["policy", "run", "t", "p", "n", "seed"]


def warn(msg):
    print(f"plot_structural: {msg}", file=sys.stderr)


def read_csv(path, needed):
    """Rows of `path`, or None (with a message) if the file or a needed
    column is missing."""
    if not os.path.exists(path):
        warn(f"{path} not found")
        return None
    with open(path) as fh:
        reader = csv.DictReader(fh)
        missing = [c for c in needed if c not in (reader.fieldnames or [])]
        if missing:
            warn(f"{path} lacks column(s) {', '.join(missing)}")
            return None
        return list(reader)


def load(results, name):
    rows = read_csv(os.path.join(results, f"structural_{name}.csv"), NEED)
    if rows is None:
        return None
    return {"rows": rows, "cols": set(rows[0].keys()) if rows else set()}


def series(data, arm, col, scale=1.0, geometric=False):
    """(ts, centre, lo, hi) over runs of `col` for `arm`, or None if absent:
    mean and mean -/+ sd, or with `geometric` (for log axes, where mean - sd
    can be negative) the geometric mean and its multiplicative sd band."""
    if data is None:
        return None
    if col not in data["cols"]:
        warn(f"column {col} missing, series skipped")
        return None
    by_t = {}
    for r in data["rows"]:
        if r["policy"] != arm or r[col] in ("", "nan"):
            continue
        by_t.setdefault(int(r["t"]), []).append(float(r[col]) * scale)
    if not by_t:
        warn(f"no rows for arm {arm}, series skipped")
        return None
    ts = sorted(by_t)
    if geometric:
        logs = {t: [math.log(v) for v in by_t[t] if v > 0] for t in ts}
        ts = [t for t in ts if logs[t]]
        mu = [st.mean(logs[t]) for t in ts]
        sd = [st.stdev(logs[t]) if len(logs[t]) > 1 else 0.0 for t in ts]
        return (ts, [math.exp(m) for m in mu],
                [math.exp(m - d) for m, d in zip(mu, sd)],
                [math.exp(m + d) for m, d in zip(mu, sd)])
    mean = [st.mean(by_t[t]) for t in ts]
    sd = [st.stdev(by_t[t]) if len(by_t[t]) > 1 else 0.0 for t in ts]
    return (ts, mean, [m - d for m, d in zip(mean, sd)],
            [m + d for m, d in zip(mean, sd)])


def log_marks(ts, k=6):
    """Indices of about k samples evenly spaced on a log axis."""
    lo, hi = math.log(ts[0]), math.log(ts[-1])
    idx = []
    for i in range(k):
        target = lo + (hi - lo) * (i + 0.5) / k
        j = min(range(len(ts)), key=lambda m: abs(math.log(ts[m]) - target))
        if j not in idx:
            idx.append(j)
    return idx


def draw(ax, s, arm, ls=None, band=True, mfc=None, label=True, logx=False):
    ts, mean, lo, hi = s
    style = ARM_STYLE[arm]
    face = mfc if mfc is not None else style["mfc"]
    every = max(1, len(ts) // 6)
    marks = log_marks(ts) if logx else (every // 2, every)
    ax.plot(ts, mean, color=style["color"], ls=ls or style["ls"],
            marker=style["marker"], markevery=marks,
            mfc=face if face else style["color"], mec=style["color"],
            mew=0.7, label=arm if label else None)
    if band:
        ax.fill_between(ts, lo, hi,
                        color=style["color"], alpha=0.15, lw=0)


def legend_above(fig, axes, ncol=4, y=0.985):
    handles, labels = [], []
    for ax in axes:
        for h, lab in zip(*ax.get_legend_handles_labels()):
            if lab not in labels:
                handles.append(h)
                labels.append(lab)
    if handles:
        fig.legend(handles, labels, loc="lower center", ncol=ncol,
                   bbox_to_anchor=(0.5, y), handlelength=2.2,
                   columnspacing=1.0)


def four_panels(plt, results, col, ylabel, logy=False):
    data = {d: load(results, d) for d in DISTS}
    if not any(data.values()):
        warn(f"{col}: no data, figure skipped")
        return None
    fig, axes = plt.subplots(2, 2, figsize=(WIDTH, 3.3), sharex=True)
    for ax, d in zip(axes.flat, DISTS):
        for arm in ARMS:
            s = series(data[d], arm, col, geometric=logy)
            if s is None:
                continue
            draw(ax, ([t / 1e5 for t in s[0]],) + s[1:], arm)
        if logy:
            ax.set_yscale("log")
        recessive_grid(ax)
        ax.set_xlim(left=0)
        if logy:
            ax.text(0.03, 0.03, DIST_LABEL[d], transform=ax.transAxes,
                    ha="left", va="bottom", fontsize=7)
        else:
            ax.text(0.97, 0.95, DIST_LABEL[d], transform=ax.transAxes,
                    ha="right", va="top", fontsize=7)
    for ax in axes[1]:
        ax.set_xlabel(r"accesses ($\times10^5$)")
    fig.supylabel(ylabel, fontsize=8, x=0.02)
    fig.tight_layout(h_pad=0.5, w_pad=0.6)
    legend_above(fig, axes.flat)
    return fig


def plot_cost(plt, results):
    return four_panels(plt, results, "cost_over_entropy",
                       r"total cost $/\ \mathrm{Ent}(\sigma)$")


def plot_kl(plt, results):
    return four_panels(plt, results, "kl",
                       r"$\mathrm{KL}(\mathrm{OPT}\,\|\,\mathrm{OBS})$",
                       logy=True)


def plot_depth(plt, results):
    data = {d: load(results, d) for d in DISTS}
    if not any(data.values()):
        warn("depth: no data, figure skipped")
        return None
    fig, axes = plt.subplots(4, 2, figsize=(WIDTH, 5.0), sharex=True,
                             sharey=True)
    for row, d in zip(axes, DISTS):
        p = None
        for ax, col in zip(row, ("hmax", "depth")):
            for arm in ARMS:
                s = series(data[d], arm, col)
                if s is None:
                    continue
                draw(ax, s, arm, band=False, logx=True)
            if data[d] is not None and data[d]["rows"]:
                p = float(data[d]["rows"][0]["p"])
                ts = sorted({int(r["t"]) for r in data[d]["rows"]})
                guide = [math.log(t) / math.log(1.0 / p) for t in ts]
                ax.plot(ts, guide, color=REF_GREY, ls=":", lw=0.9,
                        label=r"$\log_b t$")
            ax.set_xscale("log")
            recessive_grid(ax)
        row[0].set_ylabel(DIST_LABEL[d])
    # Column keys inside the top panels (no titles, as in plot_reseed.py).
    for ax, key in zip(axes[0], (r"$H_{\max}$", r"$H_{\max}-\min_x H_x$")):
        ax.text(0.03, 0.95, key, transform=ax.transAxes, ha="left",
                va="top", fontsize=7)
    for ax in axes[-1]:
        ax.set_xlabel("accesses $t$")
    fig.tight_layout(h_pad=0.4, w_pad=0.6)
    legend_above(fig, axes.flat, y=0.99)
    return fig


SPACE_ARMS = ["classic", "gated-K3", "exact", "morris"]
SPACE_DISTS = ["zipf1.5", "uniform"]


def plot_space(plt, results):
    data = {d: load(results, d) for d in SPACE_DISTS}
    if not any(data.values()):
        warn("space: no data, figure skipped")
        return None
    fig, axes = plt.subplots(1, 2, figsize=(WIDTH, 2.0), sharey=True)
    for ax, d in zip(axes, SPACE_DISTS):
        for arm in SPACE_ARMS:
            s = series(data[d], arm, "bits_per_item")
            if s is not None:
                draw(ax, s, arm, ls="-", logx=True)
            s = series(data[d], arm, "bits_per_item_uncompressed")
            if s is not None:
                draw(ax, s, arm, ls="--", mfc="white", label=False,
                     logx=True)
        ax.set_xscale("log")
        recessive_grid(ax)
        ax.set_xlabel("accesses $t$")
        ax.text(0.03, 0.97, DIST_LABEL[d], transform=ax.transAxes,
                ha="left", va="top", fontsize=7)
    axes[0].set_ylabel("bits per item")
    fig.tight_layout(w_pad=0.6)
    # Line style key: solid compressed, dashed uncompressed.
    from matplotlib.lines import Line2D
    handles, labels = axes[0].get_legend_handles_labels()
    handles += [Line2D([], [], color=REF_GREY, ls="-"),
                Line2D([], [], color=REF_GREY, ls="--")]
    labels += ["compressed", "uncompressed"]
    fig.legend(handles, labels, loc="lower center", ncol=3,
               bbox_to_anchor=(0.5, 0.98), handlelength=2.2,
               columnspacing=1.0)
    return fig


# ---------------------------------------------------------------------------
# Tables
# ---------------------------------------------------------------------------

def final_rows(data):
    T = max(int(r["t"]) for r in data["rows"])
    return T, [r for r in data["rows"] if int(r["t"]) == T]


def ms(vals, fmt):
    m = st.mean(vals)
    s = st.stdev(vals) if len(vals) > 1 else 0.0
    return f"{m:{fmt}} ({s:{fmt}})"


def print_tables(results):
    names = DISTS + list(EXTRA)
    for name in names:
        data = load(results, name)
        if data is None or not data["rows"]:
            continue
        T, rows = final_rows(data)
        n = int(rows[0]["n"])
        runs = len({r["run"] for r in rows})
        arms = []
        for r in rows:
            if r["policy"] not in arms:
                arms.append(r["policy"])
        label = DIST_LABEL.get(name, EXTRA.get(name, name))
        ent = st.mean(float(r["entropy"]) for r in rows) / T
        print(f"### {name} (n = {n}, F = {T}, runs = {runs}, "
              f"Ent/F = {ent:.3f}, p = {rows[0]['p']}, "
              f"seed = {rows[0]['seed']})\n")
        print("Cost and shape at t = F, mean (sd) over runs.\n")
        print("| arm | cost/Ent | cost/A | q99 cost_t/a_t | max cost_t/a_t "
              "| KL | hmax | depth | gap mean | gap min | gap max |")
        print("|---|---|---|---|---|---|---|---|---|---|---|")
        for arm in arms:
            rr = [r for r in rows if r["policy"] == arm]
            f = lambda c: [float(r[c]) for r in rr]  # noqa: E731
            print(f"| {arm} | {ms(f('cost_over_entropy'), '.3f')} "
                  f"| {ms(f('cost_over_a'), '.3f')} "
                  f"| {ms(f('ratio_q99'), '.2f')} "
                  f"| {max(f('ratio_max')):.2f} "
                  f"| {ms(f('kl'), '.3f')} "
                  f"| {ms(f('hmax'), '.1f')} "
                  f"| {ms(f('depth'), '.1f')} "
                  f"| {ms(f('hgap_mean'), '.2f')} "
                  f"| {min(f('hgap_min')):.2f} "
                  f"| {max(f('hgap_max')):.2f} |")
        print()
        # Space: the maximum compressed count over every sample and run.
        print("Space and hazard at t = F, mean (sd) over runs; "
              "\"max ptr/n\" is over every sample of every run.\n")
        print("| arm | compressed ptr/n (head incl.) | max ptr/n "
              "| items only ptr/n | uncompressed ptr/n "
              "| bits/item compressed | bits/item uncompressed "
              "| hazard_min | min hazard_min |")
        print("|---|---|---|---|---|---|---|---|---|")
        for arm in arms:
            rr = [r for r in rows if r["policy"] == arm]
            allr = [r for r in data["rows"] if r["policy"] == arm]
            f = lambda c: [float(r[c]) for r in rr]  # noqa: E731
            pc = [v / n for v in f("ptr_compressed")]
            pmax = max(float(r["ptr_compressed"]) for r in allr) / n
            pi = [v / n for v in f("ptr_compressed_items")]
            pu = [v / n for v in f("ptr_uncompressed")]
            hz = [v for v in f("hazard_min") if not math.isnan(v)]
            print(f"| {arm} | {ms(pc, '.3f')} | {pmax:.3f} | {ms(pi, '.3f')} "
                  f"| {ms(pu, '.2f')} | {ms(f('bits_per_item'), '.1f')} "
                  f"| {ms(f('bits_per_item_uncompressed'), '.1f')} "
                  f"| {ms(hz, '.3f')} | {min(hz):.3f} |")
        print()


GAP_DISTS = ["zipf1.5", "uniform"]


def plot_gap(plt, results):
    """Upper tail of the height gap H_x - log_b(1 + f_x) across all items of
    all runs at t = F (from structural_items_<dist>.csv, written with
    --items=), against lem:heightupper's p^(g - K) with K = 1."""
    import math
    data = {}
    for d in GAP_DISTS:
        rows = read_csv(os.path.join(results, f"structural_items_{d}.csv"),
                        ["policy", "run", "f", "h", "p"])
        if rows:
            data[d] = rows
    if not data:
        warn("gap: no structural_items_*.csv, figure skipped")
        return None
    fig, axes = plt.subplots(1, 2, figsize=(WIDTH, 2.0), sharey=True)
    thresholds = [g / 2 for g in range(-8, 33)]  # -4 to 16 in halves
    for ax, d in zip(axes, GAP_DISTS):
        rows = data.get(d)
        if not rows:
            ax.set_visible(False)
            continue
        p = float(rows[0]["p"])
        b = 1.0 / p
        for arm in ARMS:
            gaps = [int(r["h"]) - math.log(1 + int(r["f"])) / math.log(b)
                    for r in rows if r["policy"] == arm]
            if not gaps:
                continue
            gaps.sort()
            n = len(gaps)
            xs, ys = [], []
            for g in thresholds:
                k = n - bisect.bisect_left(gaps, g)
                if k == 0:
                    break
                xs.append(g)
                ys.append(k / n)
            draw(ax, (xs, ys, ys, ys), arm, band=False)
        gs = [g for g in thresholds if g >= 1]
        ax.plot(gs, [p ** (g - 1) for g in gs], color=REF_GREY, ls=":",
                lw=0.9, label=r"$p^{\,g-K}$, $K=1$" if d == GAP_DISTS[0] else None)
        ax.set_yscale("log")
        ax.set_ylim(1e-4 / 10, 1.5)
        recessive_grid(ax)
        ax.set_xlabel(r"$g$")
        ax.text(0.97, 0.95, DIST_LABEL[d], transform=ax.transAxes,
                ha="right", va="top", fontsize=7)
    axes[0].set_ylabel(r"items with $H_x-\log_b(1+f_x)\geq g$")
    fig.tight_layout(w_pad=0.6)
    legend_above(fig, axes, y=0.99)
    return fig


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    here = os.path.dirname(os.path.abspath(__file__))
    results = args[0] if args else os.path.join(here, "..", "results")
    results = os.path.normpath(results)
    out_dir = results

    # --arms=a,b,c restricts every figure to those arms (the paper's figures
    # leave the legacy rules out); --out=DIR writes the PDFs elsewhere.
    for opt in sys.argv[1:]:
        if opt.startswith("--arms="):
            chosen = [a for a in opt[len("--arms="):].split(",") if a]
            unknown = [a for a in chosen if a not in ARMS]
            if unknown:
                warn(f"unknown arm(s) {', '.join(unknown)}; known: {', '.join(ARMS)}")
            ARMS[:] = [a for a in ARMS if a in chosen]
            SPACE_ARMS[:] = [a for a in SPACE_ARMS if a in chosen]
        elif opt.startswith("--out="):
            out_dir = os.path.normpath(opt[len("--out="):])
            os.makedirs(out_dir, exist_ok=True)

    if "--tables" in sys.argv[1:]:
        print_tables(results)
        return

    plt = setup_matplotlib()
    figures = [
        ("structural_cost.pdf", lambda: plot_cost(plt, results)),
        ("structural_kl.pdf", lambda: plot_kl(plt, results)),
        ("structural_depth.pdf", lambda: plot_depth(plt, results)),
        ("structural_space.pdf", lambda: plot_space(plt, results)),
        ("structural_gap.pdf", lambda: plot_gap(plt, results)),
    ]
    for name, make in figures:
        fig = make()
        if fig is None:
            continue
        out = os.path.join(out_dir, name)
        fig.savefig(out)
        plt.close(fig)
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
