#!/usr/bin/env python3
"""SessionStart hook: give Claude a short picture of the working tree at the
start of every session. Plain stdout is added to Claude's context.

Reports the branch and uncommitted changes, whether the structural build
exists, whether the SetBench fork is cloned, and any regenerated output
waiting in rerun/.
"""
import os
import subprocess
import sys


def sh(*cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def main() -> None:
    root = os.environ.get("CLAUDE_PROJECT_DIR", os.getcwd())
    os.chdir(root)
    branch = sh("git", "rev-parse", "--abbrev-ref", "HEAD") or "?"
    head = sh("git", "log", "-1", "--format=%h %s") or "?"
    changes = sh("git", "status", "--porcelain")
    n_changes = len(changes.splitlines()) if changes else 0
    built = os.path.exists("structural/build/CMakeCache.txt")
    fork = os.path.isdir("third_party/splaylist/structs_bench")
    rerun = []
    for d in ("rerun", "structural/rerun"):
        if os.path.isdir(d):
            rerun += [os.path.join(d, f) for f in os.listdir(d) if f.endswith(".csv")]

    print("Session context (from .claude/hooks/session_context.py):")
    print(f"- branch {branch}, head {head}; {n_changes} uncommitted change(s)")
    print(f"- structural build: {'present' if built else 'not configured yet (cmake -S structural -B structural/build)'}")
    print(f"- SetBench fork: {'cloned' if fork else 'not cloned (see the concurrent-validation skill)'}")
    if rerun:
        print(f"- {len(rerun)} regenerated CSV(s) in rerun/ not yet compared or cleaned")
    print("- committed results are read-only; see CLAUDE.md for the rules")
    sys.exit(0)


if __name__ == "__main__":
    main()
