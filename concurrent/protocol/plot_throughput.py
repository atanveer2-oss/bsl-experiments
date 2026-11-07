#!/usr/bin/env python3
"""Figures for the throughput and scaling part of the experiments section.

Reads the CSVs that run_protocol.py writes (throughput_*.csv and
coldstart_*.csv, searched recursively under each directory argument, so
concurrent/results/ with one subdirectory per node works as is) and writes
PDFs into the first directory argument, or --out:

  throughput_<mix>_k<keys>.pdf  throughput (Mops/s) against threads, one panel
                                per distribution, structures as series; mean
                                over repetitions with sd whiskers.
  pathlength_<mix>_k<keys>.pdf  mean search path length against threads, same
                                layout. Structures whose driver reports no
                                path length (bronson_pext_bst_occ returns 0)
                                are left out with a message.
  coldstart_<mix>_k<keys>_t<T>.pdf
                                throughput in each window against time since
                                the prefill (window midpoint, log scale), one
                                panel per distribution.

Figures are 3.3 in wide (one column), carry no titles (captions belong in
the paper), and use Computer Modern to match the article class. If a CSV, a
column, or a cell is missing, the script says so and skips it; it never fills
in data. Points are drawn only where rows exist.

Usage: plot_throughput.py [results_dir_or_csv ...] [--out DIR] [--skip a,b]
(--skip defaults to the Bronson AVL tree; several directories pool, so
results results-p50 draws both p settings as series)
"""
import argparse
import csv
import glob
import math
import os
import statistics as st
import sys

WIDTH = 3.3  # inches, single column

# Categorical slots of the dataviz reference palette, in a fixed order that
# the palette validator passes (adjacent pairs separated for colour-vision
# deficiency; the one pair in the 6 to 8 band, magenta and aqua, is also
# separated by marker and dash). Colour follows the structure, never its
# rank: every structure owns one hue. The classic control is the neutral
# dark grey, dashed: it is the same code as the gated rule with the rule
# switched off, and the grey reads as "baseline". Markers and line styles
# carry identity too, so the figures survive greyscale.
BLUE, ORANGE, AQUA, YELLOW = "#2a78d6", "#eb6834", "#1baf7a", "#eda100"
MAGENTA, GREEN, VIOLET, RED = "#e87ba4", "#008300", "#6250d6", "#e34948"
INK, INK2 = "#3b4048", "#8a8f96"

STYLE = {
    "bsl_gated": dict(color=BLUE, marker="o", ls="-", label="Gated rule (ours)"),
    "bsl_morris": dict(color=VIOLET, marker="D", ls="-", label="Morris estimator (ours)"),
    "bsl_exact": dict(color=MAGENTA, marker="s", ls="--", label="Exact estimator (ours)"),
    "bsl_classic": dict(color=INK, marker="h", ls=(0, (4, 1.5)), label="Classic, same code (control)"),
    "flexlist_locks": dict(color=ORANGE, marker="^", ls="-", label="Splay-list"),
    "cbtree-concurrent": dict(color=AQUA, marker="v", ls="-", label="CBTree"),
    "cbtree-concurrent-fair": dict(color=YELLOW, marker="<", ls="--", label="CBTree, fair"),
    "skiplist_locks": dict(color=GREEN, marker="+", ls=(0, (1, 1.2)), label="Skip list, locks (fork)"),
    "skiplist_lazy": dict(color=INK2, marker="x", ls=":", label="Lazy skip list (fork)"),
    "bronson_pext_bst_occ": dict(color=RED, marker="P", ls="-.", label="Bronson AVL"),
}
SPARE = dict(color=YELLOW, marker="*", ls="-")
ORDER = list(STYLE)

DIST_ORDER = ["uniform", "90-10", "95-5", "99-1", "zipf1"]


def warn(msg):
    print(f"plot_throughput: {msg}", file=sys.stderr)


def dist_label(d):
    if d == "uniform":
        return "uniform"
    if d.startswith("zipf"):
        return f"Zipf {d[4:]}"
    return d.replace("-", "/")


def find_csvs(paths, stem):
    out = []
    for p in paths:
        if os.path.isdir(p):
            out += sorted(glob.glob(os.path.join(p, "**", f"{stem}_*.csv"), recursive=True))
        elif os.path.basename(p).startswith(stem + "_"):
            out.append(p)
    return out


def read_rows(files, needed):
    rows = []
    for path in files:
        with open(path, newline="") as fh:
            reader = csv.DictReader(fh)
            missing = [c for c in needed if c not in (reader.fieldnames or [])]
            if missing:
                warn(f"{path} lacks column(s) {', '.join(missing)}; skipped")
                continue
            rows += list(reader)
    return rows


def setup_matplotlib():
    import logging
    import matplotlib
    logging.getLogger("fontTools").setLevel(logging.ERROR)
    matplotlib.use("Agg")
    from matplotlib import font_manager
    import matplotlib.pyplot as plt

    available = {f.name for f in font_manager.fontManager.ttflist}
    serif = [f for f in ("cmr10", "STIX Two Text", "Times", "DejaVu Serif")
             if f in available]
    plt.rcParams.update({
        "font.family": "serif",
        "font.serif": serif,
        "mathtext.fontset": "cm",
        "axes.formatter.use_mathtext": True,
        "axes.unicode_minus": False,
        "font.size": 8,
        "axes.labelsize": 8,
        "xtick.labelsize": 7,
        "ytick.labelsize": 7,
        "legend.fontsize": 7,
        "legend.frameon": False,
        "axes.linewidth": 0.6,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "xtick.major.width": 0.6,
        "ytick.major.width": 0.6,
        "xtick.minor.width": 0.4,
        "ytick.minor.width": 0.4,
        "lines.linewidth": 1.3,
        "lines.markersize": 4.2,
        "pdf.fonttype": 42,
        "savefig.bbox": "tight",
        "savefig.pad_inches": 0.02,
    })
    return plt


def recessive_grid(ax, axis="y"):
    ax.grid(axis=axis, which="major", color="0.88", lw=0.5)
    ax.set_axisbelow(True)


def series_key(row):
    """A series is a structure plus its recorded parameters, so two (p, K)
    settings of our structure never pool."""
    s, p, k = row["structure"], row.get("p", ""), row.get("K", "")
    return (s, p, k)


def series_style(key, spare_used):
    s, p, k = key
    if s in STYLE:
        style = dict(STYLE[s])
    elif not spare_used:
        spare_used.append(s)
        style = dict(SPARE, label=s.replace("_", " "))
    elif s in spare_used:
        style = dict(SPARE, label=s.replace("_", " "))
    else:
        return None
    # p applies to every rule of ours; the jump bound K only to the gated rule
    # (the counter rules jump by 1 + G).
    if s == "bsl_gated" and p and k:
        style["label"] += f" ($p={p}$, $K={k}$)"
    elif s.startswith("bsl_") and p:
        style["label"] += f" ($p={p}$)"
    return style


def sort_series(keys):
    return sorted(keys, key=lambda k: (ORDER.index(k[0]) if k[0] in ORDER else len(ORDER), k))


def check_machines(rows, what):
    machines = {r.get("cpu_model", "") for r in rows}
    if len(machines) > 1:
        warn(f"{what}: rows come from {len(machines)} CPU models {sorted(machines)}; "
             "they are pooled, which is only right if the nodes are identical")


def panel_grid(plt, n, height_per_row=1.25):
    ncols = 2 if n > 1 else 1
    nrows = math.ceil((n + 1) / ncols) if n > 1 else 1
    fig, axes = plt.subplots(nrows, ncols, figsize=(WIDTH, height_per_row * nrows + 0.2),
                             squeeze=False)
    flat = [ax for row in axes for ax in row]
    return fig, flat


def finish_legend(fig, axes_used, spare_axes):
    handles, labels = [], []
    for ax in axes_used:
        for h, l in zip(*ax.get_legend_handles_labels()):
            if l not in labels:
                handles.append(h)
                labels.append(l)
    for ax in spare_axes:
        ax.axis("off")
    if spare_axes:
        spare_axes[0].legend(handles, labels, loc="center", ncol=1, handlelength=2.2)
    else:
        fig.legend(handles, labels, loc="lower center", ncol=3,
                   bbox_to_anchor=(0.5, 1.0), handlelength=2.2)


def agg(values):
    vals = [v for v in values if v is not None and not math.isnan(v)]
    if not vals:
        return None
    return st.fmean(vals), (st.stdev(vals) if len(vals) > 1 else 0.0), len(vals)


def to_float(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


# ---------------------------------------------------------------------------
# Against threads
# ---------------------------------------------------------------------------

def against_threads(plt, rows, out_dir, metric):
    """metric: 'throughput' or 'pathlength'."""
    col = "throughput_ops_s" if metric == "throughput" else "path_length"
    scale = 1e-6 if metric == "throughput" else 1.0
    ylabel = "throughput (Mops/s)" if metric == "throughput" else "search path length"
    written = []
    for (mix, kr) in sorted({(r["mix"], r["key_range"]) for r in rows}):
        sub = [r for r in rows if r["mix"] == mix and r["key_range"] == kr]
        check_machines(sub, f"{metric} {mix} k={kr}")
        dists = [d for d in DIST_ORDER if any(r["distribution"] == d for r in sub)]
        dists += sorted({r["distribution"] for r in sub} - set(dists))
        series = sort_series({series_key(r) for r in sub})
        if metric == "pathlength":
            keep = []
            for k in series:
                vals = [to_float(r[col]) for r in sub if series_key(r) == k]
                if all(v in (None, 0.0) for v in vals):
                    warn(f"{k[0]}: no path length recorded (driver reports 0); left out of "
                         f"pathlength_{mix}_k{kr}.pdf")
                else:
                    keep.append(k)
            series = keep
        if not series:
            warn(f"{metric} {mix} k={kr}: nothing to plot")
            continue
        fig, axes = panel_grid(plt, len(dists))
        spare_used = []
        all_threads = sorted({int(r["threads"]) for r in sub})
        for ax, dist in zip(axes, dists):
            drows = [r for r in sub if r["distribution"] == dist]
            for key in series:
                style = series_style(key, spare_used)
                if style is None:
                    warn(f"{key[0]}: no palette slot left; series skipped")
                    continue
                pts = []
                for t in all_threads:
                    a = agg([to_float(r[col]) for r in drows
                             if series_key(r) == key and int(r["threads"]) == t])
                    if a is not None:
                        pts.append((t, a[0] * scale, a[1] * scale))
                if not pts:
                    warn(f"{metric} {mix} k={kr} {dist}: no rows for {key[0]}")
                    continue
                xs, ys, es = zip(*pts)
                label = style.pop("label")
                ax.errorbar(xs, ys, yerr=es, label=label, capsize=1.2, elinewidth=0.5,
                            markerfacecolor="none" if style["marker"] not in "x+" else None,
                            **style)
            ax.set_xscale("log", base=2)
            # Label every thread count when there are few, else a spread of
            # five that always includes the first and the last.
            if len(all_threads) <= 6:
                ticks = all_threads
            else:
                ticks = sorted({all_threads[0], all_threads[-1]}
                               | {t for t in all_threads if t in (8, 32, 128)})
            ax.set_xticks(ticks)
            ax.get_xaxis().set_major_formatter(plt.matplotlib.ticker.ScalarFormatter())
            ax.minorticks_off()
            ax.set_ylim(bottom=0)
            recessive_grid(ax)
            ax.set_title(dist_label(dist), loc="left", fontsize=7, pad=2)  # panel label
        used = axes[:len(dists)]
        for ax in used[-2:]:
            ax.set_xlabel("threads")
        fig.supylabel(ylabel, fontsize=8, x=0.0)
        finish_legend(fig, used, axes[len(dists):])
        fig.tight_layout(pad=0.3, h_pad=0.6, w_pad=0.6)
        out = os.path.join(out_dir, f"{metric}_{mix}_k{kr}.pdf")
        fig.savefig(out)
        plt.close(fig)
        written.append(out)
    return written


# ---------------------------------------------------------------------------
# Cold start
# ---------------------------------------------------------------------------

def cold_start(plt, rows, out_dir):
    written = []
    for (mix, kr, t) in sorted({(r["mix"], r["key_range"], int(r["threads"])) for r in rows}):
        sub = [r for r in rows if r["mix"] == mix and r["key_range"] == kr
               and int(r["threads"]) == t]
        check_machines(sub, f"cold start {mix} k={kr} t={t}")
        dists = [d for d in DIST_ORDER if any(r["distribution"] == d for r in sub)]
        dists += sorted({r["distribution"] for r in sub} - set(dists))
        series = sort_series({series_key(r) for r in sub})
        windows = sorted({(float(r["window_start_s"]), float(r["window_end_s"])) for r in sub})
        fig, axes = panel_grid(plt, len(dists))
        spare_used = []
        for ax, dist in zip(axes, dists):
            drows = [r for r in sub if r["distribution"] == dist]
            for key in series:
                style = series_style(key, spare_used)
                if style is None:
                    warn(f"{key[0]}: no palette slot left; series skipped")
                    continue
                pts = []
                for (a, b) in windows:
                    s = agg([to_float(r["throughput_ops_s"]) for r in drows
                             if series_key(r) == key and float(r["window_start_s"]) == a
                             and float(r["window_end_s"]) == b])
                    if s is not None:
                        pts.append(((a + b) / 2, s[0] * 1e-6, s[1] * 1e-6))
                if not pts:
                    warn(f"cold start {mix} k={kr} t={t} {dist}: no rows for {key[0]}")
                    continue
                xs, ys, es = zip(*pts)
                label = style.pop("label")
                ax.errorbar(xs, ys, yerr=es, label=label, capsize=1.2, elinewidth=0.5,
                            markerfacecolor="none" if style["marker"] not in "x+" else None,
                            **style)
            ax.set_xscale("log")
            ax.get_xaxis().set_major_formatter(plt.matplotlib.ticker.ScalarFormatter())
            ax.set_ylim(bottom=0)
            recessive_grid(ax)
            ax.set_title(dist_label(dist), loc="left", fontsize=7, pad=2)  # panel label
        used = axes[:len(dists)]
        for ax in used[-2:]:
            ax.set_xlabel("time since prefill (s)")
        fig.supylabel("throughput (Mops/s)", fontsize=8, x=0.0)
        finish_legend(fig, used, axes[len(dists):])
        fig.tight_layout(pad=0.3, h_pad=0.6, w_pad=0.6)
        out = os.path.join(out_dir, f"coldstart_{mix}_k{kr}_t{t}.pdf")
        fig.savefig(out)
        plt.close(fig)
        written.append(out)
    return written


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    default = os.path.join(os.path.dirname(here), "results")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="*", default=[default],
                    help="result directories or CSV files (default concurrent/results)")
    ap.add_argument("--out", help="output directory (default: the first directory argument)")
    ap.add_argument("--skip", default="bronson_pext_bst_occ",
                    help="comma-separated structures to leave out of every figure "
                         "(default: the Bronson AVL tree, which the paper's figure "
                         "does not show; pass '' to keep everything)")
    args = ap.parse_args(argv)
    skip = {s for s in args.skip.split(",") if s}
    out_dir = args.out or next((p for p in args.paths if os.path.isdir(p)),
                               os.path.dirname(args.paths[0]) or ".")
    os.makedirs(out_dir, exist_ok=True)

    base = ["structure", "p", "K", "key_range", "distribution", "mix", "threads",
            "rep", "throughput_ops_s", "path_length", "cpu_model"]
    plt = setup_matplotlib()
    written = []

    tfiles = find_csvs(args.paths, "throughput")
    if not tfiles:
        warn("no throughput_*.csv found; throughput and path-length figures skipped")
    else:
        rows = [r for r in read_rows(tfiles, base) if r["structure"] not in skip]
        if rows:
            written += against_threads(plt, rows, out_dir, "throughput")
            written += against_threads(plt, rows, out_dir, "pathlength")

    cfiles = find_csvs(args.paths, "coldstart")
    if not cfiles:
        warn("no coldstart_*.csv found; cold-start figures skipped")
    else:
        rows = [r for r in read_rows(cfiles, base + ["window_start_s", "window_end_s"])
                if r["structure"] not in skip]
        if rows:
            written += cold_start(plt, rows, out_dir)

    for w in written:
        print(w)
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
