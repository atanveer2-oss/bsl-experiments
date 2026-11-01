#!/usr/bin/env python3
"""Figures for the reseeding part of the experiments section.

Reads the CSVs that build/reseed writes into results/ and writes one PDF per
figure into the same directory:

  reseed_barrier_tail.pdf  P[x-block cost/m >= t] against t on the
                           persistence-barrier sequence (K = 1), persistent,
                           reseeded and clocked ceiling c1 = 2; one panel per
                           n (100 above, 1000 below). The R = 2000 files are
                           used where they exist.
  reseed_phases.pdf        cost per access against the number of phases T
                           (log x), persistent and reseeded, with Ent/F and
                           local Ent/F as dashed references (n = 1024 by
                           default).
  reseed_stationary.pdf    cost/Ent on uniform, Zipf 1.0 and Zipf 1.5 as
                           grouped bars (mean over runs, sd whiskers):
                           persistent, reseeded and, if results/
                           reseed_shadow.csv exists, the shadow rebuild.

Figures are 3.3 in wide (one column), carry no titles (captions belong in
the paper), and use Computer Modern to match the article class. If a CSV or
a column a figure needs is missing, the script says so and skips that
figure or series; it never fills in data.

Usage: plot_reseed.py [results_dir] [--phases-n=1024]
"""
import bisect
import csv
import math
import os
import statistics as st
import sys

WIDTH = 3.3  # inches, single column

# Categorical slots 1-3 of the dataviz reference palette (validated all-pairs
# for colour-vision deficiency); markers and line styles carry identity too,
# so the figures survive greyscale printing.
BLUE, ORANGE, AQUA = "#2a78d6", "#eb6834", "#1baf7a"
REF_GREY = "0.35"

ARM_STYLE = {
    "persistent": dict(color=BLUE, marker="o", ls="-"),
    "reseeded": dict(color=ORANGE, marker="s", ls="--"),
    "ceiling": dict(color=AQUA, marker="^", ls="-."),
    "shadow": dict(color=AQUA, marker="^", ls="-."),
}


def warn(msg):
    print(f"plot_reseed: {msg}", file=sys.stderr)


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


def setup_matplotlib():
    import logging
    import matplotlib
    # fontTools complains about cmr10's header timestamps when embedding.
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
        # cmr10 has no Unicode minus; mathtext tick labels avoid the glyph.
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
        "lines.linewidth": 1.0,
        "lines.markersize": 3.5,
        "hatch.linewidth": 0.6,  # hatch takes the (white) bar edge colour
        "pdf.fonttype": 42,  # embed TrueType, no Type 3 fonts
        "savefig.bbox": "tight",
        "savefig.pad_inches": 0.02,
    })
    return plt


def recessive_grid(ax, axis="y"):
    ax.grid(axis=axis, which="major", color="0.88", lw=0.5)
    ax.set_axisbelow(True)


# ---------------------------------------------------------------------------
# (1) Barrier tail
# ---------------------------------------------------------------------------

def barrier_series(results):
    """{n: {arm: (values, source, R)}} for persistent, reseeded, ceiling2."""
    need = ["arm", "n", "x_block_over_m"]
    # Preference order per arm: R = 2000 files first.
    sources = {
        "persistent": ("gated1", ["reseed_barrier_R2000.csv",
                                  "reseed_barrier.csv"]),
        "reseeded": ("gated1+reseed", ["reseed_barrier_R2000.csv",
                                       "reseed_barrier.csv"]),
        "ceiling": ("gated1+ceiling2", ["reseed_barrier_ceiling_n100.csv",
                                        "reseed_barrier_ceiling_n1000.csv",
                                        "reseed_barrier_topup_n100.csv",
                                        "reseed_barrier_topup_n1000.csv"]),
    }
    cache = {}
    out = {}
    for n in (100, 1000):
        out[n] = {}
        for key, (arm, files) in sources.items():
            best = None
            for name in files:
                if name not in cache:
                    cache[name] = read_csv(os.path.join(results, name), need)
                rows = cache[name]
                if rows is None:
                    continue
                vals = [float(r["x_block_over_m"]) for r in rows
                        if r["arm"] == arm and int(r["n"]) == n]
                if vals and (best is None or len(vals) > len(best[0])):
                    best = (vals, name, len(vals))
            if best is None:
                warn(f"barrier: no rows for arm {arm} at n = {n}")
            else:
                out[n][key] = best
    return out


def plot_barrier(plt, results):
    data = barrier_series(results)
    if not any(data[n] for n in data):
        warn("barrier tail: no data, figure skipped")
        return None
    label = {"persistent": "persistent", "reseeded": "reseeded",
             "ceiling": r"ceiling, $c_1=2$"}
    # --arms=persistent,reseeded leaves the ceiling arm out (the submission
    # does not define it); --layout=row puts the two n side by side.
    arms = OPTS.get("arms", "persistent,reseeded,ceiling").split(",")
    # --ns=1000 draws one panel (the submission shows n = 1000 only and
    # states in the text that n = 100 gives the same curve).
    ns = [int(v) for v in OPTS.get("ns", "100,1000").split(",")]
    if OPTS.get("layout") == "row":
        fig, axes = plt.subplots(1, len(ns), figsize=(2.75 * len(ns), 1.9),
                                 sharey=True, squeeze=False)
        axes = list(axes[0])
    else:
        fig, axes = plt.subplots(len(ns), 1, figsize=(WIDTH, 1.8 * len(ns)),
                                 sharex=True, squeeze=False)
        axes = list(axes[:, 0])
    tmax = 0.0
    for ax, n in zip(axes, ns):
        floor = 1.0
        for key in arms:
            if key not in data[n]:
                continue
            vals, src, R = data[n][key]
            tmax = max(tmax, max(vals))
            floor = min(floor, 1.0 / R)
            # Empirical P[V >= t] on a fine grid, up to the largest observed
            # value (beyond it P = 0, which a log axis cannot show).
            xs = sorted(vals)
            ts = [i * 0.005 for i in range(int(xs[-1] / 0.005) + 1)]
            ts.append(xs[-1])
            ps = [(R - bisect.bisect_left(xs, t)) / R for t in ts]
            style = ARM_STYLE[key]
            ax.plot(ts, ps, color=style["color"], ls=style["ls"], lw=1.0,
                    label=label[key])
            print(f"  barrier n={n} {key}: {src}, R={R}")
        ax.set_yscale("log")
        ax.set_ylim(floor / 2, 1.5)
        recessive_grid(ax)
        ax.text(0.98, 0.95, f"$n={n}$", transform=ax.transAxes,
                ha="right", va="top")
    for ax in (axes if OPTS.get("layout") == "row" else axes[-1:]):
        ax.set_xlabel("$u$" if OPTS.get("layout") == "row" else "$t$")
        ax.set_xlim(0, math.ceil(tmax) + 0.5)
    fig.supylabel(r"$\mathrm{P}[\,$x-block cost$/m \geq u\,]$" if OPTS.get("layout") == "row"
                  else r"$\mathrm{P}[\,$x-block cost$/m \geq t\,]$",
                  fontsize=8, x=0.02)
    fig.tight_layout(h_pad=0.6)
    # One legend for both panels, above them: no corner of either panel is
    # clear of all three curves. The runs per curve (R) go in the caption;
    # the script prints them.
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=3,
               bbox_to_anchor=(0.55 if len(axes) > 1 else 0.6, 0.985),
               handlelength=2.2, columnspacing=1.2)
    return fig


# ---------------------------------------------------------------------------
# (2) Phases
# ---------------------------------------------------------------------------

def plot_phases(plt, results, phases_n):
    path = os.path.join(results, "reseed_phases.csv")
    rows = read_csv(path, ["arm", "n", "T", "F", "cost_over_F",
                           "ent_global", "ent_local"])
    if rows is None:
        warn("phases: figure skipped")
        return None
    rows = [r for r in rows if int(r["n"]) == phases_n]
    if not rows:
        warn(f"phases: no rows with n = {phases_n}, figure skipped")
        return None
    Ts = sorted({int(r["T"]) for r in rows})
    fig, ax = plt.subplots(figsize=(WIDTH, 2.3))
    arms = [("gated", "persistent"), ("gated+reseed", "reseeded")]
    for arm, key in arms:
        xs, ys, es = [], [], []
        for T in Ts:
            v = [float(r["cost_over_F"]) for r in rows
                 if r["arm"] == arm and int(r["T"]) == T]
            if not v:
                continue
            xs.append(T)
            ys.append(st.mean(v))
            es.append(st.stdev(v) if len(v) > 1 else 0.0)
        if not xs:
            warn(f"phases: no rows for arm {arm}")
            continue
        s = ARM_STYLE[key]
        ax.errorbar(xs, ys, yerr=es, color=s["color"], marker=s["marker"],
                    ls=s["ls"], capsize=1.5, elinewidth=0.6, label=key)
    # The benchmarks depend only on the sequence, so every run has the same
    # value; take the first row per T.
    ent_g, ent_l = [], []
    for T in Ts:
        r = next(r for r in rows if int(r["T"]) == T)
        F = float(r["F"])
        ent_g.append(float(r["ent_global"]) / F)
        ent_l.append(float(r["ent_local"]) / F)
    ax.plot(Ts, ent_g, color=REF_GREY, ls=(0, (5, 2)), lw=0.8,
            label=r"$\mathrm{Ent}/F$")
    ax.plot(Ts, ent_l, color=REF_GREY, ls=(0, (2, 2)), lw=0.8,
            label=r"local $\mathrm{Ent}/F$")
    ax.set_xscale("log", base=2)
    ax.set_xticks(Ts)
    ax.set_xlabel(f"number of phases $T$ ($n={phases_n}$)")
    ax.set_ylabel("cost per access")
    ax.set_ylim(bottom=0)
    recessive_grid(ax)
    handles, labels = ax.get_legend_handles_labels()
    order = sorted(range(len(labels)), key=lambda i: "Ent" in labels[i])
    ax.legend([handles[i] for i in order], [labels[i] for i in order],
              loc="upper left", ncol=2)
    print(f"  phases: {path}, n={phases_n}, T={Ts}")
    fig.tight_layout()
    return fig


# ---------------------------------------------------------------------------
# (3) Stationary profiles
# ---------------------------------------------------------------------------

PROFILES = [("uniform", "uniform"), ("zipf1.0", "Zipf 1.0"),
            ("zipf1.5", "Zipf 1.5")]


def stationary_data(results):
    """{(profile, key): [cost_over_ent per run]}."""
    need = ["profile", "arm", "cost_over_ent"]
    data = {}
    shadow = read_csv(os.path.join(results, "reseed_shadow.csv"), need)
    if shadow is not None:
        names = {"gated": "persistent", "gated+reseed": "reseeded",
                 "gated+shadow": "shadow"}
        for r in shadow:
            if r["arm"] in names:
                data.setdefault((r["profile"], names[r["arm"]]), []).append(
                    float(r["cost_over_ent"]))
    else:
        warn("stationary: no shadow arm (run build/reseed --only=shadow)")
    for prof, _ in PROFILES:
        if (prof, "persistent") in data:
            continue
        rows = read_csv(os.path.join(results, f"reseed_{prof}.csv"), need)
        if rows is None:
            continue
        for r in rows:
            key = {"gated": "persistent", "gated+reseed": "reseeded"}.get(
                r["arm"])
            if key:
                data.setdefault((prof, key), []).append(
                    float(r["cost_over_ent"]))
        print(f"  stationary {prof}: reseed_{prof}.csv")
    if shadow is not None:
        print("  stationary: reseed_shadow.csv")
    return data


def plot_stationary(plt, results):
    data = stationary_data(results)
    keys = [k for k in ("persistent", "reseeded", "shadow")
            if any((p, k) in data for p, _ in PROFILES)]
    if not keys:
        warn("stationary: no data, figure skipped")
        return None
    label = {"persistent": "persistent", "reseeded": "reseeded (eager)",
             "shadow": "reseeded (shadow)"}
    hatch = {"persistent": None, "reseeded": None, "shadow": "////"}
    fig, ax = plt.subplots(figsize=(WIDTH, 2.2))
    width = 0.8 / len(keys)
    for i, key in enumerate(keys):
        xs, ys, es = [], [], []
        for j, (prof, _) in enumerate(PROFILES):
            v = data.get((prof, key))
            if not v:
                warn(f"stationary: no {key} rows for {prof}")
                continue
            xs.append(j + (i - (len(keys) - 1) / 2) * width)
            ys.append(st.mean(v))
            es.append(st.stdev(v) if len(v) > 1 else 0.0)
        color = ARM_STYLE[key]["color"]
        ax.bar(xs, ys, width=width, color=color, edgecolor="white",
               linewidth=1.0, hatch=hatch[key], label=label[key], zorder=2)
        ax.errorbar(xs, ys, yerr=es, fmt="none", ecolor="0.15",
                    elinewidth=0.6, capsize=1.5, zorder=3)
    ax.set_xticks(range(len(PROFILES)))
    ax.set_xticklabels([name for _, name in PROFILES])
    ax.set_ylabel(r"cost$\,/\,\mathrm{Ent}$")
    ax.set_ylim(0, None)
    ax.tick_params(axis="x", length=0)
    recessive_grid(ax)
    ax.legend(loc="upper left", ncol=1)
    fig.tight_layout()
    return fig


OPTS = {}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:]
                if a.startswith("--") and "=" in a)
    OPTS.update(opts)
    here = os.path.dirname(os.path.abspath(__file__))
    results = args[0] if args else os.path.join(here, "..", "results")
    results = os.path.normpath(results)
    phases_n = int(opts.get("phases-n", 1024))

    plt = setup_matplotlib()
    figures = [
        ("reseed_barrier_tail.pdf", lambda: plot_barrier(plt, results)),
        ("reseed_phases.pdf", lambda: plot_phases(plt, results, phases_n)),
        ("reseed_stationary.pdf", lambda: plot_stationary(plt, results)),
    ]
    if "only" in opts:  # --only=reseed_barrier_tail.pdf draws one figure
        figures = [f for f in figures if f[0] == opts["only"]]
    for name, make in figures:
        fig = make()
        if fig is None:
            continue
        out = opts.get("out") or os.path.join(results, name)
        fig.savefig(out)
        plt.close(fig)
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
