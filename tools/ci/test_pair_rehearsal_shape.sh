#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The pair rehearsal must stay unable to publish or to reach the release lane:
# one hosted job, dispatch only, read-only, no environment, no secret, no call
# to another workflow, no write command, nothing run from the candidate tree,
# and no way to turn a failed check green. The rules read the workflow text
# (comments stripped) and the gh calls of tools/ci/pair_rehearsal.sh.
# The test then applies each widening below to a copy and requires it refused.
#
# Run: tools/ci/test_pair_rehearsal_shape.sh [workflow]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WF="${1:-$HERE/../../.github/workflows/pair-rehearsal.yml}"
SCRIPT="$HERE/pair_rehearsal.sh"
FAILS=0

if [ ! -f "$WF" ]; then echo "FAIL: no workflow file at $WF"; exit 1; fi
if [ ! -f "$SCRIPT" ]; then echo "FAIL: no script at $SCRIPT"; exit 1; fi

# Prints one violation per line for the workflow $1 and the script $2.
violations() {
  python3 -I - "$1" "$2" <<'PY'
import re, sys
raw = open(sys.argv[1]).read()
lines = [l for l in raw.splitlines() if not l.lstrip().startswith("#")]
code = "\n".join(lines)
v = []
def has(pat, flags=re.M):
    return re.search(pat, code, flags) is not None

# Triggers and permissions.
on = re.search(r"^on:\n((?:[ ]+.*\n|\n)+)", code + "\n", re.M)
triggers = re.findall(r"^  ([a-z_]+):", on.group(1), re.M) if on else []
if triggers != ["workflow_dispatch"]:
    v.append("triggers are %r, not only workflow_dispatch" % triggers)
perms = re.search(r"^permissions:\n((?:  .*\n)+)", code + "\n", re.M)
granted = dict(re.findall(r"^  ([a-z-]+):\s*(\w+)\s*$", perms.group(1), re.M)) if perms else {"?": "missing"}
if granted != {"contents": "read", "actions": "read"}:
    v.append("top-level permissions are %r, not contents/actions read" % granted)
jobs_text = code.split("\njobs:\n", 1)[-1] if "\njobs:\n" in code else ""
if re.search(r"^\s+permissions:", jobs_text, re.M):
    v.append("a job sets its own permissions")

# One job, hosted, only on master.
job_names = re.findall(r"^  ([A-Za-z0-9_-]+):\s*$", jobs_text, re.M)
if len(job_names) != 1:
    v.append("has %d jobs, not one" % len(job_names))
if re.findall(r"^\s*runs-on:\s*(.*?)\s*$", code, re.M) != ["ubuntu-latest"]:
    v.append("runs-on is not exactly one ubuntu-latest")
if not has(r"^    if: \$\{\{ github\.event_name == 'workflow_dispatch' && github\.ref == 'refs/heads/master' \}\}$"):
    v.append("the job is not limited to a dispatch on master")
if not has(r"^    timeout-minutes: \d+$"):
    v.append("no job timeout")

# Steps: only the two pinned checkouts, and nothing else is used or called.
uses = re.findall(r"^\s*(?:-\s+)?uses:\s*(.*?)\s*$", code, re.M)
for u in uses:
    if not re.fullmatch(r"actions/checkout@[0-9a-f]{40} # v[0-9.]+", u):
        v.append("uses %r: only a pinned actions/checkout is allowed" % u)
if len(uses) != 2:
    v.append("has %d uses: lines, expected the two checkouts" % len(uses))
persist = re.findall(r"persist-credentials:\s*(\S+)", code)
if persist != ["false", "false"]:
    v.append("persist-credentials is %r, not false on both checkouts" % persist)
if has(r"^\s*repository:") or has(r"^\s*tok[e]n:") or has(r"^\s*ssh-key"):
    v.append("a checkout sets repository, token or ssh-key")
refs = re.findall(r"^\s*ref:\s*(.*?)\s*$", code, re.M)
if refs != ["master", "refs/tags/${{ inputs.tag }}"]:
    v.append("checkout refs are %r: the check must come from master, the candidate from its tag" % refs)
if re.findall(r"^\s*path:\s*(\S+)", code, re.M) != ["candidate"]:
    v.append("the candidate is not checked out to candidate/ alone")
if len(re.findall(r"sparse-checkout-cone-mode:\s*false\s*$", code, re.M)) != 2:
    v.append("a checkout lacks an explicit non-cone sparse-checkout")
if len(re.findall(r"^\s*sparse-checkout:\s*\|", code, re.M)) != 2:
    v.append("a checkout lacks an explicit sparse-checkout list")

# Forbidden anywhere.
for pat, what in [
    (r"^\s+environment:", "names an environment"),
    (r"\bsecrets\b", "mentions secrets"),
    (r"toJSON|fromJSON", "uses toJSON or fromJSON"),
    (r"workflow_call|workflow_run|repository_dispatch", "is callable or chained"),
    (r"release\.yml|pipeline-release", "names a release workflow"),
    (r"continue-on-error", "sets continue-on-error"),
    (r"\|\|\s*(true|:|exit\s+0)\b", "turns a failure green"),
    (r"set\s+\+e|set\s+\+o\s+pipefail|\bset\s+-[a-z]*\+", "disables errexit or pipefail"),
    (r"\bgh\b", "calls gh (only the script reads, with gh run view and gh api GET)"),
    (r"\bgit\s+(push|tag|commit|fetch|remote|config)", "writes with git"),
    (r"\bcurl\b[^\n]*\s(-X|--request|-d|--data|-F|-T|--upload-file)\b", "curl writes"),
    (r"upload-artifact|actions/cache|docker\s|gcloud|aws\s|twine|\bnuget\b|\bnpm\s+publish", "uploads or publishes"),
    (r"\b(eval|source)\b|^\s*\.\s+\S", "sources or evaluates"),
]:
    if has(pat):
        v.append(what)

# Run blocks: strict mode, no expression, and only the master copy is run.
runs = re.findall(r"^( +)run:\s*\|\n((?:\1  .*\n|\n)+)", code + "\n", re.M)
if len(runs) != 1:
    v.append("has %d run blocks, expected one" % len(runs))
for _, body in runs:
    if not re.search(r"^\s*set -euo pipefail\s*$", body, re.M):
        v.append("the run block does not set -euo pipefail")
    if "${{" in body:
        v.append("an expression inside run: (inputs reach the shell only through env:)")
    for ln in body.splitlines():
        if "candidate" in ln and not any(re.fullmatch(p, ln.strip()) for p in [
            r"PIN=candidate/install/debian/optimizer-pin\.json",
            r"--repositories-bzl candidate/bazel/repositories\.bzl\)",
            r'ARGS\+=\(--run-id "\$RUN_ID" --repo "\$GITHUB_REPOSITORY" --tag-sha "\$\(git -C candidate rev-parse HEAD\)"\)',
        ]):
            v.append("the run block touches the candidate tree other than as data: %r" % ln.strip())
    for ln in body.splitlines():
        s = ln.strip()
        if re.match(r"(bash|sh|python3?|node|perl|ruby)\b", s) and not (
                s.startswith("bash tools/ci/pair_rehearsal.sh ") or s.startswith("python3 -I -c ")
                or "$(python3 -I -c " in s):
            v.append("the run block runs %r: only the master copy of the rehearsal is run" % s[:60])
    for need in ["--tag ", "--pin ", "--optimizer-version-file ", "--optimizer-module-bazel ",
                 "--repositories-bzl ", "--run-id ", "--tag-sha ", "--repo "]:
        if need not in body:
            v.append("the rehearsal is not given %s" % need.strip())
    if not re.search(r"tee -a \"\$GITHUB_STEP_SUMMARY\"\s*$", body.strip(), re.M):
        v.append("the rehearsal output does not end in the job summary")
    if not re.search(r'case "\$TAG" in module-v\*\)', body):
        v.append("the tag input is not checked")

# The script: its only gh calls are reads.
src = open(sys.argv[2]).read()
for m in re.finditer(r"^[^#\n]*\bgh (\S+)( \S+)?[^\n]*$", src, re.M):
    ln = m.group(0)
    if "echo" in ln or "FAIL" in ln and "gh " not in ln.split("FAIL")[0]:
        continue
    sub = (m.group(1) + (m.group(2) or "")).strip()
    if sub not in ("run view", "api --allow-escape-sequences"):
        v.append("pair_rehearsal.sh calls gh %s" % sub)
    if re.search(r"\s(-X|--method|-f|-F|--field|--raw-field|--input)\b", ln):
        v.append("pair_rehearsal.sh makes a gh write call")
print("\n".join(v))
PY
}

out="$(violations "$WF" "$SCRIPT")"
if [ -z "$out" ]; then echo "PASS: $(basename "$WF") is one hosted dispatch-only read-only job, no environment, secret or publish path"
else echo "FAIL: $out"; FAILS=$((FAILS + 1)); fi

# Each widening of a copy must be reported.
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
widen() { # name old new
  if ! python3 -I - "$WF" "$T/w.yml" "$2" "$3" <<'PY'
import sys
s = open(sys.argv[1]).read()
assert sys.argv[3] in s, "mutation anchor missing: " + sys.argv[3]
open(sys.argv[2], "w").write(s.replace(sys.argv[3], sys.argv[4], 1))
PY
  then echo "FAIL: anchor for $1 not found"; FAILS=$((FAILS + 1)); return; fi
  if [ -n "$(violations "$T/w.yml" "$SCRIPT")" ]; then echo "PASS: refuses $1"
  else echo "FAIL: did not refuse $1"; FAILS=$((FAILS + 1)); fi
}
RUN='bash tools/ci/pair_rehearsal.sh'
widen "a push trigger"            "  workflow_dispatch:" "  push:
  workflow_dispatch:"
widen "a schedule trigger"        "  workflow_dispatch:" "  schedule:
    - cron: '0 * * * *'
  workflow_dispatch:"
widen "a write permission"        "  actions: read" "  actions: write"
widen "contents write"            "  contents: read" "  contents: write"
widen "a job permission"          "    timeout-minutes: 10" "    timeout-minutes: 10
    permissions:
      contents: write"
widen "an environment"            "    timeout-minutes: 10" "    timeout-minutes: 10
    environment: release"
widen "a secret"                  "GH_TOKEN: \${{ github.token }}" "GH_TOKEN: \${{ secrets.NUGET_API_KEY }}"
widen "toJSON(secrets)"           "GH_TOKEN: \${{ github.token }}" "GH_TOKEN: \${{ github.token }}
          ALL: \${{ toJSON(secrets) }}"
widen "a call to the release lane" "    steps:" "    uses: We-Amp/mod_pagespeed/.github/workflows/pipeline-release-module.yml@master
    steps:"
widen "a tag push"                "$RUN" "git push origin HEAD; $RUN"
widen "a release command"         "$RUN" "gh release create x; $RUN"
widen "gh api -X POST"            "$RUN" "gh api -X POST repos/x/y/dispatches; $RUN"
widen "gh run rerun"              "$RUN" "gh run rerun 1; $RUN"
widen "a non-hosted runner"       "runs-on: ubuntu-latest" "runs-on: fleet-heavy"
widen "another hosted label"      "runs-on: ubuntu-latest" "runs-on: ubuntu-24.04"
widen "a second job"              "    steps:" "    steps: []
  other:
    runs-on: ubuntu-latest
    steps:"
widen "the master condition removed" "    if: \${{ github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/master' }}
" ""
widen "|| true"                   "$RUN \"\${ARGS[@]}\"" "$RUN \"\${ARGS[@]}\" || true; true"
widen "continue-on-error"         "    timeout-minutes: 10" "    timeout-minutes: 10
    continue-on-error: true"
widen "pipefail removed"          "set -euo pipefail" "set -eu"
widen "errexit switched off"      "set -euo pipefail" "set -euo pipefail; set +e"
widen "the tool checkout at the tag" "          ref: master" "          ref: refs/tags/\${{ inputs.tag }}"
widen "the candidate at a branch" "          ref: refs/tags/\${{ inputs.tag }}" "          ref: \${{ inputs.tag }}"
widen "running the candidate's script" "$RUN" "bash candidate/tools/ci/pair_rehearsal.sh"
widen "running candidate python"  "$RUN" "python3 candidate/tools/ci/x.py; $RUN"
widen "a candidate script sourced" "$RUN" "source candidate/x.sh; $RUN"
widen "a list-form uses"          "      - name: Rehearse the pair" "      - uses: ./.github/actions/x
      - name: Rehearse the pair"
widen "a local action in a step"  "      - name: Rehearse the pair" "      - name: x
        uses: ./.github/actions/x
      - name: Rehearse the pair"
widen "an unpinned action"        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1" "actions/checkout@v7"
widen "a checkout of another repository" "          path: candidate" "          path: candidate
          repository: evil/fork"
widen "persist-credentials true"  "          path: candidate
          sparse-checkout: |" "          path: candidate
          persist-credentials: true
          sparse-checkout: |"
widen "an expression in run:"     "echo \"::error::not a module-v* tag\"" "echo \"\${{ inputs.tag }}\""
widen "no tag check"              "case \"\$TAG\" in module-v*)" "case \"\$TAG\" in *)"
widen "cache-library arguments removed" "                --optimizer-module-bazel optimizer-MODULE.bazel
                --repositories-bzl candidate/bazel/repositories.bzl)" "                )"
widen "run evidence arguments removed" "--run-id \"\$RUN_ID\" --repo \"\$GITHUB_REPOSITORY\" --tag-sha" "--tag-sha"
widen "a cone-mode sparse checkout" "          sparse-checkout-cone-mode: false
          persist-credentials: false

      - name: Checkout the candidate tag" "          persist-credentials: false

      - name: Checkout the candidate tag"
widen "an artifact upload"        "      - name: Rehearse the pair" "      - name: x
        uses: actions/upload-artifact@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7
      - name: Rehearse the pair"
widen "the summary dropped"       "| tee -a \"\$GITHUB_STEP_SUMMARY\"" ""

# The script's own gh calls are reads: a widened copy of it is refused too.
cp "$SCRIPT" "$T/s.sh"; sed -i.bak "s#gh run view \"\$id\"#gh run rerun \"\$id\"#" "$T/s.sh"
if [ -n "$(violations "$WF" "$T/s.sh")" ]; then echo "PASS: refuses a gh write in the script"
else echo "FAIL: did not refuse a gh write in the script"; FAILS=$((FAILS + 1)); fi
cp "$SCRIPT" "$T/s.sh"; sed -i.bak 's#gh api --allow-escape-sequences "repos#gh api -X POST --allow-escape-sequences "repos#' "$T/s.sh"
if [ -n "$(violations "$WF" "$T/s.sh")" ]; then echo "PASS: refuses gh api -X in the script"
else echo "FAIL: did not refuse gh api -X in the script"; FAILS=$((FAILS + 1)); fi

# A missing workflow is a failure, not a pass.
if bash "${BASH_SOURCE[0]}" "$T/none.yml" >/dev/null 2>&1; then echo "FAIL: a missing workflow file passed"; FAILS=$((FAILS + 1))
else echo "PASS: a missing workflow file fails"; fi
[ "$FAILS" -eq 0 ] || { echo "$FAILS failed"; exit 1; }
