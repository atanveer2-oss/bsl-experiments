#!/usr/bin/env python3
"""Progress and preliminary numbers from collected Chameleon CSVs.

    python3 progress.py [results/chameleon] [--md] [--json]

Reads every throughput_*.csv and coldstart_*.csv under the directory (collect.sh
puts one node's files under results/chameleon/<node>/), and prints: rows done
per mix, repetitions completed per cell, how many planned rows remain for the
default protocol, the rate of the last hour, and a preliminary table of
median throughput and path length per structure at a few thread counts, for
each distribution at the primary key range and the read-only mix. Medians
over repetitions; the number of repetitions behind each cell is shown, so a
reader can tell a one-repetition number from a settled one.

Everything here is a progress check, not a result: the figures come from
plot_throughput.py on the complete CSVs.
"""
import argparse
import collections
import csv
import datetime as dt
import glob
import json
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_protocol as rp  # noqa: E402

SHOW_THREADS = [1, 32, 64, 128]


def load(paths, stem):
    rows = []
    for d in paths:
        files = [d] if os.path.isfile(d) else glob.glob(os.path.join(d, "**", f"{stem}_*.csv"), recursive=True)
        for f in files:
            with open(f, newline="") as fh:
                for r in csv.DictReader(fh):
                    r["_file"] = f
                    rows.append(r)
    return rows


def planned_total(rows_structures):
    """Rows the default protocol plans for the structures seen (same arithmetic as estimate.py)."""
    n_read = len(rows_structures)
    n_upd = len([s for s in rows_structures if s not in rp.NO_UPDATE_SUPPORT])
    cells = len(rp.DEFAULT_DISTRIBUTIONS) * len(rp.DEFAULT_KEY_RANGES) * len(rp.DEFAULT_THREADS)
    return (n_read + n_upd) * cells * 10


def fmt_mops(v):
    return f"{v / 1e6:.2f}"


def summarise(rows, cold):
    out = {}
    if not rows:
        return {"rows": 0}
    structures = sorted({r["structure"] for r in rows})
    out["rows"] = len(rows)
    out["structures"] = structures
    out["nodes"] = sorted({os.path.basename(os.path.dirname(r["_file"])) for r in rows})
    out["commit"] = sorted({r.get("commit", "") for r in rows})
    by_mix = collections.Counter(r["mix"] for r in rows)
    out["rows_by_mix"] = dict(by_mix)
    # repetitions per cell
    key = (lambda r: (r["structure"], r["mix"], r["distribution"], r["key_range"], r["threads"]))
    reps = collections.Counter(key(r) for r in rows)
    out["cells"] = len(reps)
    out["reps_min"] = min(reps.values())
    out["reps_max"] = max(reps.values())
    out["reps_hist"] = dict(sorted(collections.Counter(reps.values()).items()))
    if not cold:
        out["planned_rows"] = planned_total(structures)
    # rate
    times = sorted(dt.datetime.strptime(r["started_utc"], "%Y-%m-%dT%H:%M:%SZ") for r in rows if r.get("started_utc"))
    if times:
        out["first_utc"] = times[0].strftime("%Y-%m-%d %H:%M")
        out["last_utc"] = times[-1].strftime("%Y-%m-%d %H:%M")
        hour_ago = times[-1] - dt.timedelta(hours=1)
        out["rows_last_hour"] = sum(1 for t in times if t > hour_ago)
        if "planned_rows" in out and out["rows_last_hour"]:
            out["hours_left_at_rate"] = round((out["planned_rows"] - len(rows)) / out["rows_last_hour"], 1)
    # failures are not in the CSV (the runner records only successes); validation results are
    bad = [r for r in rows if r["structure"].startswith("bsl_") and "validate=OK" not in r.get("ds_stats", "")]
    out["bsl_rows_without_validate_ok"] = len(bad)
    # peak memory
    peaks = [float(r["peak_rss_mb"]) for r in rows if r.get("peak_rss_mb")]
    if peaks:
        out["peak_rss_gb_max"] = round(max(peaks) / 1024, 1)
        bym = collections.defaultdict(list)
        for r in rows:
            if r.get("peak_rss_mb"):
                bym[(r["structure"], r["mix"])].append(float(r["peak_rss_mb"]))
        out["peak_rss_gb_by_structure_mix"] = {f"{s} {m}": round(max(v) / 1024, 2) for (s, m), v in sorted(bym.items())}
    if cold:
        return out
    # preliminary table: read-only mix, primary key range, per distribution
    kr = str(rp.DEFAULT_KEY_RANGES[0])
    table = {}
    for dist in rp.DEFAULT_DISTRIBUTIONS:
        sel = [r for r in rows if r["mix"] == "100-0-0" and r["key_range"] == kr and r["distribution"] == dist]
        if not sel:
            continue
        t = {}
        for s in structures:
            cells = {}
            for th in SHOW_THREADS:
                v = [r for r in sel if r["structure"] == s and int(r["threads"]) == th]
                if not v:
                    continue
                thr = statistics.median(float(r["throughput_ops_s"]) for r in v)
                pl = [float(r["path_length"]) for r in v if r.get("path_length") not in ("", None)]
                cells[th] = {"mops": round(thr / 1e6, 2), "path": round(statistics.median(pl), 1) if pl else None, "n": len(v)}
                # Our adapter's exit line splits the path into levels descended and
                # forward steps (whole run, warmup included): report the vertical share.
                vs = []
                for r in v:
                    m = dict(re.findall(r"(path_total|path_levels)=(\d+)", r.get("ds_stats", "")))
                    if m.get("path_total") and int(m["path_total"]) > 0:
                        vs.append(int(m["path_levels"]) / int(m["path_total"]))
                if vs:
                    cells[th]["vertical"] = round(100 * statistics.median(vs))
            if cells:
                t[s] = cells
        if t:
            table[dist] = t
    out["preliminary"] = table
    return out


def runner_warnings(paths):
    """Counts of the runner's FAILED, MEMORY and PROTOCOL-FINISHED lines in every
    collected bsl-run.log (collect.sh copies each node's log beside its CSVs).
    Failed runs are not in the CSV, so this is the only place they show up."""
    out = {"failed": 0, "memory": 0, "finished_nodes": 0, "logs": 0, "failed_lines": []}
    for d in paths:
        files = [d] if os.path.isfile(d) else glob.glob(os.path.join(d, "**", "bsl-run.log"), recursive=True)
        for f in files:
            if not f.endswith("bsl-run.log"):
                continue
            out["logs"] += 1
            with open(f, errors="replace") as fh:
                for line in fh:
                    if "FAILED " in line:
                        out["failed"] += 1
                        if len(out["failed_lines"]) < 10:
                            out["failed_lines"].append(os.path.basename(os.path.dirname(f)) + ": " + line.strip()[:200])
                    elif "MEMORY:" in line:
                        out["memory"] += 1
                    elif line.startswith("PROTOCOL-FINISHED"):
                        out["finished_nodes"] += 1
    return out


def markdown(thr, cold, warn=None):
    lines = []
    if thr.get("rows", 0) == 0 and cold.get("rows", 0) == 0:
        return "No rows collected yet."
    lines.append(f"**Throughput sweep**: {thr.get('rows', 0)} rows"
                 + (f" of {thr['planned_rows']} the default protocol plans for these structures" if thr.get("planned_rows") else "")
                 + (f", {thr['cells']} cells with {thr['reps_min']} to {thr['reps_max']} repetitions" if thr.get("cells") else "")
                 + (f"; last hour {thr['rows_last_hour']} rows" if thr.get("rows_last_hour") is not None else "")
                 + (f", about {thr['hours_left_at_rate']} h left at that rate" if thr.get("hours_left_at_rate") is not None else "")
                 + ".")
    if thr.get("rows"):
        lines.append(f"Mixes: {thr['rows_by_mix']}. Nodes: {', '.join(thr['nodes'])}. Commit: {', '.join(thr['commit'])}."
                     + (f" Peak memory so far {thr['peak_rss_gb_max']} GB." if thr.get("peak_rss_gb_max") else "")
                     + (f" **{thr['bsl_rows_without_validate_ok']} bsl rows without validate=OK.**" if thr.get("bsl_rows_without_validate_ok") else " Every bsl row validated."))
    if warn and warn["logs"]:
        lines.append(f"Runner logs: {warn['logs']} node log(s), {warn['finished_nodes']} finished; "
                     + (f"**{warn['failed']} FAILED runs** (not in the CSV; a rerun retries them)" if warn["failed"] else "no failed runs")
                     + (f", **{warn['memory']} memory-guard skips**." if warn["memory"] else ", no memory-guard skips."))
        for fl in warn["failed_lines"]:
            lines.append(f"  - {fl}")
    if cold.get("rows"):
        lines.append(f"**Cold start**: {cold['rows']} rows, {cold.get('cells', 0)} cells, {cold.get('reps_min', 0)} to {cold.get('reps_max', 0)} repetitions.")
    for dist, t in thr.get("preliminary", {}).items():
        lines.append("")
        lines.append(f"Preliminary, {dist}, key range {rp.DEFAULT_KEY_RANGES[0]}, read-only: median Mops/s (path length, vertical share of it for ours) [repetitions]")
        head = "| structure | " + " | ".join(f"{th} thr" for th in SHOW_THREADS) + " |"
        lines.append(head)
        lines.append("|---|" + "---|" * len(SHOW_THREADS))
        for s, cells in t.items():
            row = [s]
            for th in SHOW_THREADS:
                c = cells.get(th)
                if c and "vertical" in c:
                    row.append(f"{c['mops']} ({c['path']}, {c['vertical']}% v) [{c['n']}]")
                else:
                    row.append(f"{c['mops']} ({c['path']}) [{c['n']}]" if c else "")
            lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="*", default=[os.path.join(HERE, "..", "results", "chameleon")])
    ap.add_argument("--json", action="store_true", help="machine-readable summary instead of markdown")
    args = ap.parse_args()
    thr = summarise(load(args.paths, "throughput"), cold=False)
    cold = summarise(load(args.paths, "coldstart"), cold=True)
    warn = runner_warnings(args.paths)
    if args.json:
        print(json.dumps({"throughput": thr, "cold": cold, "runner": warn}, indent=1, default=str))
    else:
        print(markdown(thr, cold, warn))


if __name__ == "__main__":
    main()
