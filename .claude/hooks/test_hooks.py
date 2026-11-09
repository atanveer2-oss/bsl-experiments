#!/usr/bin/env python3
"""Tests for the hooks: feed each hook the JSON Claude Code sends on stdin
and check its decision. Run from the repository root:

    python3 .claude/hooks/test_hooks.py

Exit status 0 when every case passes. build_and_test.py is tested on a
file it must ignore and on a structural header (which builds and runs ctest,
about a minute the first time).
"""
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HOOKS = os.path.join(ROOT, ".claude", "hooks")


def call(hook, event):
    env = dict(os.environ, CLAUDE_PROJECT_DIR=ROOT)
    p = subprocess.run([sys.executable, os.path.join(HOOKS, hook)], input=json.dumps(event),
                       capture_output=True, text=True, env=env, cwd=ROOT)
    return p.returncode, p.stdout, p.stderr


def edit(path):
    return {"hook_event_name": "PreToolUse", "tool_name": "Edit", "cwd": ROOT,
            "tool_input": {"file_path": os.path.join(ROOT, path)}}


def bash(cmd):
    return {"hook_event_name": "PreToolUse", "tool_name": "Bash", "cwd": ROOT,
            "tool_input": {"command": cmd}}


CASES = [
    # (hook, event, expected exit code, description)
    ("protect_results.py", edit("structural/results/structural_zipf1.5.csv"), 2, "edit a committed CSV"),
    ("protect_results.py", edit("concurrent/results-p50/node-1/throughput_tacc_2026-10-29.csv"), 2, "edit a committed throughput CSV"),
    ("protect_results.py", edit("structural/include/bsl/skiplist.hpp"), 0, "edit source code"),
    ("protect_results.py", edit("rerun/structural_zipf1.5.csv"), 0, "write to rerun/"),
    ("protect_results.py", bash("cp rerun/structural_zipf1.5.csv structural/results/"), 2, "copy over a committed CSV"),
    ("protect_results.py", bash("./structural/build/structural --keys=1000 > structural/results/x.csv"), 2, "redirect into results/"),
    ("protect_results.py", bash("cp structural/results/structural_zipf1.5.csv /tmp/x.csv"), 0, "copy out of results/"),
    ("protect_results.py", bash("cmp rerun/a.csv structural/results/a.csv"), 0, "compare with results/"),
    ("protect_results.py", bash("sed -i '' s/a/b/ concurrent/results/node-1/BUILD_INFO"), 2, "sed -i on a result"),
    ("protect_results.py", bash("git rm -r --cached structural/results"), 0, "git subcommands are left to permissions"),
    ("block_secrets.py", bash("cat ~/.config/chameleon/CHI@TACC-openrc.sh"), 2, "print the RC file"),
    ("block_secrets.py", bash("source ~/.config/chameleon/CHI@TACC-openrc.sh && openstack lease list"), 2, "source the RC file into the session"),
    ("block_secrets.py", bash("cp ~/.ssh/id_ed25519 ."), 2, "copy an SSH key"),
    ("block_secrets.py", bash("git add CHI@TACC-openrc.sh"), 2, "stage an RC file"),
    ("block_secrets.py", bash("./reserve.sh status"), 0, "campaign script reads the RC itself"),
    ("block_secrets.py", bash("./collect.sh --hosts-file ~/.config/chameleon/bsl-hosts"), 0, "pass the hosts file path"),
    ("block_secrets.py", bash("git add concurrent/README.md"), 0, "stage an ordinary file"),
]


def main():
    failed = 0
    for hook, event, want, what in CASES:
        code, _, err = call(hook, event)
        ok = code == want
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {hook:22} {what} (exit {code}, want {want})")
        if not ok and err:
            print("      " + err.strip().splitlines()[0])

    # build_and_test: ignores non-C++ files, builds and tests on a header.
    post = {"hook_event_name": "PostToolUse", "tool_name": "Edit", "cwd": ROOT,
            "tool_input": {"file_path": os.path.join(ROOT, "README.md")}}
    code, out, _ = call("build_and_test.py", post)
    ok = code == 0 and out.strip() == ""
    failed += not ok
    print(f"{'PASS' if ok else 'FAIL'}  build_and_test.py      ignores README.md")
    post["tool_input"]["file_path"] = os.path.join(ROOT, "structural/include/bsl/rng.hpp")
    code, out, _ = call("build_and_test.py", post)
    ok = code == 0 and '"decision"' not in out
    failed += not ok
    print(f"{'PASS' if ok else 'FAIL'}  build_and_test.py      builds and tests after a header edit"
          + ("" if ok else f": {out.strip()[:300]}"))

    code, out, _ = call("session_context.py", {"hook_event_name": "SessionStart", "source": "startup"})
    ok = code == 0 and "Session context" in out
    failed += not ok
    print(f"{'PASS' if ok else 'FAIL'}  session_context.py     prints the session context")

    print(f"\n{len(CASES) + 3 - failed} of {len(CASES) + 3} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
