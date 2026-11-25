#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Only a v<X>.<Y>.<Z> tag can be released; a prerelease tag (v<X>-pre.<N>),
# a tag with another suffix or with build metadata can be dry-run and nothing
# else. This test asserts, on pipeline-release-nuget.yml, release.yml and the
# other workflows:
#   1. the ref-guard step that decides real or dry run refuses such a tag for
#      a tag push and for a dispatch with dry_run false, writes no output when
#      it refuses, and accepts a dispatch dry run (the step's own script is
#      extracted and executed);
#   2. every job but the first two depends on ref-guard, and each job that
#      runs in the `release` environment has an `if` that needs the job before
#      it to have succeeded (`!cancelled()` jobs ignore `needs` alone; no
#      `always()`; sign-windows and package need real == 'true');
#   3. release.yml is the only caller of the lane and the only workflow with a
#      tag trigger; it passes dry_run through unchanged; no other workflow
#      names an environment or pushes to nuget.org;
#   4. the VERSION.txt guard holds a prerelease dry run to its base version,
#      and only for -pre.<digits>.
# Then it proves the assertions bite by running them on broken copies.
#
# Text matching cannot be complete: this test catches the common ways to widen
# the real path, not every way. The environment match on the nuget.org Trusted
# Publishing policy is the stop that does not depend on this file.
#
# Run: tools/ci/test_nuget_prerelease_refusal.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
W="$(mktemp -d "${TMPDIR:-/tmp}/prerel.XXXXXX")"
trap 'rm -rf "$W"' EXIT
LANE=.github/workflows/pipeline-release-nuget.yml
CALLER=.github/workflows/release.yml

# step_script <file> <step name>: the run: block of a step, dedented.
step_script() {
  python3 -I - "$1" "$2" <<'PY'
import re, sys
lines = open(sys.argv[1]).read().split("\n")
name = sys.argv[2]
for i, l in enumerate(lines):
    if re.match(r"^\s+- (name|id): " + re.escape(name) + r"\s*$", l) or (l.strip() == "- name: " + name):
        break
else:
    sys.exit("step not found: " + name)
indent = len(lines[i]) - len(lines[i].lstrip())
j = i + 1
while j < len(lines) and not re.match(r"^\s*run: \|\s*$", lines[j]):
    if len(lines[j]) - len(lines[j].lstrip()) <= indent and lines[j].strip().startswith("- "): sys.exit("no run block")
    j += 1
body, k = [], j + 1
base = None
while k < len(lines):
    t = lines[k]
    if t.strip() == "":
        body.append(""); k += 1; continue
    ind = len(t) - len(t.lstrip())
    if base is None: base = ind
    if ind < base: break
    body.append(t[base:]); k += 1
print("\n".join(body))
PY
}

# structure <root>: prints violations of 2 and 3.
structure() {
  python3 -I - "$1/$LANE" "$1/$CALLER" "$1/.github/workflows" <<'PY'
import glob, os, re, sys
lane, caller, wfdir = sys.argv[1:4]
errs = []
text = open(lane).read().split("\n")
jobs, cur, injobs = {}, None, False
for l in text:
    if l == "jobs:": injobs = True; continue
    if not injobs: continue
    m = re.match(r"^  [\"']?([A-Za-z_][A-Za-z0-9_-]*)[\"']?:\s*(#.*)?$", l)
    if m: cur = m.group(1); jobs[cur] = {"needs": [], "env": False, "if": "", "text": ""}; continue
    if cur is None: continue
    jobs[cur]["text"] += l + "\n"
    m = re.match(r"^    needs:\s*(.*)$", l)
    if m:
        v = m.group(1).strip()
        if not v: errs.append("%s: block-form needs is not supported by this test" % cur)
        jobs[cur]["needs"] = [x.strip() for x in v.strip("[]").split(",") if x.strip()]
    if re.match(r"^    environment:", l): jobs[cur]["env"] = True
for name, j in jobs.items():  # the job-level if: (single line or folded block)
    ls = j["text"].split("\n")
    for k, l in enumerate(ls):
        if re.match(r"^    if:", l):
            body = l
            for m2 in ls[k + 1:]:
                if m2.startswith("      "): body += " " + m2.strip()
                else: break
            j["if"] = body
def closure(j, seen=None):
    seen = seen if seen is not None else set()
    for n in jobs.get(j, {"needs": []})["needs"]:
        if n not in seen: seen.add(n); closure(n, seen)
    return seen
ENV = ("sign-windows", "package", "sign-checksums", "publish-nuget", "promote-release")
for need in ("event-guard", "ref-guard", "attach-signature") + ENV:
    if need not in jobs: errs.append("job %s not found" % need)
if "event-guard" not in jobs.get("ref-guard", {"needs": []})["needs"]: errs.append("ref-guard does not need event-guard")
envjobs = sorted(j for j in jobs if jobs[j]["env"])
for j in jobs:
    if j in ("event-guard", "ref-guard"): continue
    if "ref-guard" not in closure(j): errs.append("%s does not depend on ref-guard" % j)
    if "always()" in jobs[j]["if"]: errs.append("%s: its if: uses always()" % j)
# The whole condition of every job that runs in the release environment (and of
# attach-signature, which feeds publish-nuget), whitespace-collapsed. A change
# to any of them, including an added || term, must be reviewed together with
# this test.
EXPECT_IF = {
    "sign-windows": "${{ !cancelled() && needs.ref-guard.outputs.real == 'true' && (needs.windows-release-build.result == 'success' || needs.windows-release-build-hosted.result == 'success') }}",
    "package": "${{ !cancelled() && needs.ref-guard.outputs.real == 'true' && needs.ci-release-gate.result == 'success' && needs.security-gates.result == 'success' && needs.sidecar-pack.result == 'success' && (needs.linux-release-tests.result == 'success' || needs.linux-release-tests-hosted.result == 'success') && (needs.windows-release-tests.result == 'success' || needs.windows-release-tests-hosted.result == 'success') && needs.sign-windows.result == 'success' && needs.macos-release-build.result == 'success' && needs.macos-release-tests.result == 'success' && needs.linux-arm64-release-build.result == 'success' && needs.linux-arm64-release-tests.result == 'success' }}",
    "sign-checksums": "${{ !cancelled() && needs.package.result == 'success' }}",
    "attach-signature": "${{ !cancelled() && needs.sign-checksums.result == 'success' && needs.provenance.result == 'success' }}",
    "publish-nuget": "${{ !cancelled() && needs.package.result == 'success' && needs.attach-signature.result == 'success' }}",
    "promote-release": "${{ !cancelled() && needs.publish-nuget.result == 'success' && inputs.auto_promote == true }}",
}
if envjobs != sorted(ENV): errs.append("the jobs naming an environment are %s, expected exactly %s" % (envjobs, sorted(ENV)))
for j, want in EXPECT_IF.items():
    got = re.sub(r"\s+", " ", re.sub(r"^\s*if:\s*(>-\s*)?", "", jobs.get(j, {"if": ""})["if"])).strip()
    if got != want: errs.append("%s: its if: is %r, expected %r" % (j, got, want))
# nuget push and NuGet/login live in publish-nuget and nowhere else in the lane.
def codeonly(t): return "\n".join(l for l in t.split("\n") if not l.lstrip().startswith("#"))
for j, d in jobs.items():
    if j == "publish-nuget": continue
    if re.search(r"nuget\s+push|nuget/login", codeonly(d["text"]), re.I): errs.append("%s: runs nuget push or NuGet/login (only publish-nuget may)" % j)
pn = codeonly(jobs.get("publish-nuget", {"text": ""})["text"])
if not re.search(r"nuget\s+push", pn, re.I) or "nuget/login@" not in pn.lower(): errs.append("publish-nuget: no longer holds nuget push and NuGet/login (test needs updating)")
# publish-nuget writes its steps and env out: no YAML anchor or alias there. The
# other environment jobs may define only the shared package step list and env.
ALLOWED_ANCHORS = {"package-steps", "package-env"}
for j in ENV:
    for m in re.finditer(r"^    [A-Za-z_][A-Za-z0-9_-]*:\s*[&*]([A-Za-z0-9_-]+)", jobs.get(j, {"text": ""})["text"], re.M):
        if j == "publish-nuget" or m.group(1) not in ALLOWED_ANCHORS: errs.append("%s: uses the YAML anchor or alias %s" % (j, m.group(1)))
# publish-nuget has no YAML anchor, alias or merge key anywhere (step-level and
# scalar-level included): nothing it runs can be defined once and reused elsewhere.
ANCHOR = re.compile(r"^\s*(-\s+)?([\w-]+:\s*)?(<<:\s*)?[&*][A-Za-z_][\w-]*(\s|$)", re.M)
for m in ANCHOR.finditer(jobs.get("publish-nuget", {"text": ""})["text"]):
    errs.append("publish-nuget: a YAML anchor, alias or merge key: %s" % m.group(0).strip())
# every environment job names exactly the release environment
for j in ENV:
    if not re.search(r"^    environment:\s*release\s*(#.*)?$", jobs.get(j, {"text": ""})["text"], re.M): errs.append("%s: its environment is not exactly `release`" % j)
# the literal real output of ref-guard, and nothing that could skip or soften the guard
rg = jobs.get("ref-guard", {"text": ""})["text"]
if "\n      real: ${{ steps.mode.outputs.real }}\n" not in "\n" + rg: errs.append("ref-guard: the real output is not exactly steps.mode.outputs.real")
if '            echo "real=$REAL"\n' not in rg: errs.append("ref-guard: the mode step does not write real=$REAL")
for j in ("event-guard", "ref-guard") + ENV:
    jt = jobs.get(j, {"text": ""})["text"]
    if re.search(r"^    continue-on-error:", jt, re.M): errs.append("%s: has continue-on-error" % j)
for j in ("event-guard", "ref-guard"):
    jt = jobs.get(j, {"text": ""})["text"]
    if re.search(r"^    if:", jt, re.M): errs.append("%s: has a job-level if" % j)
    if re.search(r"^(        |      - )(if|continue-on-error):", jt, re.M): errs.append("%s: a step has if: or continue-on-error" % j)
for j in ("vendor", "vendor-hosted"):  # the version step reads REAL_RUN
    if "REAL_RUN: ${{ needs.ref-guard.outputs.real }}" not in jobs.get(j, {"text": ""})["text"]: errs.append("%s: REAL_RUN is not passed to the job" % j)
c = open(caller).read()
if "    tags: ['optimizer-v*', 'v*']" not in c: errs.append("release.yml: the tag trigger changed (test needs updating)")
if len(re.findall(r"uses:\s*\S*pipeline-release-nuget", c)) != 1: errs.append("release.yml: does not call the lane exactly once")
if "pipeline-release-nuget.yml@main" not in c: errs.append("release.yml does not call the lane")
if "      dry_run: ${{ github.event_name == 'workflow_dispatch' && inputs.dry_run }}\n      auto_promote:" not in c: errs.append("release.yml: dry_run is not passed through as dispatch input")
if re.search(r"^\s*environment:", c, re.M): errs.append("release.yml names an environment itself")
LANES = ("release.yml", "pipeline-release-nuget.yml", "pipeline-release-optimizer.yml")
for f in sorted(glob.glob(wfdir + "/*.yml")):
    b, t = os.path.basename(f), open(f).read()
    code = "\n".join(l for l in t.split("\n") if not l.lstrip().startswith("#"))
    if b != "release.yml" and re.search(r"^    tags:", code, re.M): errs.append("%s has a tag trigger" % b)
    if b not in LANES and re.search(r"^\s*environment:", code, re.M): errs.append("%s names an environment" % b)
    if b != "pipeline-release-nuget.yml" and re.search(r"nuget\s+push|nuget/login|api\.nuget\.org/v3/index\.json[^\n]*push", code, re.I):
        errs.append("%s pushes to nuget.org" % b)
    if b not in ("release.yml",) and b != "pipeline-release-nuget.yml" and re.search(r"uses:\s*\S*pipeline-release-nuget", code): errs.append("%s calls the release lane" % b)
print("\n".join(errs))
if not errs: print("jobs in the release environment: " + ", ".join(envjobs), file=sys.stderr)
sys.exit(1 if errs else 0)
PY
}

# modes <root>: step 1.
modes() {
  local root="$1" f="$W/mode.sh" bad=0
  step_script "$root/$LANE" "Real run or dry run" > "$f" 2>"$W/err" || { cat "$W/err"; return 1; }
  [ -s "$f" ] || { echo "mode step not found"; return 1; }
  # the refusal must come before anything is written to the step outputs
  local rl ol
  rl="$(grep -n 'is not a v<X>.<Y>.<Z> tag' "$f" | head -1 | cut -d: -f1)"; ol="$(grep -n 'GITHUB_OUTPUT' "$f" | head -1 | cut -d: -f1)"
  { [ -n "$rl" ] && [ -n "$ol" ] && [ "$rl" -lt "$ol" ]; } || { echo "the refusal does not come before the outputs are written"; bad=1; }
  try() { # try <want ok|no> <event> <dry_run input> <ref> <desc> [real]
    local want="$1" ev="$2" dry="$3" ref="$4" desc="$5" got out
    : > "$W/go"
    if out="$(EVENT_NAME="$ev" DRY_RUN_INPUT="$dry" REF="$ref" SHA=0123456789abcdef0123456789abcdef01234567 GITHUB_OUTPUT="$W/go" bash "$f" 2>&1)"; then got=ok; else got=no; fi
    if [ "$got" != "$want" ]; then echo "$desc: wanted $want, got $got"; bad=1; return; fi
    if [ "$want" = no ] && [ -s "$W/go" ]; then echo "$desc: refused, but outputs were written: $(tr '\n' ' ' < "$W/go")"; bad=1; fi
    [ "$want" = no ] || [ -z "${6:-}" ] || grep -qx "real=$6" "$W/go" || { echo "$desc: expected real=$6, got $(grep real= "$W/go")"; bad=1; }
  }
  local t
  for t in v2.8.0-pre.1 v2.8.0-pre.1+b v2.8.0- v-pre v2.8.0-rc.1 v2.8.0+pre.1 v2.8.0-pre.1x v2.8 v2.8.0.1 vX; do
    try no push ''    "refs/tags/$t" "push of $t"
    try no workflow_dispatch false "refs/tags/$t" "dispatch dry_run false of $t"
  done
  for t in v2.8.0-pre.1 v2.8.0-rc.1 v2.8.0+pre.1; do
    try ok workflow_dispatch true "refs/tags/$t" "dispatch dry run of $t" false
  done
  # dry_run is real only when it is exactly false: an empty or odd value is a dry run
  for t in v2.8.0-pre.1 v2.8.0; do
    try ok workflow_dispatch ''      "refs/tags/$t" "dispatch with an empty dry_run of $t" false
    try ok workflow_dispatch maybe   "refs/tags/$t" "dispatch with dry_run=maybe of $t" false
  done
  try ok push ''    refs/tags/v2.8.0         'push of a release tag' true
  try ok workflow_dispatch false refs/tags/v2.8.0 'dispatch dry_run false of a release tag' true
  try ok workflow_dispatch true  refs/tags/v2.8.0 'dispatch dry run of a release tag' false
  try no push '' refs/heads/main 'push of a branch'
  [ "$bad" = 0 ]
}

# versions <root>: step 4.
versions() {
  local root="$1" f="$W/ver.sh" bad=0
  step_script "$root/$LANE" "Verify VERSION.txt matches the release tag" > "$f" 2>/dev/null
  [ -s "$f" ] || { echo "version step not found"; return 1; }
  tryv() { # tryv <want> <tag> <real> <VERSION.txt> <desc>
    local want="$1" tag="$2" real="$3" ver="$4" desc="$5" got
    mkdir -p "$W/vt"; printf '%s\n' "$ver" > "$W/vt/VERSION.txt"
    if (cd "$W/vt" && RELEASE_TAG="$tag" REAL_RUN="$real" bash "$f" >/dev/null 2>&1); then got=ok; else got=no; fi
    [ "$got" = "$want" ] || { echo "$desc: wanted $want, got $got"; bad=1; }
  }
  tryv ok v2.8.0-pre.1 false 2.8.0 'dry run of -pre.1, VERSION.txt is the base'
  tryv ok v2.8.0-pre.0 false 2.8.0 'dry run of -pre.0'
  tryv ok v2.8.0-pre.10 false 2.8.0 'dry run of -pre.10'
  tryv no v2.8.0-pre.1 false 2.7.0 'dry run of -pre.1, VERSION.txt is older'
  tryv no v2.8.0-pre.1 true  2.8.0 'real run of -pre.1'
  tryv no v2.8.0-rc.1  false 2.8.0 'dry run of -rc.1'
  tryv no v2.8.0-pre.1x false 2.8.0 'dry run of -pre.1x'
  tryv no v2.8.0-pre.1-pre.2 false 2.8.0 'dry run of -pre.1-pre.2'
  tryv no v2.8.0-pre.01 false 2.8.0 'dry run of -pre.01'
  tryv no v2.8.0-pre.  false 2.8.0 'dry run of -pre. with no number'
  tryv no v2.8.0+pre.1 false 2.8.0 'dry run of +pre.1'
  tryv no v2.8.0+pre.1 false '2.8.0+pre.1' 'dry run of +pre.1 whose VERSION.txt names it'
  tryv no v2.8.0+b     true  2.8.0 'real run with build metadata'
  tryv no v2.8.0+b     false 2.8.0 'dry run with build metadata'
  tryv ok v2.8.0       false 2.8.0 'dry run of a release tag'
  tryv ok v2.8.0       true  2.8.0 'real run of a release tag'
  tryv no v2.8.0       true  2.7.0 'real run, VERSION.txt differs'
  [ "$bad" = 0 ]
}

# wants <root>: the sidecar step's WANT rule (stubbed resolve; no network).
wants() {
  local root="$1" f="$W/pin.sh" bad=0
  step_script "$root/$LANE" "The module release of this tag" > "$f" 2>/dev/null
  [ -s "$f" ] || { echo "sidecar pin step not found"; return 1; }
  tryw() { # tryw <want ok|no> <note yes|no> <tag> <real> <resolved module tag> <desc>
    local want="$1" note="$2" tag="$3" real="$4" pin="$5" desc="$6" got has=no d="$W/pinrun"
    rm -rf "$d"; mkdir -p "$d/trust/tools/ci" "$d/tmp"
    printf '#!/bin/bash\necho "%s"\n' "$pin" > "$d/trust/tools/ci/sidecar_fetch.sh"
    : > "$d/env"; : > "$d/out"; : > "$d/sum"
    if (cd "$d" && RELEASE_TAG="$tag" REAL_RUN="$real" SHORT_SHA=abcdef0 GITHUB_ENV="$d/env" GITHUB_OUTPUT="$d/out" GITHUB_STEP_SUMMARY="$d/sum" RUNNER_TEMP="$d/tmp" GH_READ_TOKEN=x bash "$f" >/dev/null 2>&1); then got=ok; else got=no; fi
    [ -f "$d/tmp/sidecar-nupkg/README-SIDECAR-PIN.txt" ] && has=yes
    if [ "$got" != "$want" ]; then echo "$desc: wanted $want, got $got"; bad=1; return; fi
    if [ "$want" = ok ] && [ "$has" != "$note" ]; then echo "$desc: note $has, expected $note"; bad=1; fi
  }
  tryw ok no  v2.8.0-pre.1 false module-v2.8.0 'pre dry run, module-v<X> is the source: no note'
  tryw ok yes v2.8.0-pre.1 false module-v2.7.0 'pre dry run, another release is the source: note'
  tryw ok no  v2.8.0       false module-v2.8.0 'dry run, module-v<X> is the source: no note'
  tryw ok yes v2.8.0       false module-v2.7.0 'dry run, another release: note'
  tryw ok no  v2.8.0       true  module-v2.8.0 'real run, module-v<X>'
  tryw no no  v2.8.0       true  module-v2.7.0 'real run, another release'
  tryw no no  v2.8.0-pre.1 true  module-v2.8.0 'real run of a prerelease tag never takes the base release'
  [ "$bad" = 0 ]
}

all() { local root="$1" n=0 o
  o="$(modes "$root")" || { echo "$o"; n=1; }
  o="$(structure "$root" 2>/dev/null)" || { echo "$o"; n=1; }
  o="$(versions "$root")" || { echo "$o"; n=1; }
  o="$(wants "$root")" || { echo "$o"; n=1; }
  return $n; }

fail=0
if out="$(all "$ROOT")"; then echo "ok   prerelease refusal, job dependencies and version guard hold"; structure "$ROOT" 2>&1 >/dev/null | sed 's/^/     /'; else echo "FAIL"; echo "$out" | sed 's/^/     /'; fail=1; fi

mutate() { # mutate <desc> <file> <python expression over s>; the checks must fail (permit: must pass)
  local want=fail; [ "${MUT_PERMIT:-}" = 1 ] && want=pass
  local d="$W/m"; rm -rf "$d"; mkdir -p "$d/.github/workflows"
  cp "$ROOT"/.github/workflows/*.yml "$d/.github/workflows/"
  python3 -I - "$d/$2" "$3" <<'PY'
import os, re, sys
p, expr = sys.argv[1], sys.argv[2]
s = open(p).read() if os.path.exists(p) else ""
def jobedit(s, job, fn):  # apply fn to the text of one job of the lane
    m = re.search(r"\n  %s:\n.*?(?=\n  [a-z0-9-]+:\n|\Z)" % re.escape(job), s, re.S)
    return s[:m.start()] + fn(m.group(0)) + s[m.end():]
def setif(s, job, new):
    return jobedit(s, job, lambda t: re.sub(r"\n    if:.*?(?=\n    [a-z-]+:)", "\n    if: " + new, t, count=1, flags=re.S))
s2 = eval(expr, {"s": s, "re": re, "jobedit": jobedit, "setif": setif})
open(p, "w").write(s2)
sys.exit(0 if s2 != s else 3)
PY
  [ $? = 0 ] || { echo "FAIL mutation did not change anything: $1"; fail=1; return; }
  local why
  if why="$(all "$d" 2>&1)"; then
    if [ "$want" = pass ]; then echo "ok   allowed: $1"; else echo "FAIL not caught: $1"; fail=1; fi
  else
    if [ "$want" = pass ]; then echo "FAIL wrongly refused: $1  <-  $(printf '%s' "$why" | head -n 1 | cut -c1-150)"; fail=1; else echo "ok   caught: $1  <-  $(printf '%s' "$why" | head -n 1 | cut -c1-150)"; fi
  fi
}
COND='! [[ "${REF#refs/tags/}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]'
mutate 'the refusal removed' $LANE "s.replace('[ \"\$REAL\" = true ] && $COND', 'false')"
mutate 'the refusal limited to dispatch' $LANE "s.replace('[ \"\$REAL\" = true ] && $COND', '[ \"\$EVENT_NAME\" = workflow_dispatch ] && [ \"\$REAL\" = true ] && $COND')"
mutate 'the refusal narrowed to *-pre*' $LANE "s.replace('$COND', '[[ \"\${REF#refs/tags/}\" == *-pre* ]]')"
mutate 'the refusal narrowed to *-pre.*' $LANE "s.replace('$COND', '[[ \"\${REF#refs/tags/}\" == *-pre.* ]]')"
mutate 'the refusal limited to a suffix with a dash' $LANE "s.replace('$COND', '[[ \"\${REF#refs/tags/}\" == *-* ]]')"
mutate 'the refusal written after the outputs' $LANE "(lambda a, b: s[:a] + s[b:].replace('          } >> \"\$GITHUB_OUTPUT\"\n', '          } >> \"\$GITHUB_OUTPUT\"\n' + s[a:b], 1))(s.index('          # Only a v<X>.<Y>.<Z> tag can take a real run.'), s.index('          {\n            echo \"real=\$REAL\"'))"
for j in sign-windows package sign-checksums publish-nuget promote-release; do
  mutate "if: always() on $j" $LANE "setif(s, '$j', 'always()')"
done
mutate 'real == true dropped from package' $LANE "jobedit(s, 'package', lambda t: t.replace(\" && needs.ref-guard.outputs.real == 'true'\", '', 1))"
mutate 'real == true dropped from sign-windows' $LANE "jobedit(s, 'sign-windows', lambda t: t.replace(\" && needs.ref-guard.outputs.real == 'true'\", '', 1))"
mutate 'package no longer needs sign-windows to succeed' $LANE "jobedit(s, 'package', lambda t: t.replace(\"      && needs.sign-windows.result == 'success'\n\", '', 1))"
mutate 'publish-nuget no longer needs package to succeed' $LANE "setif(s, 'publish-nuget', \"\${{ !cancelled() && needs.attach-signature.result == 'success' }}\")"
mutate 'security-gates no longer needs ref-guard' $LANE "s.replace('  security-gates:\n    name: Security Gates (SBOM + Grype + VEX)\n    needs: ref-guard', '  security-gates:\n    name: Security Gates (SBOM + Grype + VEX)\n    needs: event-guard')"
mutate 'publish-nuget needs nothing' $LANE "s.replace('    needs: [ref-guard, package, attach-signature]', '    needs: []')"
mutate 'an environment job without ref-guard' $LANE "s + '\n  extra:\n    runs-on: ubuntu-24.04\n    environment: release\n    steps: []\n'"
mutate 'the dry-run base rule removed' $LANE "s.replace('EXPECTED=\"\${EXPECTED%%-pre.*}\"', ':')"
mutate 'the tag regex widened to any -pre suffix' $LANE "s.replace('(-pre\\\\.(0|[1-9][0-9]*))?\$ ]]; then\n            echo \"::error::release tag', '(-pre\\\\..*)?\$ ]]; then\n            echo \"::error::release tag')"
mutate '-pre.01 allowed' $LANE "s.replace('(0|[1-9][0-9]*)', '[0-9]+')"
mutate 'build metadata allowed in the tag' $LANE "s.replace('^v[0-9]+\\\\.[0-9]+\\\\.[0-9]+(-pre', '^v[0-9]+\\\\.[0-9]+\\\\.[0-9]+(\\\\+[0-9a-z.]+)?(-pre')"
mutate 'the tag regex removed' $LANE "s.replace('if ! [[ \"\$RELEASE_TAG\" =~', 'if false && ! [[ \"\$RELEASE_TAG\" =~')"

mutate 'the base rule applied to real runs too' $LANE "s.replace('if [ \"\$REAL_RUN\" != true ]; then\n            EXPECTED', 'if true; then\n            EXPECTED')"
mutate 'the WANT rule applied to real runs' $LANE "s.replace('if [ \"\$REAL_RUN\" != true ]; then WANT', 'if true; then WANT')"
mutate 'the WANT rule removed' $LANE "s.replace('if [ \"\$REAL_RUN\" != true ]; then WANT=\"module-\${RELEASE_TAG%%-pre.*}\"; fi', ':')"
mutate 'a || term added to the package condition' $LANE "setif(s, 'package', \"\${{ !cancelled() || needs.ref-guard.outputs.real == 'true' }}\")"
mutate '|| failure() added to publish-nuget' $LANE "jobedit(s, 'publish-nuget', lambda t: t.replace(\"needs.attach-signature.result == 'success' }}\", \"needs.attach-signature.result == 'success' || failure() }}\", 1))"
mutate 'a second environment job named' $LANE "jobedit(s, 'attach-signature', lambda t: t.replace('    runs-on: ubuntu-24.04', '    environment: release\n    runs-on: ubuntu-24.04', 1))"
mutate 'an environment job renamed away' $LANE "jobedit(s, 'promote-release', lambda t: t.replace('    environment: release\n', '', 1))"
mutate 'REAL_RUN dropped from vendor' $LANE "jobedit(s, 'vendor', lambda t: t.replace('      REAL_RUN: \${{ needs.ref-guard.outputs.real }}\n', '', 1))"
mutate 'REAL_RUN dropped from vendor-hosted' $LANE "jobedit(s, 'vendor-hosted', lambda t: t.replace('      REAL_RUN: \${{ needs.ref-guard.outputs.real }}\n', '', 1))"
mutate 'another workflow calls the lane' .github/workflows/website.yml "s + '\n  x:\n    uses: We-Amp/pagespeed-optimizer/.github/workflows/pipeline-release-nuget.yml@main\n'"
mutate 'a new workflow on v* tags with a release environment' .github/workflows/extra-release.yml "'name: x\non:\n  push:\n    tags: [\"v*\"]\njobs:\n  go:\n    runs-on: ubuntu-24.04\n    environment: release\n    steps:\n      - run: dotnet nuget push x.nupkg --source https://api.nuget.org/v3/index.json\n'"
mutate 'a new workflow on v* tags that pushes without an environment' .github/workflows/extra-release.yml "'name: x\non:\n  push:\n    tags: [\"v*\"]\njobs:\n  go:\n    runs-on: ubuntu-24.04\n    steps:\n      - run: dotnet nuget push x.nupkg\n'"
mutate 'dry_run: false hard-coded in release.yml' $CALLER "s.replace(\"      dry_run: \${{ github.event_name == 'workflow_dispatch' && inputs.dry_run }}\n      auto_promote:\", '      dry_run: false\n      auto_promote:')"
mutate 'an extra release.yml job calling the lane' $CALLER "s + '\n  again:\n    if: startsWith(github.ref, \"refs/tags/v\")\n    uses: We-Amp/pagespeed-optimizer/.github/workflows/pipeline-release-nuget.yml@main\n    with:\n      dry_run: false\n'"
mutate 'the v* trigger changed' $CALLER "s.replace(\"'v*'\", \"'w*'\")"
mutate 'nuget push added to another job' $LANE "jobedit(s, 'security-gates', lambda t: t.replace('    steps:', '    steps:\\n      - run: dotnet nuget push x.nupkg', 1))"
mutate 'NuGet/login added to another job' $LANE "jobedit(s, 'package', lambda t: t.replace('    steps:', '    steps:\\n      - uses: NuGet/login@v1', 1))"
mutate 'publish-nuget environment changed' $LANE "jobedit(s, 'publish-nuget', lambda t: t.replace('    environment: release', '    environment: staging', 1))"
mutate 'sign-windows environment changed' $LANE "jobedit(s, 'sign-windows', lambda t: t.replace('    environment: release', '    environment: staging', 1))"
mutate 'the real output hard-coded' $LANE "s.replace('      real: \${{ steps.mode.outputs.real }}', \"      real: 'true'\")"
mutate 'the real output read from another step' $LANE "s.replace('      real: \${{ steps.mode.outputs.real }}', '      real: \${{ steps.guard.outputs.real }}')"
mutate 'dry_run compared with != true' $LANE "s.replace('[ \"\$DRY_RUN_INPUT\" = false ]', '[ \"\$DRY_RUN_INPUT\" != true ]')"
mutate 'continue-on-error on the mode step' $LANE "s.replace('        id: mode\n', '        id: mode\n        continue-on-error: true\n')"
mutate 'continue-on-error on the ref-guard job' $LANE "jobedit(s, 'ref-guard', lambda t: t.replace('    runs-on: ubuntu-24.04', '    runs-on: ubuntu-24.04\n    continue-on-error: true', 1))"
mutate 'continue-on-error on the event-guard job' $LANE "jobedit(s, 'event-guard', lambda t: t.replace('    runs-on: ubuntu-24.04', '    runs-on: ubuntu-24.04\n    continue-on-error: true', 1))"
mutate 'continue-on-error on publish-nuget' $LANE "jobedit(s, 'publish-nuget', lambda t: t.replace('    timeout-minutes: 15', '    timeout-minutes: 15\n    continue-on-error: true', 1))"
mutate 'if: false on the mode step' $LANE "s.replace('        id: mode\n', '        id: mode\n        if: false\n')"
mutate 'if on the guard step' $LANE "s.replace('      - id: guard\n', '      - id: guard\n        if: false\n')"
mutate 'a job-level if on ref-guard' $LANE "jobedit(s, 'ref-guard', lambda t: t.replace('    runs-on: ubuntu-24.04', '    runs-on: ubuntu-24.04\n    if: false', 1))"
PUSHJOB='\n  %s:%s\n    needs: ref-guard\n    runs-on: ubuntu-latest\n    steps:\n      - run: dotnet nuget push x.nupkg\n'
mutate 'a pushing job named extra_job after publish-nuget' $LANE "jobedit(s, 'publish-nuget', lambda t: t + '$PUSHJOB' % ('extra_job', ''))"
mutate 'a pushing job named Extra after publish-nuget' $LANE "jobedit(s, 'publish-nuget', lambda t: t + '$PUSHJOB' % ('Extra', ''))"
mutate 'a pushing job with a trailing comment after publish-nuget' $LANE "jobedit(s, 'publish-nuget', lambda t: t + '$PUSHJOB' % ('extra', ' # x'))"
mutate 'nuget/login in lower case in another job' $LANE "jobedit(s, 'security-gates', lambda t: t.replace('    steps:', '    steps:\\n      - uses: nuget/login@v1', 1))"
mutate 'NUGET/LOGIN in upper case in another job' $LANE "jobedit(s, 'security-gates', lambda t: t.replace('    steps:', '    steps:\\n      - uses: NUGET/LOGIN@v1', 1))"
mutate 'a Nuget Push in mixed case in another job' $LANE "jobedit(s, 'security-gates', lambda t: t.replace('    steps:', '    steps:\\n      - run: dotnet Nuget Push x.nupkg', 1))"
mutate 'an anchor on the steps of publish-nuget' $LANE "jobedit(s, 'publish-nuget', lambda t: t.replace('    steps:', '    steps: &pub', 1)) + '\\n  extra:\\n    needs: ref-guard\\n    runs-on: ubuntu-latest\\n    steps: *pub\\n'"
mutate 'a step with if: as its first key on the guard' $LANE "s.replace('      - id: guard\\n', '      - if: false\\n        id: guard\\n')"
mutate 'a step with continue-on-error as its first key' $LANE "s.replace('      - id: guard\\n', '      - continue-on-error: true\\n        id: guard\\n')"
mutate 'an anchor on the push step with an alias in a new job' $LANE "jobedit(s, 'publish-nuget', lambda t: t.replace('      - name: Push packages to nuget.org', '      - &pushstep\\n        name: Push packages to nuget.org', 1)) + '\\n  extra:\\n    needs: ref-guard\\n    runs-on: ubuntu-latest\\n    steps:\\n      - *pushstep\\n'"
mutate 'an anchor on the push command with an alias in a new job' $LANE "jobedit(s, 'publish-nuget', lambda t: t.replace('        run: |\\n          set -euo pipefail\\n          for pkg', '        run: &r |\\n          set -euo pipefail\\n          for pkg', 1)) + '\\n  extra:\\n    needs: ref-guard\\n    runs-on: ubuntu-latest\\n    steps:\\n      - run: *r\\n'"
mutate 'a quoted job name after publish-nuget' $LANE "jobedit(s, 'publish-nuget', lambda t: t + '\\n  \\x22extra\\x22:\\n    needs: ref-guard\\n    runs-on: ubuntu-latest\\n    steps:\\n      - run: dotnet nuget push x.nupkg\\n')"
mutate 'a single-quoted job name after publish-nuget' $LANE "jobedit(s, 'publish-nuget', lambda t: t + \"\\n  'extra':\\n    needs: ref-guard\\n    runs-on: ubuntu-latest\\n    steps:\\n      - run: dotnet nuget push x.nupkg\\n\")"
MUT_PERMIT=1 mutate 'a trailing comment on an environment line' $LANE "jobedit(s, 'publish-nuget', lambda t: t.replace('    environment: release', '    environment: release # the protected environment', 1))"
[ "$fail" = 0 ] && echo PASS || { echo FAILED; exit 1; }
