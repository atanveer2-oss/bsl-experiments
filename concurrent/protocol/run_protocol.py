#!/usr/bin/env python3
"""Resumable runner for the SetBench throughput protocol (PROTOCOL.md).

Runs on the measurement node, after node_build.sh has built the binaries. Every
completed run appends one row to a CSV, so a lost SSH session costs at most
the run that was in flight: start the same command again and it skips every
row already present.

Two modes:

  default       throughput and path length against threads. One row per run,
                written to results/chameleon/throughput_<site>_<date>.csv.
  --cold-start  throughput as a function of elapsed time from a fresh prefill.
                The fork has no throughput-over-time output, so each point is
                its own process: window [a, b] runs `presecs a secs (b - a)`
                from a fresh prefill and reports the ops completed in that
                window. Rows go to results/chameleon/coldstart_<site>_<date>.csv.

How the benchmark is driven (all read from microbench/benchmark.cpp and
benchmark_write.cpp, the fork's own drivers, not from memory):

  * arguments are bare `name value` pairs: prefill, presecs, secs, threads,
    x, y, zipf, cops, and in the update driver write, writeset. An unknown
    name prints "Wrong arguments" and exits 0, so this runner validates the
    echoed parameters instead of trusting the exit code.
  * hot-set skew `x X y Y`: X percent of operations go to a hot set holding
    Y percent of the keys. Uniform is `x 100 y 100` (the drivers' defaults).
  * `zipf A`: Zipf(A) over ranks, rank r mapped to the r-th key drawn at
    prefill, so the ranking is over randomly drawn (shuffled) keys.
  * `cops C` sets the adjustment period of the splay-list and the CBTree.
  * output: "h: 0, ops: N, sumLengths: L"; throughput is N / secs (as in
    make_graph.py), path length is L, the mean per operation.
  * worker i is pinned to CPU i by the driver itself (pthread_setaffinity_np);
    memory is interleaved with `numactl --interleave=all` and jemalloc is
    preloaded, exactly as the authors' count_stats_zipf.sh does.

Standard library only. See chameleon/README.md for the full procedure.
"""
import argparse
import csv
import datetime as dt
import glob
import os
import platform
import re
import shlex
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EXPERIMENTS = os.path.dirname(HERE)

# Ours (experiments/setbench/ds/, copied into the build by setbench/build.sh),
# its control arm on the same code, and the six baselines (ds/ names).
# bsl_exact and bsl_morris join with --structures when they exist.
DEFAULT_STRUCTURES = [
    "bsl_gated",
    "bsl_classic",             # control: same code, heights fixed at insertion
    "flexlist_locks",          # splay-list (Aksenov et al.)
    "cbtree-concurrent",       # CBTree
    "cbtree-concurrent-fair",  # CBTree, fair variant
    "skiplist_lazy",
    "skiplist_locks",
    "bronson_pext_bst_occ",    # Bronson et al. concurrent AVL
]

# cbtree-concurrent{,-fair}/adapter.h: erase() calls setbench_error and
# insertIfAbsent asserts tid == 0, so neither can run the update mix.
NO_UPDATE_SUPPORT = {"cbtree-concurrent", "cbtree-concurrent-fair"}

# Adjustment period per structure, as in the splay-list paper's thread sweep
# (count_stats_zipf.sh, plotted by make_graph.py): Splay-list 1/(1000p),
# CBTree 1/1000, CBTree Fair 1/(1000p). `T` is the thread count. The driver
# always calls setCops with a default of 1, which for the splay-list means an
# adjustment on every operation, so these must be passed explicitly.
DEFAULT_COPS = {
    "flexlist_locks": "1000*T",
    "cbtree-concurrent": "1000",
    "cbtree-concurrent-fair": "1000*T",
}

DEFAULT_DISTRIBUTIONS = ["uniform", "90-10", "95-5", "99-1", "zipf1"]
DEFAULT_KEY_RANGES = [1000000, 100000]
DEFAULT_MIXES = ["100-0-0", "80-10-10"]  # lookup-insert-delete percentages
DEFAULT_THREADS = [1, 2, 4, 8, 16, 32, 48, 64, 96, 128]
SMT_THREADS = [256]
DEFAULT_WINDOWS = [0, 1, 2, 5, 10, 20, 60, 120]
DEFAULT_COLD_THREADS = [1, 64]

# Without harness.patch the drivers and the CBTree overflow at tid >= 100.
UNPATCHED_MAX_THREADS = 99

BIN_SUFFIX = ".alloc_new.reclaim_none.pool_none.out"

COLUMNS = [
    "site", "started_utc", "host", "machine", "cpu_model", "sockets",
    "cores", "hw_threads", "threads_per_core", "numa_nodes", "kernel",
    "governor", "pin_layout", "commit", "harness_patch", "build_xargs", "binary",
    "structure", "p", "K", "cops", "key_range", "distribution", "x", "y",
    "zipf_alpha", "mix", "write_frac", "writeset", "threads", "rep", "seed",
    "allocator", "numa_policy", "warmup_s", "duration_s",
    "ops", "throughput_ops_s", "path_length", "ds_stats", "wall_s", "peak_rss_mb",
    "log", "cmdline",
]
COLD_COLUMNS = COLUMNS[:COLUMNS.index("warmup_s")] + [
    "window_start_s", "window_end_s",
] + COLUMNS[COLUMNS.index("warmup_s"):]

KEY_FIELDS = ["structure", "p", "K", "cops", "key_range", "distribution",
              "mix", "writeset", "threads", "rep", "warmup_s", "duration_s"]
COLD_KEY_FIELDS = KEY_FIELDS + ["window_start_s", "window_end_s"]


def mem_total_gb():
    """RAM of this machine in GB from /proc/meminfo, or 0 when unreadable."""
    try:
        with open("/proc/meminfo") as fh:
            for line in fh:
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) / 1e6
    except OSError:
        pass
    return 0.0


def vm_hwm_mb(pid):
    """Peak resident set of a live process in MB (Linux: VmHWM in
    /proc/<pid>/status), or 0 when unreadable."""
    try:
        with open(f"/proc/{pid}/status") as fh:
            for line in fh:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1]) / 1024.0
    except OSError:
        pass
    return 0.0


def run_measured(cmd, timeout):
    """Run cmd, collecting its output, and sample its peak resident set every
    half second while it runs. Returns (text, returncode or "timeout",
    peak_mb). The peak is the largest VmHWM seen; the benchmark allocates
    during prefill and, under update mixes, through the whole run, so the
    last sample before exit is within half a second of the true peak."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    peak = 0.0
    deadline = time.time() + timeout
    while True:
        try:
            out, err = p.communicate(timeout=0.5)
            break
        except subprocess.TimeoutExpired:
            peak = max(peak, vm_hwm_mb(p.pid))
            if time.time() > deadline:
                p.kill()
                out, err = p.communicate()
                return (out or "") + (err or ""), "timeout", peak
    return (out or "") + (err or ""), p.returncode, peak


def warn(msg):
    print(f"run_protocol: {msg}", file=sys.stderr, flush=True)


def int_list(s):
    return [int(v) for v in s.split(",") if v.strip()]


def str_list(s):
    return [v.strip() for v in s.split(",") if v.strip()]


# ---------------------------------------------------------------------------
# Workload mapping
# ---------------------------------------------------------------------------

def distribution_args(name):
    """Benchmark tokens and metadata for a distribution name.

    uniform -> x 100 y 100; "X-Y" -> x X y Y; "zipfA" -> zipf A."""
    if name == "uniform":
        return ["x", "100", "y", "100"], dict(x="100", y="100", zipf_alpha="")
    m = re.fullmatch(r"(\d+)-(\d+)", name)
    if m:
        x, y = m.groups()
        return ["x", x, "y", y], dict(x=x, y=y, zipf_alpha="")
    m = re.fullmatch(r"zipf([0-9.]+)", name)
    if m:
        a = m.group(1)
        return ["zipf", a], dict(x="", y="", zipf_alpha=a)
    raise ValueError(f"unknown distribution {name!r} "
                     "(use uniform, X-Y such as 90-10, or zipfA such as zipf1)")


def mix_args(name, writeset):
    """(binary prefix, tokens, write fraction) for a lookup-insert-delete mix.

    100-0-0 runs benchmark.cpp (bench_*). Anything else runs
    benchmark_write.cpp (write_*), whose updates are split evenly between
    insertIfAbsent and erase, so insert and delete must be equal."""
    m = re.fullmatch(r"(\d+)-(\d+)-(\d+)", name)
    if not m:
        raise ValueError(f"unknown mix {name!r} (use L-I-D, e.g. 80-10-10)")
    lk, ins, de = (int(v) for v in m.groups())
    if lk + ins + de != 100 or ins != de:
        raise ValueError(f"mix {name!r}: percentages must sum to 100 with "
                         "insert == delete (the driver splits updates 50/50)")
    if ins == 0:
        return "bench", [], "0"
    frac = f"{(ins + de) / 100:g}"
    return "write", ["write", frac, "writeset", f"{writeset:g}"], frac


def cops_value(expr, threads):
    if expr is None or expr == "":
        return None
    if not re.fullmatch(r"[0-9T*+ ]+", expr):
        raise ValueError(f"cops expression {expr!r} may use digits, T, * and +")
    return int(eval(expr.replace("T", str(threads)), {"__builtins__": {}}))


# ---------------------------------------------------------------------------
# Build and machine provenance
# ---------------------------------------------------------------------------

def read_build_info(work):
    """BUILD_INFO written by provision.sh into the build directory."""
    info = {}
    path = os.path.join(work, "BUILD_INFO")
    if os.path.exists(path):
        with open(path) as fh:
            for line in fh:
                if "=" in line:
                    k, v = line.rstrip("\n").split("=", 1)
                    info[k.strip()] = v.strip()
    return info


def local_commit():
    try:
        sha = subprocess.run(["git", "-C", EXPERIMENTS, "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", EXPERIMENTS, "status", "--porcelain", "--", "."],
                               capture_output=True, text=True, check=True).stdout.strip()
        return sha + ("-dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def sysfs(path, default=""):
    try:
        with open(path) as fh:
            return fh.read().strip()
    except OSError:
        return default


def machine_info():
    info = dict(host=socket.gethostname(), kernel=" ".join(platform.uname()[i] for i in (0, 2, 4)),
                cpu_model="", sockets="", cores="", hw_threads="",
                threads_per_core="", numa_nodes="",
                governor=sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
                pin_layout="")
    try:
        out = subprocess.run(["lscpu"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        out = ""
    fields = {}
    for line in out.splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            fields[k.strip()] = v.strip()
    info["cpu_model"] = fields.get("Model name", "")
    info["sockets"] = fields.get("Socket(s)", "")
    info["hw_threads"] = fields.get("CPU(s)", "")
    info["threads_per_core"] = fields.get("Thread(s) per core", "")
    info["numa_nodes"] = fields.get("NUMA node(s)", "")
    try:
        info["cores"] = str(int(fields["Core(s) per socket"]) * int(fields["Socket(s)"]))
    except (KeyError, ValueError):
        pass
    info["pin_layout"] = pin_layout(info)
    info["machine"] = (f"{info['cpu_model'] or platform.machine()} x{info['sockets'] or '?'}, "
                       f"{info['cores'] or '?'}C/{info['hw_threads'] or '?'}T, {info['kernel']}")
    return info


def pin_layout(info):
    """The driver pins worker i to CPU i. Report whether CPUs 0..cores-1 are
    distinct physical cores (so every count up to the core count avoids SMT
    siblings) and in which socket order they come."""
    try:
        cores = int(info["cores"])
    except (KeyError, ValueError):
        return ""
    seen, pkgs = set(), []
    for cpu in range(cores):
        base = f"/sys/devices/system/cpu/cpu{cpu}/topology/"
        pkg, core = sysfs(base + "physical_package_id"), sysfs(base + "core_id")
        if not pkg:
            return ""
        seen.add((pkg, core))
        if not pkgs or pkgs[-1] != pkg:
            pkgs.append(pkg)
    first = "cores-first" if len(seen) == cores else "siblings-interleaved"
    return f"{first};sockets={'>'.join(pkgs)}"


# ---------------------------------------------------------------------------
# The plan
# ---------------------------------------------------------------------------

class Run:
    __slots__ = ("structure", "p", "K", "cops", "key_range", "distribution",
                 "dist_meta", "mix", "write_frac", "writeset", "threads",
                 "rep", "seed", "warmup", "duration", "window", "binary",
                 "tokens", "seconds")

    def key(self, cold):
        d = self.row_fields()
        return tuple(str(d[f]) for f in (COLD_KEY_FIELDS if cold else KEY_FIELDS))

    def row_fields(self):
        d = dict(structure=self.structure, p=self.p, K=self.K,
                 cops="" if self.cops is None else self.cops,
                 key_range=self.key_range, distribution=self.distribution,
                 mix=self.mix, write_frac=self.write_frac,
                 writeset=self.writeset if self.write_frac != "0" else "",
                 threads=self.threads, rep=self.rep, seed=self.seed,
                 warmup_s=self.warmup, duration_s=self.duration,
                 binary=self.binary)
        d.update(self.dist_meta)
        if self.window is not None:
            d["window_start_s"], d["window_end_s"] = self.window
        return d


def parse_kv_map(items, what):
    """name:payload items -> {name: payload}."""
    out = {}
    for item in items or []:
        if ":" not in item:
            raise SystemExit(f"run_protocol: --{what} expects NAME:VALUE, got {item!r}")
        name, payload = item.split(":", 1)
        out[name] = payload
    return out


def bsl_params(work, xargs, structure):
    """(p, K) a bsl_* binary was built with: the -DBSL_P / -DBSL_K in the
    build's XARGS, else the defaults in bsl_skiplist.h. Our variants share
    ds/bsl_gated's header. ("", "") for anything else."""
    if not structure.startswith("bsl_"):
        return "", ""
    vals = {}
    for cand in (os.path.join(work, "structs_bench", "ds", structure, "bsl_skiplist.h"),
                 os.path.join(work, "structs_bench", "ds", "bsl_gated", "bsl_skiplist.h"),
                 os.path.join(EXPERIMENTS, "setbench", "ds", structure, "bsl_skiplist.h"),
                 os.path.join(EXPERIMENTS, "setbench", "ds", "bsl_gated", "bsl_skiplist.h")):
        if os.path.exists(cand):
            with open(cand) as fh:
                for m in re.finditer(r"#define\s+BSL_(P|K)\s+(\S+)", fh.read()):
                    vals.setdefault(m.group(1), m.group(2))
            break
    for m in re.finditer(r"-DBSL_(P|K)=(\S+)", xargs or ""):
        vals[m.group(1)] = m.group(2)
    return vals.get("P", ""), vals.get("K", "")


def build_plan(args, max_threads, seeded, struct_defaults=None):
    """Every run, in execution order. Repetition is the outermost loop, so a
    campaign cut short still covers every cell with fewer repetitions, and
    structure is the innermost, so structures are compared under the closest
    conditions in time."""
    cold = args.cold_start
    structures = args.structures
    dists = args.cold_distributions if cold else args.distributions
    key_ranges = args.cold_key_ranges if cold else args.key_ranges
    mixes = args.cold_mixes if cold else args.mixes
    threads = list(args.cold_threads if cold else args.threads)
    if args.smt and not cold:
        threads += [t for t in SMT_THREADS if t not in threads]
    dropped = [t for t in threads if t > max_threads]
    if dropped:
        warn(f"dropping thread counts {dropped}: this build supports at most "
             f"{max_threads} (apply chameleon/harness.patch, see README)")
    threads = [t for t in threads if t <= max_threads]
    windows = list(zip(args.windows, args.windows[1:])) if cold else [None]

    params = parse_kv_map(args.struct_param, "struct-param")
    extra = parse_kv_map(args.struct_args, "struct-args")
    cops_map = dict(DEFAULT_COPS)
    cops_map.update(parse_kv_map(args.cops, "cops"))
    no_update = set(NO_UPDATE_SUPPORT) | set(args.no_update or [])

    meta = {}
    for s in structures:
        kv = {}
        for part in params.get(s, "").split(","):
            if "=" in part:
                k, v = part.split("=", 1)
                kv[k.strip()] = v.strip()
        dp, dk = (struct_defaults or {}).get(s, ("", ""))
        meta[s] = (kv.get("p", dp), kv.get("K", dk))

    plan = []
    for rep in range(args.reps):
        seed = args.seed_base + rep if seeded else "fixed"
        for mix in mixes:
            prefix, mix_tokens, wfrac = mix_args(mix, args.writeset)
            for kr in key_ranges:
                for dist in dists:
                    dtoks, dmeta = distribution_args(dist)
                    for t in threads:
                        for s in structures:
                            if prefix == "write" and s in no_update:
                                continue
                            for win in windows:
                                r = Run()
                                r.structure, (r.p, r.K) = s, meta[s]
                                r.cops = cops_value(cops_map.get(s), t)
                                r.key_range, r.distribution, r.dist_meta = kr, dist, dmeta
                                r.mix, r.write_frac, r.writeset = mix, wfrac, args.writeset
                                r.threads, r.rep, r.seed = t, rep, seed
                                if win is None:
                                    r.warmup, r.duration, r.window = args.warmup, args.duration, None
                                    if prefix == "write":
                                        # Update mixes allocate for their whole length (reclaimer_none
                                        # frees nothing), so they may run shorter than the read-only mix.
                                        if getattr(args, "update_warmup", None) is not None:
                                            r.warmup = args.update_warmup
                                        if getattr(args, "update_duration", None) is not None:
                                            r.duration = args.update_duration
                                else:
                                    r.warmup, r.duration, r.window = win[0], win[1] - win[0], win
                                r.binary = f"{prefix}_{s}{BIN_SUFFIX}"
                                toks = ["prefill", str(kr), "presecs", str(r.warmup),
                                        "secs", str(r.duration), "threads", str(t)]
                                if r.cops is not None:
                                    toks += ["cops", str(r.cops)]
                                if seeded:
                                    toks += ["seed", str(seed)]
                                toks += dtoks + mix_tokens
                                toks += shlex.split(extra.get(s, ""))
                                r.tokens = toks
                                r.seconds = r.warmup + r.duration
                                plan.append(r)
    if args.shard:
        i, n = (int(v) for v in args.shard.split("/"))
        plan = [r for j, r in enumerate(plan) if j % n == i]
    return plan


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

RESULT_RE = re.compile(r"h:\s*(\S+),\s*ops:\s*(-?\d+),\s*sumLengths:\s*(\S+)")


def parse_output(text, run):
    """(ops, path_length) from the driver's output, or raise ValueError if the
    echoed parameters disagree with what was asked for."""
    if "Wrong arguments" in text:
        raise ValueError("driver rejected the arguments (Wrong arguments)")
    params = {}
    for line in text.splitlines():
        if ":" in line and not line.startswith("h:"):
            k, v = line.split(":", 1)
            params[k.strip()] = v.strip()
    expect = {"threads": str(run.threads), "prefill": str(run.key_range),
              "secs": str(run.duration)}
    if run.dist_meta["zipf_alpha"]:
        expect["zipf"] = "1"
    else:
        expect.update(x=run.dist_meta["x"], y=run.dist_meta["y"], zipf="0")
    for k, v in expect.items():
        if params.get(k) != v:
            raise ValueError(f"driver echoed {k}={params.get(k)!r}, expected {v!r}")
    m = RESULT_RE.search(text)
    if not m:
        raise ValueError("no 'h: .., ops: .., sumLengths: ..' line in the output")
    ops = int(m.group(2))
    if ops <= 0:
        raise ValueError(f"ops = {ops}")
    # Our adapter prints "bsl_stats rule=.. p=.. K=.. ..." at exit (and exits
    # 3 if its validation fails). The p and K it reports are the ones the
    # binary was built with, so they must match what the row records.
    stats = ""
    b = re.search(r"^bsl_stats (.*)$", text, re.M)
    if b:
        stats = b.group(1).strip()
        kv = dict(item.split("=", 1) for item in stats.split() if "=" in item)
        for name, want in (("p", run.p), ("K", run.K)):
            if want and name in kv and float(kv[name]) != float(want):
                raise ValueError(f"binary reports {name}={kv[name]}, the row would record {want}")
    v = re.search(r"^bsl_validate=(\S+)", text, re.M)
    if v:
        stats += f" validate={v.group(1)}"
    return ops, m.group(3), stats


def done_keys(path, cold):
    keys = set()
    if not os.path.exists(path):
        return keys
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            keys.add(tuple(str(row.get(f, "")) for f in (COLD_KEY_FIELDS if cold else KEY_FIELDS)))
    return keys


def default_out(results, site, cold, new):
    stem = "coldstart" if cold else "throughput"
    existing = sorted(glob.glob(os.path.join(results, f"{stem}_{site}_*.csv")))
    if existing and not new:
        return existing[-1]
    today = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d")
    return os.path.join(results, f"{stem}_{site}_{today}.csv")


def jemalloc_ok(lib):
    if not os.path.exists(lib):
        return False
    p = subprocess.run(["env", f"LD_PRELOAD={lib}", "true"], capture_output=True, text=True)
    return p.returncode == 0 and "cannot be preloaded" not in p.stderr


def command(run, bin_dir, prefix):
    pre = list(prefix)
    if run.structure.startswith("bsl_") and run.seed != "fixed":
        # Our structures draw heights from their own per-thread generators,
        # seeded by the BSL_SEED environment variable (bsl_skiplist.h); the
        # harness patch's `seed` argument reaches only the fork's key and
        # operation streams. Without this every repetition of ours would grow
        # the same structure over a different access sequence.
        pre = ["env", f"BSL_SEED={run.seed}"] + pre
    return pre + [os.path.join(bin_dir, run.binary)] + run.tokens


def log_path(log_dir, run, cold):
    name = (f"{run.mix}_{run.distribution}_k{run.key_range}_t{run.threads}"
            f"_r{run.rep}")
    if cold:
        name += f"_w{run.window[0]}-{run.window[1]}"
    return os.path.join(log_dir, "coldstart" if cold else "throughput",
                        run.structure, name + ".log")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    add_arguments(ap)
    args = ap.parse_args(argv)
    return execute(args)


def add_arguments(ap):
    g = ap.add_argument_group("protocol")
    g.add_argument("--structures", type=str_list, default=DEFAULT_STRUCTURES,
                   help="comma-separated ds/ names (default: %(default)s)")
    g.add_argument("--distributions", type=str_list, default=DEFAULT_DISTRIBUTIONS,
                   help="uniform, X-Y hot-set (90-10 = 90%% of operations on 10%% "
                        "of keys), zipfA (default: %(default)s)")
    g.add_argument("--key-ranges", type=int_list, default=DEFAULT_KEY_RANGES,
                   help="prefill sizes (default: %(default)s)")
    g.add_argument("--mixes", type=str_list, default=DEFAULT_MIXES,
                   help="lookup-insert-delete percentages (default: %(default)s)")
    g.add_argument("--threads", type=int_list, default=DEFAULT_THREADS,
                   help="thread counts (default: %(default)s)")
    g.add_argument("--smt", action="store_true",
                   help="also run 256 threads (needs SMT on and harness.patch)")
    g.add_argument("--warmup", type=int, default=20, help="seconds (default 20)")
    g.add_argument("--duration", type=int, default=20, help="measured seconds (default 20)")
    g.add_argument("--reps", type=int, default=10, help="repetitions (default 10)")
    g.add_argument("--seed-base", type=int, default=1,
                   help="repetition r uses seed base+r (default 1; seed 0 is the "
                        "fork's original streams)")
    g.add_argument("--writeset", type=float, default=0.25,
                   help="fraction of keys the update mix inserts and deletes "
                        "(default 0.25, the authors' count_stats_write.sh)")
    g.add_argument("--cops", action="append", metavar="NAME:EXPR",
                   help="adjustment period per structure, may use T (defaults: "
                        + ", ".join(f"{k}:{v}" for k, v in DEFAULT_COPS.items()) + ")")
    g.add_argument("--struct-param", action="append", metavar="NAME:p=..,K=..",
                   help="p and K to record for a structure. For bsl_* they default to "
                        "the build's (XARGS in BUILD_INFO, else bsl_skiplist.h) and are "
                        "checked against what the binary prints")
    g.add_argument("--struct-args", action="append", metavar="NAME:'TOKENS'",
                   help="extra driver tokens for a structure")
    g.add_argument("--no-update", type=str_list, default=[],
                   help="further structures to exclude from update mixes")

    c = ap.add_argument_group("cold start")
    c.add_argument("--cold-start", action="store_true",
                   help="run the throughput-over-time protocol instead")
    c.add_argument("--windows", type=int_list, default=DEFAULT_WINDOWS,
                   help="window boundaries in seconds from cold (default: %(default)s)")
    c.add_argument("--cold-threads", type=int_list, default=DEFAULT_COLD_THREADS,
                   help="(default: %(default)s)")
    c.add_argument("--cold-distributions", type=str_list, default=DEFAULT_DISTRIBUTIONS)
    c.add_argument("--cold-key-ranges", type=int_list, default=[1000000])
    c.add_argument("--cold-mixes", type=str_list, default=["100-0-0"])

    e = ap.add_argument_group("execution")
    e.add_argument("--update-warmup", type=int, default=None,
                   help="warmup seconds for mixes with updates (default: --warmup). Under "
                        "reclaimer_none an update run allocates for its whole length, about "
                        "ops/s x insert share x node bytes; on the sanity node that was 4 GB/s "
                        "for ours at 96 threads, so 20+20 s runs at 128 threads would not fit in RAM")
    e.add_argument("--update-duration", type=int, default=None,
                   help="measured seconds for mixes with updates (default: --duration)")
    e.add_argument("--mem-limit-gb", type=float, default=None,
                   help="stop the campaign when a run's peak resident set exceeds this "
                        "(default: 80%% of the machine's RAM; 0 disables). Under the "
                        "fork's reclaimer_none nothing erased is ever freed, so an update "
                        "mix at many threads can take tens of GB per run: ours allocate "
                        "544-byte nodes (towers preallocated at BSL_MAX_HEIGHT), the fork's "
                        "skip lists 232. Lower BSL_MAX_HEIGHT through XARGS if a run trips this")
    e.add_argument("--site", default=os.environ.get("CHAMELEON_SITE", "tacc"))
    e.add_argument("--work", default=os.environ.get("BSL_SETBENCH_WORK",
                                                     os.path.expanduser("~/bsl-build")),
                   help="build directory from provision.sh (default $BSL_SETBENCH_WORK "
                        "or ~/bsl-build)")
    e.add_argument("--results", default=os.path.join(EXPERIMENTS, "results", "chameleon"))
    e.add_argument("--out", help="CSV path (default: the newest "
                                 "<mode>_<site>_*.csv in --results, else a new dated one)")
    e.add_argument("--new", action="store_true", help="start a new dated CSV")
    e.add_argument("--allocator", choices=["jemalloc", "system"], default="jemalloc")
    e.add_argument("--numa", choices=["interleave", "none"], default="interleave")
    e.add_argument("--max-threads", type=int,
                   help="override the thread limit read from BUILD_INFO")
    e.add_argument("--shard", help="I/N: run every N-th run starting at I (multi-node)")
    e.add_argument("--limit", type=int, help="stop after this many new runs")
    e.add_argument("--timeout-slack", type=int, default=600,
                   help="seconds allowed beyond warmup + duration (default 600)")
    e.add_argument("--skip-missing", action="store_true",
                   help="skip structures whose binary is missing instead of failing")
    e.add_argument("--dry-run", action="store_true",
                   help="print the command lines and the row count, run nothing")


def execute(args):
    cold = args.cold_start
    work = os.path.expanduser(args.work)
    bin_dir = os.path.join(work, "structs_bench", "microbench", "bin")
    lib_dir = os.path.join(work, "structs_bench", "lib")
    info = read_build_info(work)
    patched = info.get("harness_patch") == "1"
    if args.dry_run and not info:
        patched = True  # show the intended protocol
        warn(f"no {work}/BUILD_INFO; dry run assumes harness.patch is applied")
    max_threads = args.max_threads or (256 if patched else UNPATCHED_MAX_THREADS)
    if args.max_threads is None and not patched:
        warn("harness.patch not applied: thread counts capped at 99, "
             "repetitions replay one seed")
    defaults = {s: bsl_params(work, info.get("xargs", ""), s) for s in args.structures}
    plan = build_plan(args, max_threads, seeded=patched, struct_defaults=defaults)

    jemalloc = os.path.join(lib_dir, "libjemalloc.so")
    prefix = []
    if args.allocator == "jemalloc":
        prefix += ["env", f"LD_PRELOAD={jemalloc}"]
    if args.numa == "interleave":
        prefix += ["numactl", "--interleave=all"]

    results = os.path.expanduser(args.results)
    out = args.out or default_out(results, args.site, cold, args.new)
    done = done_keys(out, cold)
    todo = [r for r in plan if r.key(cold) not in done]

    if args.dry_run:
        for r in todo:
            print(shlex.join(command(r, bin_dir, prefix)))
        secs = sum(r.seconds for r in todo)
        print(f"# {len(plan)} rows in the plan, {len(plan) - len(todo)} already in "
              f"{out}, {len(todo)} to run; {secs / 3600:.1f} h of warmup + measurement "
              f"(see estimate.py for overheads)", file=sys.stderr)
        return 0

    # Preflight on the node.
    missing = sorted({r.binary for r in todo
                      if not os.path.exists(os.path.join(bin_dir, r.binary))})
    if missing:
        msg = f"{len(missing)} binaries missing from {bin_dir}: {', '.join(missing)}"
        if not args.skip_missing:
            raise SystemExit(f"run_protocol: {msg} (rerun provision.sh, or --skip-missing)")
        warn(msg + "; skipping them")
        todo = [r for r in todo if r.binary not in missing]
    if args.allocator == "jemalloc" and not jemalloc_ok(jemalloc):
        raise SystemExit(f"run_protocol: {jemalloc} cannot be preloaded "
                         "(use --allocator system and record why)")
    if args.numa == "interleave" and subprocess.run(
            ["sh", "-c", "numactl --interleave=all true"], capture_output=True).returncode:
        raise SystemExit("run_protocol: `numactl --interleave=all` fails here (not installed, "
                         "or not permitted in a container); use --numa none and record why")

    mach = machine_info()
    try:
        ncpu = int(mach["hw_threads"])
    except ValueError:
        ncpu = os.cpu_count() or 1
    over = sorted({r.threads for r in todo if r.threads > ncpu})
    if over:
        warn(f"thread counts {over} exceed the {ncpu} CPUs; the driver's pinning "
             "fails silently there, so they are skipped")
        todo = [r for r in todo if r.threads <= ncpu]

    commit = info.get("commit") or local_commit()
    log_dir = os.path.join(results, "logs", args.site)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    columns = COLD_COLUMNS if cold else COLUMNS
    new_file = not os.path.exists(out)
    fh = open(out, "a", newline="")
    writer = csv.DictWriter(fh, fieldnames=columns)
    if new_file:
        writer.writeheader()
        fh.flush()

    print(f"run_protocol: {len(todo)} runs to do, {len(done)} done, writing {out}",
          flush=True)
    failures = 0
    mem_limit = args.mem_limit_gb if args.mem_limit_gb is not None else 0.8 * mem_total_gb()
    too_big = {}   # (structure, mix, key_range) -> smallest thread count whose run passed the limit
    t_start = time.time()
    for n, r in enumerate(todo[:args.limit] if args.limit else todo, 1):
        cell = (r.structure, r.mix, r.key_range)
        if cell in too_big and r.threads >= too_big[cell]:
            warn(f"skipping {r.structure} {r.mix} k={r.key_range} t={r.threads} rep={r.rep}: a run of this "
                 f"cell at {too_big[cell]} threads passed the memory limit; not recorded")
            continue
        cmd = command(r, bin_dir, prefix)
        lp = log_path(log_dir, r, cold)
        os.makedirs(os.path.dirname(lp), exist_ok=True)
        started = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        t0 = time.time()
        text, rc, peak_mb = run_measured(cmd, r.seconds + args.timeout_slack)
        wall = time.time() - t0
        peak = f"{peak_mb:.0f}" if peak_mb else ""
        with open(lp, "a") as lf:
            lf.write(f"# {started} {shlex.join(cmd)}\n{text}\n# exit {rc} wall {wall:.1f}s"
                     f" peak_rss_mb {peak or 'unknown'}\n")
        if mem_limit and peak_mb / 1024.0 > mem_limit:
            too_big[cell] = min(r.threads, too_big.get(cell, r.threads))
            warn(f"MEMORY: {r.structure} {r.mix} {r.distribution} k={r.key_range} t={r.threads} "
                 f"reached {peak_mb / 1024.0:.2f} GB resident, over the limit of {mem_limit:.2f} GB "
                 "(--mem-limit-gb). The row is kept; this cell is skipped from this thread count up "
                 "for the rest of the campaign. Shorter update runs (--update-duration), a lower "
                 "BSL_MAX_HEIGHT (XARGS) or a higher limit, then a rerun, fill the gap.")
        try:
            if rc != 0:
                raise ValueError(f"exit status {rc}")
            ops, plen, ds_stats = parse_output(text, r)
        except ValueError as exc:
            failures += 1
            warn(f"FAILED {r.structure} {r.mix} {r.distribution} k={r.key_range} "
                 f"t={r.threads} rep={r.rep}: {exc} (log {lp}); not recorded, "
                 "a rerun retries it")
            continue
        row = dict(site=args.site, started_utc=started, commit=commit,
                   harness_patch="1" if patched else "0",
                   allocator=args.allocator, numa_policy=args.numa,
                   ops=ops, throughput_ops_s=f"{ops / r.duration:.1f}",
                   path_length=plen, ds_stats=ds_stats, wall_s=f"{wall:.1f}", peak_rss_mb=peak,
                   build_xargs=info.get("xargs", ""),
                   log=os.path.relpath(lp, results), cmdline=shlex.join(cmd))
        row.update({k: mach[k] for k in ("host", "machine", "cpu_model", "sockets", "cores",
                                         "hw_threads", "threads_per_core", "numa_nodes",
                                         "kernel", "governor", "pin_layout")})
        row.update(r.row_fields())
        writer.writerow({k: row.get(k, "") for k in columns})
        fh.flush()
        os.fsync(fh.fileno())
        elapsed = time.time() - t_start
        left = len(todo) - n
        print(f"[{n}/{len(todo)}] {r.structure} {r.mix} {r.distribution} "
              f"k={r.key_range} t={r.threads} rep={r.rep}"
              + (f" w={r.window[0]}-{r.window[1]}" if cold else "")
              + f": {ops / r.duration / 1e6:.3f} Mops/s, path {plen}"
              f"  (eta {elapsed / n * left / 3600:.1f} h)", flush=True)
    fh.close()
    if failures:
        warn(f"{failures} runs failed; see the logs, then rerun the same command")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
