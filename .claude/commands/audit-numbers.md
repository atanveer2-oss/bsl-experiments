---
description: Have the results-auditor agent recompute every number in a document from the committed CSVs.
argument-hint: <path to a README, summary or table>
---

Use the `results-auditor` subagent to audit the numbers in `$ARGUMENTS`.

Give it the path and nothing else about what the numbers should be, so its
check is independent. When it reports back, show the user its table of
claims, then list only the claims that do not match, each with the file and
line in `$ARGUMENTS` and the recomputed value. Do not edit `$ARGUMENTS`
yourself; ask the user which corrections to make.
