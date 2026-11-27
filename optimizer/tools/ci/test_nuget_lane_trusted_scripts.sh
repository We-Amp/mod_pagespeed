#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The NuGet lane runs from main, but most of its steps run in the tree of the
# tag being released, and an older tag does not carry newer scripts. The
# package checks (tools/ci/nupkg_checks.sh) therefore come from a checkout of
# main under trust/, the way the sidecar step takes its scripts. This test:
#   1. asserts every use of nupkg_checks.sh in the lane is the trust/ path,
#      and that the package steps check out main into trust/ (sparse, no
#      persisted credentials) before the first use;
#   2. simulates the failure it prevents: the three package steps run in a
#      workspace whose tree has no tools/ci/nupkg_checks.sh (an older tag).
#      With the trusted path they pass; with the tag-tree path they fail
#      (no such file; status 127 on Linux);
#   3. runs both against broken copies of the lane.
#
# Run: tools/ci/test_nuget_lane_trusted_scripts.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
W="$(mktemp -d "${TMPDIR:-/tmp}/trusted.XXXXXX")"
trap 'rm -rf "$W"' EXIT
LANE=.github/workflows/pipeline-release-nuget.yml

# shape <root>: violations of 1.
shape() {
  python3 -I - "$1/$LANE" <<'PY'
import re, sys
t = open(sys.argv[1]).read()
code = "\n".join(l for l in t.split("\n") if not l.lstrip().startswith("#"))
errs = []
uses = [l.strip() for l in code.split("\n") if "nupkg_checks.sh" in l and l.strip() != "tools/ci/nupkg_checks.sh"]  # the sparse-checkout entry is not a use
if len(uses) != 3: errs.append("expected 3 uses of nupkg_checks.sh, found %d" % len(uses))
for l in uses:
    if '"${GITHUB_WORKSPACE}/trust/tools/ci/nupkg_checks.sh"' not in l: errs.append("not the trusted path: " + l)
m = re.search(r"- name: Checkout main \(package checks\)\n\s+uses: actions/checkout@[0-9a-f]{40}[^\n]*\n\s+with:\n\s+ref: main\n\s+path: trust\n\s+sparse-checkout: \|\n\s+tools/ci/nupkg_checks\.sh\n\s+sparse-checkout-cone-mode: false\n\s+persist-credentials: false\n", code)
if not m: errs.append("the package steps do not check out main into trust/ (sparse, ref: main, persist-credentials: false)")
elif m.start() > code.index("nupkg_checks.sh"): errs.append("the trusted checkout comes after the first use")
if m:
    a = code.find("steps: &package-steps")
    nxt = re.search(r"\n  [A-Za-z_][\w-]*:\n", code[a:]) if a >= 0 else None
    end = a + nxt.start() if nxt else len(code)
    if not (0 <= a < m.start() < end): errs.append("the trusted checkout is not part of the shared package steps")
    p = code.find("- name: Pack NuGet packages", a if a >= 0 else 0)
    if p < 0 or not (p < m.start()): errs.append("the trusted checkout comes before the steps that run the released tree's code")
print("\n".join(errs)); sys.exit(1 if errs else 0)
PY
}

# step_script <file> <step name>: the run block of a step, dedented.
step_script() {
  python3 -I - "$1" "$2" <<'PY'
import re, sys
lines = open(sys.argv[1]).read().split("\n")
for i, l in enumerate(lines):
    if l.strip() == "- name: " + sys.argv[2]: break
else: sys.exit("step not found: " + sys.argv[2])
j = i + 1
while not re.match(r"^\s*run: \|\s*$", lines[j]): j += 1
body, base = [], None
for t in lines[j + 1:]:
    if t.strip() == "": body.append(""); continue
    ind = len(t) - len(t.lstrip())
    base = ind if base is None else base
    if ind < base: break
    body.append(t[base:])
print("\n".join(body))
PY
}

# simulate <root> <old|new>: 2.
simulate() {
  local root="$1" mode="$2" ws="$W/ws" name rc=0 f
  rm -rf "$ws"; mkdir -p "$ws/samples/aspnetcore" "$ws/trust/tools/ci" "$ws/tmp"
  cp "$HERE/nupkg_checks.sh" "$ws/trust/tools/ci/nupkg_checks.sh"        # main has it
  [ -f "$W/mk.py" ] || python3 -I - "$HERE/test_nupkg_checks.sh" "$W/mk.py" <<'PY'
import sys
src = open(sys.argv[1]).read()
open(sys.argv[2], "w").write(src.split("python3 -I - \"$1\" \"${2:-}\" \"${VV:-$V}\" <<'PY'\n")[1].split("\nPY\n")[0])
PY
  python3 -I "$W/mk.py" "$ws/samples/aspnetcore/nupkg" "" 9.9.9              # the tag's packages
  for name in "Expect seven packages" "Guard against wrong source-control metadata in the .nupkg files" "Guard against shipping an empty /console/ SPA"; do
    f="$W/step.sh"; step_script "$root/$LANE" "$name" > "$f"
    [ "$mode" = old ] && sed -i.bak 's#/trust/tools/ci/#/tools/ci/#' "$f"
    (cd "$ws" && GITHUB_WORKSPACE="$ws" RELEASE_TAG=v9.9.9 REAL_RUN=false RUNNER_TEMP="$ws/tmp" GITHUB_STEP_SUMMARY="$ws/sum" bash "$f" >"$W/out" 2>&1) || { rc=$?; echo "$name: status $rc: $(tail -n 1 "$W/out")"; }
  done
  return "$rc"
}

all() { local r="$1" n=0 o
  o="$(shape "$r")" || { echo "$o"; n=1; }
  o="$(simulate "$r" new)" || { echo "trusted path: $o"; n=1; }
  return "$n"; }

fail=0
if out="$(all "$ROOT")"; then echo "ok   the lane runs nupkg_checks.sh only from the trusted checkout"; else echo "FAIL"; echo "$out" | sed 's/^/     /'; fail=1; fi
# the simulation of the old behaviour: the same steps with the tag-tree path must fail because the file is missing
if out="$(simulate "$ROOT" old)"; then echo "FAIL the tag-tree path passed in a tree without the script"; fail=1; else
  if [ "$(printf '%s\n' "$out" | grep -c 'No such file or directory')" -eq 3 ]; then echo "ok   with the tag-tree path all three steps fail in a tree without the script (No such file or directory; 127 on Linux)"; else echo "FAIL the tag-tree path failed, but not for the missing script: $out"; fail=1; fi
fi

mutate() { # mutate <desc> <python expression over s>
  local d="$W/m"; rm -rf "$d"; mkdir -p "$d/.github/workflows"; cp "$ROOT/$LANE" "$d/$LANE"
  python3 -I - "$d/$LANE" "$2" <<'PY'
import sys
p, expr = sys.argv[1], sys.argv[2]
s = open(p).read(); s2 = eval(expr, {"s": s}); open(p, "w").write(s2)
sys.exit(0 if s2 != s else 3)
PY
  [ $? = 0 ] || { echo "FAIL mutation did not change anything: $1"; fail=1; return; }
  local why
  if why="$(all "$d" 2>&1)"; then echo "FAIL not caught: $1"; fail=1; else echo "ok   caught: $1  <-  $(printf '%s' "$why" | head -n 1 | cut -c1-150)"; fi
}
mutate 'one step calls the script from the tag tree' "s.replace('\"\${GITHUB_WORKSPACE}/trust/tools/ci/nupkg_checks.sh\" metadata', '\"\${GITHUB_WORKSPACE}/tools/ci/nupkg_checks.sh\" metadata')"
mutate 'all steps call the script from the tag tree' "s.replace('\${GITHUB_WORKSPACE}/trust/tools/ci/nupkg_checks.sh', '\${GITHUB_WORKSPACE}/tools/ci/nupkg_checks.sh')"
mutate 'a relative tag-tree call' "s.replace('\"\${GITHUB_WORKSPACE}/trust/tools/ci/nupkg_checks.sh\" console-spa', 'tools/ci/nupkg_checks.sh console-spa')"
mutate 'the trusted checkout removed' "s.replace('      - name: Checkout main (package checks)\n', '      - name: Something else\n')"
mutate 'the trusted checkout moved before the released tree runs' "(lambda b: s.replace(b, '', 1).replace('      - name: Ensure .NET 10 SDK', b + '      - name: Ensure .NET 10 SDK', 1))(s[s.index('      # The package checks come from main'):s.index('      - name: Expect seven packages')])"
mutate 'the trusted checkout takes the release ref' "s.replace('          ref: main\n          path: trust\n          sparse-checkout: |\n            tools/ci/nupkg_checks.sh', '          ref: \${{ needs.ref-guard.outputs.sha }}\n          path: trust\n          sparse-checkout: |\n            tools/ci/nupkg_checks.sh')"
mutate 'the trusted checkout persists credentials' "s.replace('          sparse-checkout-cone-mode: false\n          persist-credentials: false\n\n      - name: Expect seven packages', '          sparse-checkout-cone-mode: false\n\n      - name: Expect seven packages')"
if [ "$fail" = 0 ]; then echo PASS; else echo FAILED; exit 1; fi
