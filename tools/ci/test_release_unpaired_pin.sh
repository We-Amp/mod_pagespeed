#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The module-only release path of the module release lane
# (docs/ci/release.md, "Module-only releases"): a final version whose pin
# records "pairing": "unpaired" ships against the older optimizer release the
# pin names. This checks the wiring in .github/workflows/pipeline-release-module.yml
# and runs the lane's own "Record the optimizer pair version" step, extracted
# from the workflow, against a pin: the packages must depend on the pinned
# version in a module-only release and on the tag's version otherwise.
#
# WORKFLOW overrides the file under test; the test then also proves it fails
# against a copy without the unpaired condition.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WF="${WORKFLOW:-${ROOT}/.github/workflows/pipeline-release-module.yml}"
T="$(mktemp -d)"
trap 'rm -rf "${T}"' EXIT
fail=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1" >&2; fail=1; }

has() { # name fixed-string
  if grep -qF -- "$2" "$WF"; then ok "$1"; else bad "$1 (missing: $2)"; fi
}

# The wiring: the version job decides it from the tagged pin, every job that
# acts on it receives it, and the fetch job checks VERSION.txt against the
# pinned version and refuses a draft or pre-release optimizer release.
has "version job runs the pairing decision" 'python3 -I tools/ci/optimizer_pin_pairing.py --pin "${RUNNER_TEMP}/optimizer-pin.json" --version "${TAG#module-v}"'
has "version job reads the pin of the commit being built" 'git show "${SHA}:install/debian/optimizer-pin.json"'
has "version job outputs pair_unpaired" 'pair_unpaired: ${{ steps.derive.outputs.pair_unpaired }}'
has "version job writes pair_unpaired" 'echo "pair_unpaired=$PAIR_UNPAIRED"'
n="$(grep -cF 'PAIR_UNPAIRED: ${{ needs.version.outputs.pair_unpaired }}' "$WF")"
if [ "$n" -eq 3 ]; then ok "vendor, vendor-hosted and optimizer-fetch receive PAIR_UNPAIRED"
else bad "PAIR_UNPAIRED is passed to $n jobs, want 3 (vendor, vendor-hosted, optimizer-fetch)"; fi
has "fetch accepts the older pin only in a module-only release" 'if [ "$PAIR_UNPAIRED" = true ]; then'
has "fetch checks VERSION.txt against the pinned version" 'VERSION_ARGS+=(--pinned-version "$PAIR_VERSION")'
has "fetch refuses a draft or pre-release pinned release" 'os.environ.get("PAIR_UNPAIRED") == "true" and (doc.get("draft") or doc.get("prerelease"))'
has "release notes do not claim one version for an unpaired pin" 'if [ "$OPTIMIZER_TAG" = "optimizer-v${VERSION}" ]; then'

# The record step itself, as the lane runs it.
extract_record_step() {
  python3 -I - "$1" <<'PY'
import sys
lines = open(sys.argv[1]).read().split("\n")
start = next(i for i, l in enumerate(lines)
             if l.strip() == "- name: Record the optimizer pair version when there is no pair release")
run = next(i for i in range(start + 1, len(lines)) if lines[i].strip() == "run: |")
indent = len(lines[run]) - len(lines[run].lstrip())
body = []
for l in lines[run + 1:]:
    if l.strip() and len(l) - len(l.lstrip()) <= indent:
        break
    body.append(l)
pad = min(len(l) - len(l.lstrip()) for l in body if l.strip())
print("\n".join(l[pad:] for l in body))
PY
}

record() { # wf lenient unpaired pin-tag -> prints the file content or "none"
  local d="${T}/w$RANDOM"
  mkdir -p "$d/install/debian"
  printf '{"tag": "%s"}\n' "$4" > "$d/install/debian/optimizer-pin.json"
  extract_record_step "$1" > "$d/step.sh" || { echo "extract-failed"; return; }
  (cd "$d" && PAIR_LENIENT="$2" PAIR_UNPAIRED="$3" bash "$d/step.sh" >/dev/null 2>&1) || { echo "step-failed"; return; }
  if [ -f "$d/optimizer-pair-version" ]; then cat "$d/optimizer-pair-version"; else echo none; fi
}

expect_record() { # name want lenient unpaired pin-tag
  local got
  got="$(record "$WF" "$3" "$4" "$5")"
  if [ "$got" = "$2" ]; then ok "$1"; else bad "$1 (got '$got', want '$2')"; fi
}

expect_record "module-only release depends on the pinned optimizer" 2.8.1 false true optimizer-v2.8.1
expect_record "paired release has no pair-version file" none false false optimizer-v2.8.2
expect_record "rehearsal or rc still depends on the pin" 2.8.1 true false optimizer-v2.8.1
expect_record "an rc pin keeps the package spelling" '2.8.1~rc.1' true false optimizer-v2.8.1-rc.1

# Teeth: a copy of the workflow without the unpaired condition must fail.
if [ -z "${WORKFLOW:-}" ]; then
  sed 's/ || \[ "\$PAIR_UNPAIRED" = true \]; then/; then/' "$WF" > "${T}/broken.yml"
  if WORKFLOW="${T}/broken.yml" bash "${BASH_SOURCE[0]}" >/dev/null 2>&1; then
    bad "TEETH the test passes a workflow whose record step ignores PAIR_UNPAIRED"
  else
    ok "TEETH the test fails a workflow whose record step ignores PAIR_UNPAIRED"
  fi
fi

if [ "$fail" -ne 0 ]; then echo "test_release_unpaired_pin: FAILED" >&2; exit 1; fi
echo "test_release_unpaired_pin: all checks passed"
