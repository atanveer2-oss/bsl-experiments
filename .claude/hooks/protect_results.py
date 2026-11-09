#!/usr/bin/env python3
"""PreToolUse hook: committed results are read-only.

Blocks (exit 2, reason on stderr, which Claude sees):
  - Edit/Write to any file under structural/results/ or concurrent/results*/;
  - Bash commands that write into those folders (redirection, cp, mv, tee,
    rm, sed -i), e.g. copying a regenerated CSV over a committed one.

Regenerated results belong in rerun/; a committed CSV changes only through
an intentional rerun that the user asks for (CLAUDE.md, rule 2), which the
user runs or approves outside this guard.
"""
import json
import os
import re
import shlex
import sys

PROTECTED = re.compile(r"(^|/)(structural/results|concurrent/results(-p50|-sanity)?)(/|$)")


def bash_writes_protected(command: str) -> bool:
    """True if any segment of the command writes into a protected folder:
    a redirection target, the destination of cp/mv/install/rsync, any
    argument of rm/tee/truncate, or the file of sed -i."""
    for seg in re.split(r"&&|\|\||[;|\n]", command):
        for m in re.finditer(r">{1,2}\s*(\S+)", seg):
            if PROTECTED.search(m.group(1)):
                return True
        try:
            words = shlex.split(seg)
        except ValueError:
            words = seg.split()
        words = [w for w in words if not re.match(r"^\d?>", w)]
        if not words:
            continue
        prog, args = os.path.basename(words[0]), [w for w in words[1:] if not w.startswith("-")]
        if prog in ("cp", "mv", "install", "rsync", "ln") and args:
            targets = args[-1:]
        elif prog in ("rm", "tee", "truncate", "touch"):
            targets = args
        elif prog == "sed" and any(w.startswith("-i") for w in words[1:]):
            targets = args[1:]
        else:
            targets = []
        if any(PROTECTED.search(t) for t in targets):
            return True
    return False


def block(reason: str) -> None:
    print(reason, file=sys.stderr)
    sys.exit(2)


def main() -> None:
    event = json.load(sys.stdin)
    tool = event.get("tool_name", "")
    tool_input = event.get("tool_input", {}) or {}
    root = os.environ.get("CLAUDE_PROJECT_DIR", event.get("cwd", ""))

    if tool in ("Edit", "Write", "MultiEdit", "NotebookEdit"):
        path = tool_input.get("file_path") or tool_input.get("notebook_path") or ""
        rel = os.path.relpath(os.path.abspath(path), root) if root else path
        if PROTECTED.search(rel):
            block(f"Blocked by .claude/hooks/protect_results.py: {rel} is a committed "
                  "result and is read-only. Write regenerated output to rerun/ and "
                  "compare with cmp (CLAUDE.md, rule 2).")
    elif tool == "Bash":
        command = tool_input.get("command", "")
        if bash_writes_protected(command):
            block("Blocked by .claude/hooks/protect_results.py: this command writes into "
                  "a committed results folder. Write to rerun/ instead; committed CSVs "
                  "change only by a rerun the user asks for (CLAUDE.md, rule 2).")
    sys.exit(0)


if __name__ == "__main__":
    main()
