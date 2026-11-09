#!/usr/bin/env python3
"""PreToolUse hook on Bash: keep cloud credentials out of the conversation
and out of git.

Blocks (exit 2, reason on stderr):
  - commands that read or copy the Chameleon credential folder or SSH keys
    (cat, less, head, cp, source, grep ... on ~/.config/chameleon or ~/.ssh);
  - `git add` of credential-looking files (*openrc*, *.pem, id_rsa, id_ed25519,
    .env) or of a whole directory while such a file is untracked in it.

The campaign scripts themselves read the RC file (reserve.sh sources it),
which this hook allows: the secret then stays inside that process.
"""
import json
import re
import subprocess
import sys

SECRET_PATH = re.compile(r"(\.config/chameleon|\.ssh/|openrc\.sh|app-cred-)")
READS = re.compile(r"(^|\s)(cat|less|more|head|tail|cp|scp|rsync|source|\.|grep|awk|sed|base64|xxd|open|vi|vim|nano)\s")
SECRET_FILE = re.compile(r"(openrc|\.pem$|(^|/)id_(rsa|ed25519)(\.pub)?$|(^|/)\.env$)")


def block(reason: str) -> None:
    print(reason, file=sys.stderr)
    sys.exit(2)


def main() -> None:
    event = json.load(sys.stdin)
    if event.get("tool_name") != "Bash":
        sys.exit(0)
    command = (event.get("tool_input") or {}).get("command", "")

    for part in re.split(r"[;&|]+", command):
        if SECRET_PATH.search(part) and READS.search(part):
            block("Blocked by .claude/hooks/block_secrets.py: this command reads a "
                  "credential (Chameleon RC file or SSH key). Credentials never enter "
                  "the conversation; the campaign scripts read the RC file themselves "
                  "(CLAUDE.md, rule 6).")

    if re.search(r"\bgit\s+add\b", command):
        args = command.split("git add", 1)[1].split()
        if any(SECRET_FILE.search(a) for a in args):
            block("Blocked by .claude/hooks/block_secrets.py: refusing to stage a "
                  "credential-looking file.")
        if any(a in (".", "-A", "--all") or a.endswith("/") for a in args):
            try:
                untracked = subprocess.run(
                    ["git", "ls-files", "--others", "--exclude-standard"],
                    capture_output=True, text=True, timeout=5).stdout.split()
            except (OSError, subprocess.SubprocessError):
                untracked = []
            bad = [f for f in untracked if SECRET_FILE.search(f)]
            if bad:
                block("Blocked by .claude/hooks/block_secrets.py: `git add` would stage "
                      f"credential-looking files: {', '.join(bad)}.")
    sys.exit(0)


if __name__ == "__main__":
    main()
