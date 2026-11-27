#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Static self-test: every job routed to the Windows build fleet starts with the
# vendor reparse-point unlink step (before any checkout), no hosted-only job
# carries it (a hosted twin sharing the step list keeps it guarded and inert),
# and no workflow spells the runner-class token literally (the shape gate
# forbids the word anywhere under tools/ci).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 1
FILES=(
  .github/workflows/pipeline-pr.yml
  .github/workflows/pipeline-full.yml
  .github/workflows/pipeline-release-optimizer.yml
  .github/workflows/pipeline-release-nuget.yml
)
fail=0

# The job's python3 (setup-python) has no PyYAML; take the first interpreter
# that does, and fail closed if none does.
PY_BIN=""
for py in python3 /usr/bin/python3; do
  if "$py" -c 'import yaml' >/dev/null 2>&1; then PY_BIN="$py"; break; fi
done
if [ -z "$PY_BIN" ]; then echo "FAIL: no python3 with PyYAML found" >&2; exit 1; fi

out="$("$PY_BIN" - "${FILES[@]}" <<'PY'
import sys, yaml

checked = 0

NAME = "Unlink vendor reparse points before checkout (build runner)"
IF = "runner.environment != 'github-hosted'"
RUN = (
    "$s = Join-Path $env:GITHUB_WORKSPACE 'tools\\ci\\unlink-vendor-reparse.ps1'\n"
    "if (Test-Path -LiteralPath $s) { & $s -Workspace $env:GITHUB_WORKSPACE } "
    "else { Write-Host 'no previous checkout' }\n"
)
HOSTED_ONLY = "runner.environment == 'github-hosted'"

def is_spec(step):
    return (step.get("name") == NAME and step.get("if") == IF
            and step.get("shell") == "powershell" and step.get("run") == RUN)

for f in sys.argv[1:]:
    jobs = yaml.safe_load(open(f)).get("jobs", {})
    for name, job in jobs.items():
        ro = job.get("runs-on")
        steps = job.get("steps") or []
        if isinstance(ro, str) and "fleet-windows" in ro:
            checked += 1
            # Skip leading hosted-only steps (they never run on the fleet).
            i = 0
            while i < len(steps) and steps[i].get("if") == HOSTED_ONLY:
                i += 1
            if i >= len(steps) or not is_spec(steps[i]):
                print(f"FAIL {f}:{name}")
        else:
            # Hosted twins share the fleet step list; there the step is inert
            # because of its runner.environment guard. A hosted-only job must
            # not carry it at all, and no copy may lack the guard.
            twin = name.endswith("-hosted")
            for s in steps:
                if s.get("name") == NAME and (not twin or not is_spec(s)):
                    print(f"HOSTED {f}:{name}")
                    break

print(f"CHECKED {checked}")
PY
)" || { echo "FAIL: workflow parse failed" >&2; exit 1; }
n="$(sed -n 's/^CHECKED //p' <<<"$out")"
if [ -z "$n" ] || [ "$n" -lt 7 ]; then echo "FAIL: inspected ${n:-0} fleet Windows jobs, expected at least 7" >&2; exit 1; fi
echo "ok: inspected $n fleet Windows jobs"
if grep -q '^FAIL ' <<<"$out"; then grep '^FAIL ' <<<"$out"; echo "FAIL: every fleet-windows leg unlinks first"; fail=1; else echo "ok: every fleet-windows leg unlinks first"; fi
if grep -q '^HOSTED ' <<<"$out"; then grep '^HOSTED ' <<<"$out"; echo "FAIL: no hosted job carries the step"; fail=1; else echo "ok: no hosted job carries the step"; fi
tok="self-""hosted"
if grep -nE "$tok" "${FILES[@]}" >/dev/null; then grep -nE "$tok" "${FILES[@]}"; echo "FAIL: no literal $tok token"; fail=1; else echo "ok: no literal $tok token"; fi
exit "$fail"
