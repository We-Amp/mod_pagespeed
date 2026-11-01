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
#       [--run-json FILE --tag-sha SHA]
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
#   5. With --run-json (`gh run view ID --json event,headSha,conclusion,jobs`
#      of a dry run of the release lane): the run is a dispatch at the tag's
#      commit, the optimizer fetch and the package install-and-serve smoke
#      jobs succeeded (not skipped), and no job that signs or publishes ran.
#
# It reads files and prints; it publishes nothing and needs no secret.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

FAILS=0
bad() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
ok() { echo "ok: $*"; }

# check_run_json FILE TAG_SHA
check_run_json() {
  python3 -I - "$1" "$2" <<'PY'
import json, re, sys
doc = json.load(open(sys.argv[1]))
want_sha = sys.argv[2]
errs = []
if doc.get("event") != "workflow_dispatch":
    errs.append("run event is %r, not a dispatch (a pushed tag is a real run)" % doc.get("event"))
if doc.get("headSha") != want_sha:
    errs.append("run is at %s, the tag is at %s: rehearse the commit being tagged"
                % (doc.get("headSha"), want_sha))
jobs = doc.get("jobs", [])
def find(pattern):
    return [j for j in jobs if re.search(pattern, j.get("name", ""))]
need = [
    ("optimizer fetch", r"Fetch and verify the optimizer packages"),
    ("Debian install and serve", r"Smoke Test \(Debian"),
    ("RPM install and serve", r"Smoke Test \(RPM"),
]
for label, pat in need:
    found = find(pat)
    if not found:
        errs.append("no job for %s (%s) in the run" % (label, pat))
    for j in found:
        if j.get("conclusion") != "success":
            errs.append("%s: job %r is %r, it must have run and passed"
                        % (label, j.get("name"), j.get("conclusion")))
# A dry run signs and publishes nothing: these jobs are real-run only.
for j in find(r"Sign SHA256SUMS|Create the GitHub release|Publish|Promote"):
    if j.get("conclusion") not in ("skipped", "cancelled", None, ""):
        errs.append("job %r is %r: this is not a dry run" % (j.get("name"), j.get("conclusion")))
for e in errs:
    print("FAIL: " + e)
sys.exit(1 if errs else 0)
PY
}

rehearse() {
  local tag="" pin="" vfile="" mbazel="" repos="" runjson="" tagsha=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --tag) tag="${2:-}"; shift 2 ;;
      --pin) pin="${2:-}"; shift 2 ;;
      --optimizer-version-file) vfile="${2:-}"; shift 2 ;;
      --optimizer-module-bazel) mbazel="${2:-}"; shift 2 ;;
      --repositories-bzl) repos="${2:-}"; shift 2 ;;
      --run-json) runjson="${2:-}"; shift 2 ;;
      --tag-sha) tagsha="${2:-}"; shift 2 ;;
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

  if [ -n "$runjson" ]; then
    [ -n "$tagsha" ] || { echo "pair_rehearsal: --run-json needs --tag-sha" >&2; return 2; }
    if out="$(check_run_json "$runjson" "$tagsha" 2>&1)"; then ok "dry run: install and serve jobs passed at $tagsha"
    else printf '%s\n' "$out"; FAILS=$((FAILS + 1)); fi
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

  # Run evidence.
  local sha=1111111111111111111111111111111111111111
  pin optimizer-v2.8.0; ver 2.8.0
  run() { # event headSha smoke-conclusion publish-conclusion
    cat > "$t/run.json" <<JSON
{"event":"$1","headSha":"$2","conclusion":"success","jobs":[
 {"name":"module / Fetch and verify the optimizer packages","conclusion":"success"},
 {"name":"module / Smoke Test (Debian)","conclusion":"$3"},
 {"name":"module / Smoke Test (RPM)","conclusion":"success"},
 {"name":"module / Create the GitHub release","conclusion":"$4"}]}
JSON
  }
  run workflow_dispatch $sha success skipped
  expect 0 "dry run evidence"                  --tag module-v2.8.0-pre.1 "${base[@]}" --run-json "$t/run.json" --tag-sha $sha
  run workflow_dispatch $sha skipped skipped
  expect 1 "TEETH smoke job skipped"           --tag module-v2.8.0-pre.1 "${base[@]}" --run-json "$t/run.json" --tag-sha $sha
  run workflow_dispatch 2222222222222222222222222222222222222222 success skipped
  expect 1 "TEETH run at another commit"       --tag module-v2.8.0-pre.1 "${base[@]}" --run-json "$t/run.json" --tag-sha $sha
  run push $sha success skipped
  expect 1 "TEETH pushed run (a real run)"     --tag module-v2.8.0-pre.1 "${base[@]}" --run-json "$t/run.json" --tag-sha $sha
  run workflow_dispatch $sha success success
  expect 1 "TEETH release job ran"             --tag module-v2.8.0-pre.1 "${base[@]}" --run-json "$t/run.json" --tag-sha $sha

  if [ "$fails" -eq 0 ]; then echo "pair_rehearsal self-test passed"
  else echo "pair_rehearsal self-test: $fails failed"; return 1; fi
}

case "${1:-}" in
  --self-test) self_test ;;
  *) rehearse "$@" ;;
esac
