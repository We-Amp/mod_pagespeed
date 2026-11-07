#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Pair rehearsal: before the release tag, check that the module and the
# optimizer of the candidate are one pair, with the checks the real tag runs.
#
# A `module-vX.Y.Z-pre.N` dry run of the release lane never runs the version
# pair check (its optimizer tag, optimizer-vX.Y.Z-pre.N, never equals the pin,
# so the lane passes --dry-run and the guard ignores a -pre tag). A pin that
# names the previous optimizer, or a pinned commit whose VERSION.txt is the
# previous number, then shows only on the real tag. This applies the check as
# the real tag will: the rehearsed version without its -pre suffix, no
# --dry-run.
#
#   tools/ci/pair_rehearsal.sh --tag TAG --pin FILE --optimizer-version-file FILE
#       [--optimizer-module-bazel FILE --repositories-bzl FILE]
#       [--run-id ID --repo OWNER/NAME --tag-sha SHA]
#       [--candidate-run-id ID --repo OWNER/NAME --tag-sha SHA]
#   tools/ci/pair_rehearsal.sh --resolve-candidate --tag TAG --run-id ID
#       --repo OWNER/NAME --out FILE
#   tools/ci/pair_rehearsal.sh --self-test
#
# Checks (every one must pass; the exit status is the number of failures
# capped at 1, and all failures are printed):
#   1. TAG is module-vX.Y.Z[-pre.N] and, for a -pre tag, is judged as the
#      release X.Y.Z it rehearses.
#   2. The pin's tag is optimizer-vX.Y.Z (the release's own optimizer).
#   3. The pinned optimizer commit's VERSION.txt agrees with X.Y.Z
#      (check_module_optimizer_version.py, strict).
#   4. With the optimizer's MODULE.bazel: both halves link one cache library
#      commit (check_optimizer_pin_pair.py).
#   5. With --run-id (the id of a dry run of the release lane, read through
#      `gh run view` and the log of its version job; read only): the run is
#      the "Release" workflow (its file, .github/workflows/release.yml, is
#      read too, since a name is only a display string), completed and successful, a dispatch; its
#      version job says it was a dry run of exactly --tag at exactly the
#      tag's commit (--tag-sha), so a run of another tag, or of the same tag
#      before it moved, is refused; the optimizer fetch and, per package kind
#      (Debian, RPM), the install-and-serve smoke jobs have one `success` and
#      the other variant `skipped`; "Sign SHA256SUMS" and "Create the GitHub
#      release" exist and are exactly `skipped`. Without --run-id these
#      checks are not made, and the output says so.
#
#   6. With --candidate-run-id instead of --run-id (TAG is a final release
#      X.Y.Z or X.Y.Z-rc.N whose tag does not exist yet; the evidence is a
#      candidate run of the release lane, read through the Actions API): the
#      run is of this repository, of .github/workflows/release.yml, a
#      dispatch on master, completed and successful, titled exactly
#      "Release TAG candidate dry run @ HEAD_SHA", and its candidate-seal job
#      (named "Seal the candidate artifacts") concluded success. Its head_sha
#      is the commit that was built, and --tag-sha must be that commit: the
#      workflow checks out the commit --resolve-candidate printed, never a
#      tag. --resolve-candidate makes only this verification and writes
#      head_sha=<40 hex> to --out.
#
# It reads files and prints; it publishes nothing and needs no secret.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

FAILS=0
bad() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
ok() { echo "ok: $*"; }

# check_run RUN_JSON DERIVE_LOG TAG TAG_SHA WORKFLOW_PATH
check_run() {
  python3 -I - "$1" "$2" "$3" "$4" "$5" <<'PY'
import json, re, sys
doc = json.load(open(sys.argv[1]))
log = re.sub(r"\x1b\[[0-9;]*m", "", open(sys.argv[2]).read())
want_tag, want_sha, path = sys.argv[3], sys.argv[4], sys.argv[5]
errs = []
if path != ".github/workflows/release.yml":
    errs.append("run is of workflow file %r, not .github/workflows/release.yml" % path)
if doc.get("workflowName") != "Release":
    errs.append("run is workflow %r, not the Release workflow" % doc.get("workflowName"))
if doc.get("status") != "completed" or doc.get("conclusion") != "success":
    errs.append("run is %r/%r, it must be completed/success"
                % (doc.get("status"), doc.get("conclusion")))
if doc.get("event") != "workflow_dispatch":
    errs.append("run event is %r, not a dispatch (a pushed tag is a real run)" % doc.get("event"))
# Which tag the run rehearsed: the version job prints its derived values.
got = {}
for m in re.finditer(r"^\S*\s*(tag|sha|dry_run)=(\S+)\s*$", log, re.M):
    got.setdefault(m.group(1), m.group(2))
for k in ("tag", "sha", "dry_run"):
    if k not in got:
        errs.append("cannot establish %s of the run from its version job" % k)
if "tag" in got and got["tag"] != want_tag:
    errs.append("the run rehearsed %s, not %s" % (got["tag"], want_tag))
if "sha" in got and got["sha"] != want_sha:
    errs.append("the run built %s, the tag is at %s" % (got["sha"], want_sha))
if "dry_run" in got and got["dry_run"] != "true":
    errs.append("the run says dry_run=%s, it is not a dry run" % got["dry_run"])

jobs = doc.get("jobs", [])
def named(pattern):
    return [j for j in jobs if re.fullmatch(pattern, j.get("name", ""))]
PFX = r"(?:[^/]+ / )?"
# The names of these two jobs carry the adoption binding in an adopting run.
# GitHub lists a job that was skipped under its raw, unevaluated name (the
# `${{ ... }}` text), and a job that ran under the evaluated one, so accept
# the plain name, the evaluated suffix and the raw expression.
ADOPTED_SUFFIX = r"(?: \(adopted: run [0-9]+ \S+ seal [0-9a-f]{12} \S+ SHA256SUMS [0-9a-f]{12}\)|\$\{\{.*\}\})?"
one = [
    ("optimizer fetch", PFX + r"Fetch and verify the optimizer packages"),
]
for label, pat in one:
    found = named(pat)
    if len(found) != 1:
        errs.append("%s: expected one job, found %d" % (label, len(found)))
    for j in found:
        if j.get("conclusion") != "success":
            errs.append("%s: %r is %r, it must be success" % (label, j["name"], j.get("conclusion")))
# One job per runner kind: exactly one variant ran, the other was skipped.
for label, pat in [
    ("Debian install and serve", PFX + r"Smoke Test \(Debian(?:, hosted)?\)"),
    ("RPM install and serve", PFX + r"Smoke Test \(RPM(?:, hosted)?\)"),
]:
    found = named(pat)
    if not found:
        errs.append("%s: no such job in the run" % label)
        continue
    ok = [j for j in found if j.get("conclusion") == "success"]
    rest = [j for j in found if j.get("conclusion") != "success"]
    if len(ok) != 1:
        errs.append("%s: %d variants passed (%s), exactly one must"
                    % (label, len(ok), ", ".join("%s=%s" % (j["name"], j.get("conclusion")) for j in found)))
    for j in rest:
        if j.get("conclusion") != "skipped":
            errs.append("%s: %r is %r, it must be success or skipped"
                        % (label, j["name"], j.get("conclusion")))
# A dry run signs and publishes nothing: these two jobs must exist and be skipped.
for label, pat in [
    # See ADOPTED_SUFFIX: optional, evaluated or raw.
    ("signing", PFX + r"Sign SHA256SUMS" + ADOPTED_SUFFIX),
    ("release creation", PFX + r"Create the GitHub release" + ADOPTED_SUFFIX),
]:
    found = named(pat)
    if len(found) != 1:
        errs.append("%s: expected one job, found %d" % (label, len(found)))
    for j in found:
        if j.get("conclusion") != "skipped":
            errs.append("%s: %r is %r, it must be exactly skipped (this is not a dry run)"
                        % (label, j["name"], j.get("conclusion")))
for e in errs:
    print("FAIL: " + e)
if not errs:
    print("attempt %s, tag %s at %s" % (doc.get("attempt", "?"), got["tag"], got["sha"]))
sys.exit(1 if errs else 0)
PY
}

# fetch_run ID REPO RUN_JSON DERIVE_LOG PATH_FILE: read-only reads of one run.
fetch_run() {
  local id="$1" repo="$2" rj="$3" dl="$4" pf="$5" jid
  gh run view "$id" -R "$repo" --json attempt,status,conclusion,event,workflowName,jobs > "$rj" 2>&1 \
    || { echo "FAIL: cannot read run $id: $(head -c 300 "$rj")"; return 1; }
  jid="$(python3 -I -c "
import json,re,sys
for j in json.load(open(sys.argv[1])).get('jobs', []):
    if re.fullmatch(r'(?:[^/]+ / )?Derive versions from the tag', j.get('name', '')):
        print(j['databaseId']); break
" "$rj")"
  gh api "repos/$repo/actions/runs/$id" --jq .path > "$pf" 2>&1 \
    || { echo "FAIL: cannot read the workflow file of run $id: $(head -c 300 "$pf")"; return 1; }
  [ -n "$jid" ] || { echo "FAIL: run $id has no version job, cannot establish its tag"; return 1; }
  gh api --allow-escape-sequences "repos/$repo/actions/jobs/$jid/logs" > "$dl" 2>&1 \
    || { echo "FAIL: cannot read the version job log of run $id: $(head -c 300 "$dl")"; return 1; }
}

# check_candidate RUN_JSON JOBS_JSONL REPO TAG: prints FAIL lines, or the
# head_sha of the verified candidate run alone.
check_candidate() {
  python3 -I - "$1" "$2" "$3" "$4" <<'PY'
import json, re, sys
doc = json.load(open(sys.argv[1]))
jobs = [json.loads(l) for l in open(sys.argv[2]) if l.strip()]
repo, tag = sys.argv[3], sys.argv[4]
errs = []
if not re.fullmatch(r"module-v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-rc\.[0-9]+)?", tag):
    errs.append("a candidate is of module-vX.Y.Z or module-vX.Y.Z-rc.N, not %r" % tag)
for key in ("repository", "head_repository"):
    got = (doc.get(key) or {}).get("full_name")
    if got != repo:
        errs.append("the run's %s is %r, not %r" % (key, got, repo))
path = (doc.get("path") or "").partition("@")[0]
if path != ".github/workflows/release.yml":
    errs.append("run is of workflow file %r, not .github/workflows/release.yml" % doc.get("path"))
if doc.get("event") != "workflow_dispatch":
    errs.append("run event is %r, not a dispatch" % doc.get("event"))
if doc.get("head_branch") != "master":
    errs.append("run head_branch is %r, not master" % doc.get("head_branch"))
if doc.get("status") != "completed" or doc.get("conclusion") != "success":
    errs.append("run is %r/%r, it must be completed/success" % (doc.get("status"), doc.get("conclusion")))
sha = doc.get("head_sha")
if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{40}", sha):
    errs.append("run head_sha %r is not 40 lowercase hex digits" % (sha,))
    sha = ""
want = "Release %s candidate dry run @ %s" % (tag, sha)
if doc.get("display_title") != want:
    errs.append("run title is %r, expected %r" % (doc.get("display_title"), want))
seal = [j for j in jobs if re.fullmatch(r"(?:[^/]+ / )?Seal the candidate artifacts", j.get("name", ""))]
if len(seal) != 1:
    errs.append("expected one candidate-seal job, found %d" % len(seal))
for j in seal:
    if j.get("conclusion") != "success":
        errs.append("candidate-seal job %r is %r, it must be success" % (j["name"], j.get("conclusion")))
for e in errs:
    print("FAIL: " + e)
if not errs:
    print(sha)
sys.exit(1 if errs else 0)
PY
}

# fetch_candidate ID REPO RUN_JSON JOBS_JSONL: read-only reads of one run.
fetch_candidate() {
  local id="$1" repo="$2" rj="$3" jl="$4"
  gh api "repos/$repo/actions/runs/$id" > "$rj" 2>&1 \
    || { echo "FAIL: cannot read run $id: $(head -c 300 "$rj")"; return 1; }
  gh api --paginate "repos/$repo/actions/runs/$id/jobs?per_page=100" --jq '.jobs[]' > "$jl" 2>&1 \
    || { echo "FAIL: cannot read the jobs of run $id: $(head -c 300 "$jl")"; return 1; }
}

# verify_candidate ID REPO TAG: prints the verified head_sha, or FAIL lines.
verify_candidate() {
  local id="$1" repo="$2" tag="$3" w rc=0 out
  w="$(mktemp -d)"
  if out="$(fetch_candidate "$id" "$repo" "$w/run.json" "$w/jobs.jsonl")"; then
    out="$(check_candidate "$w/run.json" "$w/jobs.jsonl" "$repo" "$tag" 2>&1)" || rc=1
  else rc=1; fi
  rm -rf "$w"
  printf '%s\n' "$out"
  return "$rc"
}

# resolve_candidate --tag T --run-id ID --repo R --out FILE
resolve_candidate() {
  local tag="" id="" repo="" outf=""
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || { echo "pair_rehearsal: $1 needs a value" >&2; return 2; }
    case "$1" in
      --tag) tag="$2" ;; --run-id) id="$2" ;; --repo) repo="$2" ;; --out) outf="$2" ;;
      *) echo "pair_rehearsal: unknown argument '$1'" >&2; return 2 ;;
    esac
    shift 2
  done
  [ -n "$tag" ] && [ -n "$id" ] && [ -n "$repo" ] && [ -n "$outf" ] || {
    echo "pair_rehearsal: --resolve-candidate needs --tag, --run-id, --repo and --out" >&2; return 2; }
  local res
  if res="$(verify_candidate "$id" "$repo" "$tag")"; then
    printf 'head_sha=%s\n' "$res" >> "$outf"
    echo "ok: candidate run $id verified, built $res"
  else
    printf '%s\n' "$res"; return 1
  fi
}

rehearse() {
  local tag="" pin="" vfile="" mbazel="" repos="" runid="" repo="" tagsha="" candrun=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --tag|--pin|--optimizer-version-file|--optimizer-module-bazel|--repositories-bzl|--run-id|--candidate-run-id|--repo|--tag-sha)
        [ $# -ge 2 ] || { echo "pair_rehearsal: $1 needs a value" >&2; return 2; } ;;
    esac
    case "$1" in
      --tag) tag="$2"; shift 2 ;;
      --pin) pin="$2"; shift 2 ;;
      --optimizer-version-file) vfile="$2"; shift 2 ;;
      --optimizer-module-bazel) mbazel="$2"; shift 2 ;;
      --repositories-bzl) repos="$2"; shift 2 ;;
      --run-id) runid="$2"; shift 2 ;;
      --candidate-run-id) candrun="$2"; shift 2 ;;
      --repo) repo="$2"; shift 2 ;;
      --tag-sha) tagsha="$2"; shift 2 ;;
      *) echo "pair_rehearsal: unknown argument '$1'" >&2; return 2 ;;
    esac
  done
  [ -n "$tag" ] && [ -f "$pin" ] && [ -f "$vfile" ] || {
    echo "pair_rehearsal: --tag, --pin and --optimizer-version-file are required" >&2; return 2; }

  local parsed version base pre judged
  parsed="$(bash "$HERE/release_version.sh" parse "$tag" 2>&1)" || { bad "tag: $parsed"; return 1; }
  version="$(printf '%s\n' "$parsed" | sed -n 's/^version=//p')"
  base="$(printf '%s\n' "$parsed" | sed -n 's/^base=//p')"
  pre="$(printf '%s\n' "$parsed" | sed -n 's/^prerelease=//p')"
  judged="$version"
  case "$pre" in pre|pre.*|pre[0-9]*) judged="$base" ;; esac
  ok "tag $tag is judged as release $judged"

  local pin_tag want_tag
  pin_tag="$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1])).get('tag',''))" "$pin")"
  want_tag="optimizer-v${judged}"
  if [ "$pin_tag" = "$want_tag" ]; then ok "pin names $pin_tag"
  else bad "the pin names '$pin_tag' but release $judged needs '$want_tag'"; fi

  local out
  if out="$(python3 -I "$HERE/check_module_optimizer_version.py" \
        --module-version "$judged" --optimizer-version-file "$vfile" 2>&1)"; then
    ok "version: ${out##*$'\n'}"
  else
    bad "version: ${out}"
  fi

  if [ -n "$mbazel" ]; then
    if out="$(python3 -I "$HERE/check_optimizer_pin_pair.py" "$pin" "$mbazel" ${repos:+"$repos"} 2>&1)"; then
      ok "cache library: ${out##*$'\n'}"
    else
      bad "cache library: ${out}"
    fi
  fi

  if [ -n "$candrun" ]; then
    [ -z "$runid" ] || { echo "pair_rehearsal: --run-id and --candidate-run-id are exclusive" >&2; return 2; }
    [ -n "$tagsha" ] && [ -n "$repo" ] || { echo "pair_rehearsal: --candidate-run-id needs --repo and --tag-sha" >&2; return 2; }
    local cres
    if ! cres="$(verify_candidate "$candrun" "$repo" "$tag")"; then
      printf '%s\n' "$cres"; FAILS=$((FAILS + 1))
    elif [ "$cres" != "$tagsha" ]; then
      bad "the candidate run built $cres, the checked-out commit is $tagsha"
    else
      ok "candidate run $candrun, sealed, built $cres"
    fi
  elif [ -n "$runid" ]; then
    [ -n "$tagsha" ] && [ -n "$repo" ] || { echo "pair_rehearsal: --run-id needs --repo and --tag-sha" >&2; return 2; }
    local w rj dl pf
    w="$(mktemp -d)"; rj="$w/run.json"; dl="$w/derive.txt"; pf="$w/path.txt"
    if ! out="$(fetch_run "$runid" "$repo" "$rj" "$dl" "$pf")"; then
      printf '%s\n' "$out"; FAILS=$((FAILS + 1))
    elif out="$(check_run "$rj" "$dl" "$tag" "$tagsha" "$(tr -d '\n' < "$pf")" 2>&1)"; then
      ok "dry run $runid, install and serve passed: $out"
    else
      printf '%s\n' "$out"; FAILS=$((FAILS + 1))
    fi
    rm -rf "$w"
  else
    echo "NOT CHECKED: no dry run id given, so the install-and-serve jobs and the dry run's tag were not read"
  fi
  [ "$FAILS" -eq 0 ]
}

self_test() {
  local t rc fails=0
  t="$(mktemp -d)"; trap 'rm -rf "$t"' RETURN
  pin() { printf '{"tag":"%s","optimizer_commit":"%s","sha256sums_sha256":"x"}\n' "$1" "$(printf 'a%.0s' $(seq 40))" > "$t/pin.json"; }
  ver() { printf '%s\n' "$1" > "$t/VERSION.txt"; }
  expect() { # want-rc name args...
    local want="$1" name="$2"; shift 2
    FAILS=0
    (rehearse "$@") >"$t/out" 2>&1; rc=$?
    if [ "$rc" -eq "$want" ]; then echo "  ok   $name"
    else echo "  FAIL $name (rc=$rc, want $want)"; sed 's/^/        /' "$t/out"; fails=$((fails + 1)); fi
  }
  local base=(--pin "$t/pin.json" --optimizer-version-file "$t/VERSION.txt")

  pin optimizer-v2.8.0; ver 2.8.0
  expect 0 "matched pair, -pre tag"            --tag module-v2.8.0-pre.1 "${base[@]}"
  expect 0 "matched pair, final tag"           --tag module-v2.8.0 "${base[@]}"
  # Teeth: each half of a mismatched pair fails a -pre rehearsal, which the
  # lane's own dry run passes without looking.
  pin optimizer-v2.7.0; ver 2.8.0
  expect 1 "TEETH pin names the old optimizer" --tag module-v2.8.0-pre.1 "${base[@]}"
  pin optimizer-v2.8.0; ver 2.7.0
  expect 1 "TEETH VERSION.txt is the old number" --tag module-v2.8.0-pre.1 "${base[@]}"
  pin optimizer-v2.7.0; ver 2.7.0
  expect 1 "TEETH both halves one release behind" --tag module-v2.8.0-pre.2 "${base[@]}"
  pin optimizer-v2.8.0; ver 2.8.0
  expect 1 "bad tag"                           --tag v2.8.0 "${base[@]}"
  expect 2 "missing input"                     --tag module-v2.8.0-pre.1
  # The lane's own guard passes the mismatch the rehearsal refuses.
  ver 2.7.0
  if python3 -I "$HERE/check_module_optimizer_version.py" --module-version 2.8.0-pre.1 \
       --optimizer-version-file "$t/VERSION.txt" --dry-run >/dev/null 2>&1; then
    echo "  ok   the lane's dry-run guard skips the same mismatch"
  else echo "  FAIL the lane's dry-run guard no longer skips -pre"; fails=$((fails + 1)); fi

  # Cache library: both halves must link one commit.
  pin optimizer-v2.8.0; ver 2.8.0
  local c1 c2
  c1="$(printf 'b%.0s' $(seq 40))"; c2="$(printf 'c%.0s' $(seq 40))"
  printf 'CYCLONE_COMMIT = "%s"\n' "$c1" > "$t/repositories.bzl"
  printf 'x(\n    name = "cyclone",\n    commit = "%s",\n)\n' "$c1" > "$t/MODULE.bazel"
  expect 0 "same cache library commit"         --tag module-v2.8.0-pre.1 "${base[@]}" --optimizer-module-bazel "$t/MODULE.bazel" --repositories-bzl "$t/repositories.bzl"
  printf 'x(\n    name = "cyclone",\n    commit = "%s",\n)\n' "$c2" > "$t/MODULE.bazel"
  expect 1 "TEETH different cache library commit" --tag module-v2.8.0-pre.1 "${base[@]}" --optimizer-module-bazel "$t/MODULE.bazel" --repositories-bzl "$t/repositories.bzl"

  # Arguments: a flag without a value is refused, not looped on.
  expect 2 "flag without a value"              --tag
  expect 2 "run id without repo"               --tag module-v2.8.0-pre.1 "${base[@]}" --run-id 1 --tag-sha x

  # Run evidence: read-only gh calls against real runs of the release lane
  # (job lists and version-job lines of two dry runs, kept in testdata).
  local fx="$HERE/testdata/pair-rehearsal" shim="$t/shim"
  mkdir -p "$shim"
  cat > "$shim/gh" <<'SH'
#!/bin/sh
echo "$*" >> "$GH_CALLS"
case "$1" in
  run) [ "$2" = view ] || exit 9; cat "$FIX_RUN" ;;
  api) case "$*" in *-X*|*--method*|*" -f "*|*" -F "*|*--input*) exit 9 ;; esac
       case "$*" in */actions/runs/*) echo "${FIX_PATH:-.github/workflows/release.yml}" ;; *) cat "$FIX_LOG" ;; esac ;;
  *) exit 9 ;;
esac
SH
  chmod +x "$shim/gh"
  export GH_CALLS="$t/gh-calls"
  : > "$GH_CALLS"
  withrun() { FIX_RUN="$1" FIX_LOG="$2" PATH="$shim:$PATH" expect "${@:3}"; }
  local r1=41232127414 r2=40700548438
  local sha1=170368c80c3f1beacc9a93b40e78e8cc78c93447 sha2=da5e9a5c91666e10e38a250708726ca3dcb30542
  local ev1=(--run-id "$r1" --repo o/r --tag-sha "$sha1") ev2=(--run-id "$r2" --repo o/r --tag-sha "$sha2")
  pin optimizer-v2.7.0; ver 2.7.0
  withrun "$fx/run-$r1.json" "$fx/derive-$r1.txt" 0 "real dry run of 2.7.0-pre.1 (default jobs ran, hosted variants skipped)" \
    --tag module-v2.7.0-pre.1 "${base[@]}" "${ev1[@]}"
  pin optimizer-v2.6.0; ver 2.6.0
  withrun "$fx/run-$r2.json" "$fx/derive-$r2.txt" 0 "real dry run of 2.6.0-pre.2" \
    --tag module-v2.6.0-pre.2 "${base[@]}" "${ev2[@]}"
  # The tag the run rehearsed must be the tag asked for.
  withrun "$fx/run-$r2.json" "$fx/derive-$r2.txt" 1 "TEETH run of pre.2 offered for pre.1" \
    --tag module-v2.6.0-pre.1 "${base[@]}" "${ev2[@]}"
  withrun "$fx/run-$r2.json" "$fx/derive-$r2.txt" 1 "TEETH run built another commit" \
    --tag module-v2.6.0-pre.2 "${base[@]}" --run-id "$r2" --repo o/r --tag-sha "$sha1"
  # Mutations of the real fixtures.
  mut() { # name want python-expression-on-d(run) [log-sed]
    python3 -I - "$fx/run-$r1.json" "$t/mut.json" "$3" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
def job(n):
    return [j for j in d["jobs"] if j["name"] == n][0]
exec(sys.argv[3])
json.dump(d, open(sys.argv[2], "w"))
PY
    local log="$fx/derive-$r1.txt"
    if [ -n "${4:-}" ]; then sed "$4" "$log" > "$t/mut.txt"; log="$t/mut.txt"; fi
    pin optimizer-v2.7.0; ver 2.7.0
    withrun "$t/mut.json" "$log" "$2" "$1" --tag module-v2.7.0-pre.1 "${base[@]}" "${ev1[@]}"
  }
  mut "failed run"                         1 'd["conclusion"]="failure"'
  mut "run not completed"                  1 'd["status"]="in_progress"; d["conclusion"]=""'
  mut "pushed run"                         1 'd["event"]="push"'
  mut "another workflow"                   1 'd["workflowName"]="Nightly"'
  mut "Debian passed on both runners"      1 'job("module / Smoke Test (Debian, hosted)")["conclusion"]="success"'
  mut "Debian skipped on both runners"     1 'job("module / Smoke Test (Debian)")["conclusion"]="skipped"'
  mut "Debian failed, hosted skipped"      1 'job("module / Smoke Test (Debian)")["conclusion"]="failure"'
  mut "RPM variant failed beside a pass"   1 'job("module / Smoke Test (RPM, hosted)")["conclusion"]="failure"'
  mut "no Debian job"                      1 'd["jobs"]=[j for j in d["jobs"] if "(Debian" not in j["name"]]'
  mut "optimizer fetch skipped"            1 'job("module / Fetch and verify the optimizer packages")["conclusion"]="skipped"'
  mut "signing ran"                        1 'job("module / Sign SHA256SUMS")["conclusion"]="success"'
  mut "release job empty conclusion"       1 'job("module / Create the GitHub release")["conclusion"]=""'
  mut "release job waiting"                1 'job("module / Create the GitHub release")["conclusion"]=None'
  mut "no signing job"                     1 'd["jobs"]=[j for j in d["jobs"] if j["name"]!="module / Sign SHA256SUMS"]'
  mut "no release job"                     1 'd["jobs"]=[j for j in d["jobs"] if "Create the GitHub" not in j["name"]]'
  # Since adoption the two names carry an expression; a skipped job is listed
  # under the raw text (as in a real dry run on master).
  mut "raw expression names of the skipped jobs" 0 \
    'raw="${{ (((needs.adopt.outputs.adopted == '"'"'true'"'"') && format('"'"' (adopted: {0})'"'"', needs.adopt.outputs.binding)) || '"'"''"'"') }}"; job("module / Sign SHA256SUMS")["name"]+=raw; job("module / Create the GitHub release")["name"]+=raw'
  # A real dispatch (dry_run=false) whose release gate is waiting at the environment.
  mut "TEETH real dispatch at the release environment" 1 \
    'job("module / Create the GitHub release")["conclusion"]=None; job("module / Sign SHA256SUMS")["conclusion"]=None' \
    's/dry_run=true/dry_run=false/'
  FIX_PATH=.github/workflows/lookalike.yml mut "TEETH look-alike workflow named Release" 1 'pass'
  mut "version job lines missing"          1 'pass' '/ tag=/d'
  # The run is read, not changed: only run view and api reads were made.
  if ! grep -v -E '^(run view |api --allow-escape-sequences repos/o/r/actions/jobs/[0-9]+/logs$|api repos/o/r/actions/runs/[0-9]+ --jq \.path$)' "$GH_CALLS" | grep -q .; then
    echo "  ok   only read calls were made ($(wc -l < "$GH_CALLS" | tr -d ' ') gh calls)"
  else echo "  FAIL a non-read gh call was made"; grep -v -E '^(run view |api )' "$GH_CALLS"; fails=$((fails + 1)); fi
  # Candidate evidence: a candidate run of a final tag that does not exist yet,
  # read through the Actions API. The run record and the job list are built
  # here (the shim answers the two reads), then mutated one field at a time.
  local cshim="$t/cshim" csha=0123456789abcdef0123456789abcdef01234567
  mkdir -p "$cshim"
  cat > "$cshim/gh" <<'SH'
#!/bin/sh
echo "$*" >> "$GH_CALLS"
[ "$1" = api ] || exit 9
case "$*" in *-X*|*--method*|*" -f "*|*" -F "*|*--input*) exit 9 ;; esac
case "$*" in
  *"/actions/runs/777/jobs"*) cat "$FIX_CJOBS" ;;
  *"/actions/runs/777") cat "$FIX_CRUN" ;;
  *) exit 9 ;;
esac
SH
  chmod +x "$cshim/gh"
  cand() { # name want tag python-on-d-and-jobs [tag-sha]
    python3 -I - "$t/crun.json" "$t/cjobs.jsonl" "$csha" "$3" "$4" <<'PY'
import json, sys
sha, tag = sys.argv[3], sys.argv[4]
d = {"id": 777, "repository": {"full_name": "o/r"}, "head_repository": {"full_name": "o/r"},
     "path": ".github/workflows/release.yml", "event": "workflow_dispatch", "head_branch": "master",
     "status": "completed", "conclusion": "success", "head_sha": sha,
     "display_title": "Release %s candidate dry run @ %s" % (tag, sha)}
jobs = [{"name": "module / Seal the candidate artifacts", "conclusion": "success"},
        {"name": "module / Validate", "conclusion": "success"}]
exec(sys.argv[5])
json.dump(d, open(sys.argv[1], "w"))
open(sys.argv[2], "w").write("".join(json.dumps(j) + "\n" for j in jobs))
PY
    FIX_CRUN="$t/crun.json" FIX_CJOBS="$t/cjobs.jsonl" PATH="$cshim:$PATH" \
      expect "$2" "$1" --tag "$3" "${base[@]}" --candidate-run-id 777 --repo o/r --tag-sha "${5:-$csha}"
  }
  pin optimizer-v2.8.0; ver 2.8.0
  : > "$GH_CALLS"
  cand "candidate run of a final tag" 0 module-v2.8.0 'pass'
  cand "candidate run, title built from the same sha" 0 module-v2.8.0 'd["head_sha"]=d["head_sha"]'
  cand "TEETH head_branch is not master"    1 module-v2.8.0 'd["head_branch"]="feature"'
  cand "TEETH a pushed run"                 1 module-v2.8.0 'd["event"]="push"'
  cand "TEETH another workflow file"        1 module-v2.8.0 'd["path"]=".github/workflows/lookalike.yml"'
  cand "TEETH title of another tag"         1 module-v2.8.0 'd["display_title"]=d["display_title"].replace("2.8.0","2.7.0")'
  cand "TEETH title without candidate"      1 module-v2.8.0 'd["display_title"]=d["display_title"].replace(" candidate","")'
  cand "TEETH title names another sha"      1 module-v2.8.0 'd["display_title"]=d["display_title"][:-1]+"0"'
  cand "TEETH seal job failed"              1 module-v2.8.0 'jobs[0]["conclusion"]="failure"'
  cand "TEETH seal job skipped"             1 module-v2.8.0 'jobs[0]["conclusion"]="skipped"'
  cand "TEETH no seal job"                  1 module-v2.8.0 'jobs.pop(0)'
  cand "TEETH two seal jobs"                1 module-v2.8.0 'jobs.append(dict(jobs[0]))'
  cand "TEETH run of another repository"    1 module-v2.8.0 'd["repository"]["full_name"]="x/y"'
  cand "TEETH run from a fork"              1 module-v2.8.0 'd["head_repository"]["full_name"]="x/y"'
  cand "TEETH run not completed"            1 module-v2.8.0 'd["status"]="in_progress"; d["conclusion"]=None'
  cand "TEETH run failed"                   1 module-v2.8.0 'd["conclusion"]="failure"'
  cand "TEETH head_sha is not a sha"        1 module-v2.8.0 'd["head_sha"]="master"; d["display_title"]="Release module-v2.8.0 candidate dry run @ master"'
  cand "TEETH checked-out commit is not the built commit" 1 module-v2.8.0 'pass' fedcba9876543210fedcba9876543210fedcba98
  cand "TEETH a -pre tag has no candidate" 1 module-v2.8.0-pre.1 'pass'
  cand "TEETH the pair still fails on the candidate path" 1 module-v2.9.0 'pass'
  pin optimizer-v2.8.0-rc.1; ver 2.8.0-rc.1
  cand "candidate run of an rc tag"         0 module-v2.8.0-rc.1 'pass'
  pin optimizer-v2.8.0; ver 2.8.0
  expect 2 "candidate run id without sha"   --tag module-v2.8.0 "${base[@]}" --candidate-run-id 777 --repo o/r
  expect 2 "both kinds of run evidence"     --tag module-v2.8.0 "${base[@]}" --candidate-run-id 777 --run-id 1 --repo o/r --tag-sha x
  # --resolve-candidate: the commit goes to --out, and only when verified.
  cand "resolve fixture" 0 module-v2.8.0 'pass' >/dev/null
  : > "$t/gh_out"
  if FIX_CRUN="$t/crun.json" FIX_CJOBS="$t/cjobs.jsonl" PATH="$cshim:$PATH" \
       bash "${BASH_SOURCE[0]}" --resolve-candidate --tag module-v2.8.0 --run-id 777 --repo o/r --out "$t/gh_out" >/dev/null 2>&1 \
     && [ "$(cat "$t/gh_out")" = "head_sha=$csha" ]; then echo "  ok   resolve writes head_sha of the verified run"
  else echo "  FAIL resolve did not write head_sha=$csha"; fails=$((fails + 1)); fi
  cand "resolve fixture, seal failed" 1 module-v2.8.0 'jobs[0]["conclusion"]="failure"' >/dev/null
  : > "$t/gh_out"
  if FIX_CRUN="$t/crun.json" FIX_CJOBS="$t/cjobs.jsonl" PATH="$cshim:$PATH" \
       bash "${BASH_SOURCE[0]}" --resolve-candidate --tag module-v2.8.0 --run-id 777 --repo o/r --out "$t/gh_out" >/dev/null 2>&1; then
    echo "  FAIL resolve passed an unsealed run"; fails=$((fails + 1))
  elif [ -s "$t/gh_out" ]; then echo "  FAIL resolve wrote an output for a refused run"; fails=$((fails + 1))
  else echo "  ok   resolve refuses an unsealed run and writes nothing"; fi
  if ! grep -v -E '^api (--paginate )?repos/o/r/actions/runs/777(/jobs\?per_page=100 --jq \.jobs\[\])?$' "$GH_CALLS" | grep -q .; then
    echo "  ok   only API reads of the candidate run were made"
  else echo "  FAIL a non-read call was made for the candidate"; fails=$((fails + 1)); fi

  # No run id: the output says the evidence was not checked.
  pin optimizer-v2.8.0; ver 2.8.0
  FAILS=0; (rehearse --tag module-v2.8.0-pre.1 "${base[@]}") > "$t/out" 2>&1
  if grep -q "^NOT CHECKED" "$t/out"; then echo "  ok   no dry run id is reported as not checked"
  else echo "  FAIL no dry run id was not reported"; fails=$((fails + 1)); fi

  # The two job names are matched on their fixed text plus an optional suffix,
  # so no other job of the release pipeline may start with that text.
  local wf="${HERE}/../../.github/workflows/pipeline-release-module.yml" pfx n
  for pfx in "Sign SHA256SUMS" "Create the GitHub release"; do
    n="$(grep -cE "^    name: \"?${pfx}" "$wf" || true)"
    if [ "$n" = 1 ]; then echo "  ok   exactly one release job name starts with '${pfx}'"
    else echo "  FAIL ${n} release job names start with '${pfx}', expected 1"; fails=$((fails + 1)); fi
  done
  if [ "$fails" -eq 0 ]; then echo "pair_rehearsal self-test passed"
  else echo "pair_rehearsal self-test: $fails failed"; return 1; fi
}

case "${1:-}" in
  --self-test) self_test ;;
  --resolve-candidate) shift; resolve_candidate "$@" ;;
  *) rehearse "$@" ;;
esac
