#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The pair rehearsal workflow must stay unable to publish or to reach the
# release lane: dispatch only, read-only, no environment, no secret, no call
# to another workflow, no write command. Run: tools/ci/test_pair_rehearsal_shape.sh [workflow]
# Self-checks against a deliberately widened copy, so a weakened rule fails here.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WF="${1:-$HERE/../../.github/workflows/pair-rehearsal.yml}"
FAILS=0

# Prints one violation per line for the workflow file $1.
violations() {
  python3 -I - "$1" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
code = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))
v = []
on = re.search(r"^on:\n((?:[ ]+.*\n|\n)+)", code + "\n", re.M)
triggers = re.findall(r"^  ([a-z_]+):", on.group(1), re.M) if on else []
if triggers != ["workflow_dispatch"]:
    v.append("triggers are %r, not only workflow_dispatch" % triggers)
perms = re.search(r"^permissions:\n((?:  .*\n)+)", code + "\n", re.M)
granted = dict(re.findall(r"^  ([a-z-]+):\s*(\w+)", perms.group(1), re.M)) if perms else {"?": "missing"}
if granted != {"contents": "read", "actions": "read"}:
    v.append("top-level permissions are %r, not contents/actions read" % granted)
if re.search(r"^\s+permissions:", code.split("\njobs:")[-1], re.M):
    v.append("a job sets its own permissions")
for pat, what in [
    (r"^\s+environment:", "names an environment"),
    (r"secrets\.|secrets:|secrets\[", "reads or passes a secret"),
    (r"^\s+uses:\s+(?!actions/checkout@)", "uses an action or workflow other than checkout"),
    (r"workflow_call", "is callable"),
    (r"release\.yml|pipeline-release", "names a release workflow"),
    (r"\bgh\s+(release|workflow|api\s+(-X|--method)|pr\s+(merge|create))", "runs a gh write command"),
    (r"\bgit\s+(push|tag\s+-)", "pushes or creates a tag"),
    (r"upload-artifact|actions/cache|docker\s+login|gcloud|aws |twine|nuget", "uploads or publishes"),
]:
    if re.search(pat, code, re.M):
        v.append(what)
if not re.search(r"runs-on:\s*ubuntu-latest", code):
    v.append("not on a hosted runner")
print("\n".join(v))
PY
}

out="$(violations "$WF")"
if [ -z "$out" ]; then echo "PASS: pair-rehearsal.yml is dispatch-only, read-only, no environment, no secret, no publish path"
else echo "FAIL: $out"; FAILS=$((FAILS + 1)); fi

# Teeth: each widening of a copy must be reported.
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
widen() { # name sed-free python replacement: old new
  python3 -I - "$WF" "$T/w.yml" "$2" "$3" <<'PY'
import sys
s = open(sys.argv[1]).read()
assert sys.argv[3] in s, "fixture anchor missing: " + sys.argv[3]
open(sys.argv[2], "w").write(s.replace(sys.argv[3], sys.argv[4], 1))
PY
  if [ -n "$(violations "$T/w.yml")" ]; then echo "PASS: refuses $1"
  else echo "FAIL: did not refuse $1"; FAILS=$((FAILS + 1)); fi
}
widen "a push trigger"          "  workflow_dispatch:" "  push:
  workflow_dispatch:"
widen "write permission"        "  actions: read" "  actions: write"
widen "an environment"          "    runs-on: ubuntu-latest" "    environment: release
    runs-on: ubuntu-latest"
widen "a secret"                "GH_TOKEN: \${{ github.token }}" "GH_TOKEN: \${{ secrets.NUGET_API_KEY }}"
widen "a call to the release lane" "    steps:" "    uses: We-Amp/mod_pagespeed/.github/workflows/pipeline-release-module.yml@master
    steps:"
widen "a tag push"              "bash tools/ci/pair_rehearsal.sh" "git push origin HEAD; bash tools/ci/pair_rehearsal.sh"
widen "a release command"       "bash tools/ci/pair_rehearsal.sh" "gh release create x; bash tools/ci/pair_rehearsal.sh"
widen "a fleet runner"          "runs-on: ubuntu-latest" "runs-on: fleet-heavy"
[ "$FAILS" -eq 0 ] || { echo "$FAILS failed"; exit 1; }
