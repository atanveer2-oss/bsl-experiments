#!/usr/bin/env python3
"""PostToolUse hook: after Claude edits structural C++ code, rebuild and run
the unit tests, and tell Claude if either fails.

Runs only for files under structural/include/ or structural/bench/ (.hpp,
.cpp, .h) and structural/CMakeLists.txt. Configures structural/build on
first use. On failure it returns {"decision": "block", "reason": ...} with
the tail of the output, so the next step is to fix the code; on success it
stays silent.
"""
import json
import os
import re
import subprocess
import sys

WATCHED = re.compile(r"(^|/)structural/((include|bench)/.*\.(hpp|cpp|h)|CMakeLists\.txt)$")


def run(cmd, cwd):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr)


def main() -> None:
    event = json.load(sys.stdin)
    path = (event.get("tool_input") or {}).get("file_path", "")
    root = os.environ.get("CLAUDE_PROJECT_DIR") or event.get("cwd") or "."
    rel = os.path.relpath(os.path.abspath(path), root) if path else ""
    if not WATCHED.search(rel):
        sys.exit(0)

    build = os.path.join(root, "structural", "build")
    steps = []
    if not os.path.exists(os.path.join(build, "CMakeCache.txt")):
        steps.append(["cmake", "-S", "structural", "-B", "structural/build",
                      "-DCMAKE_BUILD_TYPE=Release"])
    steps.append(["cmake", "--build", "structural/build", "-j8"])
    steps.append(["ctest", "--test-dir", "structural/build", "--output-on-failure"])

    for cmd in steps:
        code, out = run(cmd, root)
        if code != 0:
            tail = "\n".join(out.strip().splitlines()[-40:])
            print(json.dumps({
                "decision": "block",
                "reason": (f"build_and_test hook: `{' '.join(cmd)}` failed after editing "
                           f"{rel}. Fix this before continuing.\n{tail}"),
            }))
            sys.exit(0)
    sys.exit(0)


if __name__ == "__main__":
    main()
