#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The module-only release path of the module release lane
# (docs/ci/release.md, "Module-only releases"): a final version whose pin
# records "unpaired_for": "<that version>" ships against the older optimizer
# release the pin names. This runs the lane's own steps, extracted from
# .github/workflows/pipeline-release-module.yml, against made-up pins:
#
#   - "Record the optimizer pair version when there is no pair release"
#     (vendor): the packages depend on the pinned version in a module-only
#     release and on the tag's version otherwise;
#   - "Download and verify against the pin" (optimizer-fetch), with curl
#     stubbed by a local fixture: the older pin is accepted only in a
#     module-only release, VERSION.txt is checked against the pinned version,
#     and a draft or pre-release pinned release, or an existing optimizer
#     release of the module version, is refused.
#
# Then it proves each of those checks has teeth: the test must fail against a
# copy of the workflow with that check removed.
#
# WORKFLOW overrides the file under test (the teeth runs use it).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WF="${WORKFLOW:-${ROOT}/.github/workflows/pipeline-release-module.yml}"
T="$(mktemp -d)"
trap 'rm -rf "${T}"' EXIT
fail=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1" >&2; fail=1; }

# The version job decides it and hands it to exactly the three jobs that act
# on it (vendor, vendor-hosted, optimizer-fetch).
n="$(grep -cF 'PAIR_UNPAIRED: ${{ needs.version.outputs.pair_unpaired }}' "$WF")"
if [ "$n" -eq 3 ]; then ok "vendor, vendor-hosted and optimizer-fetch receive PAIR_UNPAIRED"
else bad "PAIR_UNPAIRED is passed to $n jobs, want 3"; fi

extract_step() { # workflow step-name -> the step's run script
  python3 -I - "$1" "$2" <<'PY'
import sys
lines = open(sys.argv[1]).read().split("\n")
start = next(i for i, l in enumerate(lines) if l.strip() == "- name: " + sys.argv[2])
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

# ---- vendor: Record the optimizer pair version ---------------------------
extract_step "$WF" "Record the optimizer pair version when there is no pair release" > "${T}/record.sh" \
  || bad "cannot extract the record step"
record() { # lenient unpaired pin-tag -> the file content or "none"
  local d="${T}/r$RANDOM$RANDOM"
  mkdir -p "$d/install/debian"
  printf '{"tag": "%s"}\n' "$3" > "$d/install/debian/optimizer-pin.json"
  (cd "$d" && PAIR_LENIENT="$1" PAIR_UNPAIRED="$2" bash "${T}/record.sh" >/dev/null 2>&1) || { echo "step-failed"; return; }
  if [ -f "$d/optimizer-pair-version" ]; then cat "$d/optimizer-pair-version"; else echo none; fi
}
expect_record() { # name want lenient unpaired pin-tag
  local got; got="$(record "$3" "$4" "$5")"
  if [ "$got" = "$2" ]; then ok "record: $1"; else bad "record: $1 (got '$got', want '$2')"; fi
}
expect_record "module-only release depends on the pinned optimizer" 2.8.1 false true optimizer-v2.8.1
expect_record "paired release has no pair-version file" none false false optimizer-v2.8.2
expect_record "rehearsal or rc still depends on the pin" 2.8.1 true false optimizer-v2.8.1
expect_record "an rc pin keeps the package spelling" '2.8.1~rc.1' true false optimizer-v2.8.1-rc.1

# ---- optimizer-fetch: Download and verify against the pin ---------------
extract_step "$WF" "Download and verify against the pin" > "${T}/fetch.sh" \
  || bad "cannot extract the fetch step"
COMMIT=1111111111111111111111111111111111111111
CYC=2222222222222222222222222222222222222222
FIX="${T}/fix"
mkdir -p "${T}/bin" "${FIX}/raw/${COMMIT}" "${FIX}/api" "${FIX}/dl"
printf 'x(\n    name = "cyclone",\n    commit = "%s",\n)\n' "$CYC" > "${FIX}/raw/${COMMIT}/MODULE.bazel"
for f in pagespeed-optimizer_2.8.1_amd64.deb pagespeed-optimizer-2.8.1-1.x86_64.rpm; do
  printf '%s\n' "$f" > "${FIX}/dl/$f"
done
(cd "${FIX}/dl" && sha256sum pagespeed-optimizer_2.8.1_amd64.deb pagespeed-optimizer-2.8.1-1.x86_64.rpm > SHA256SUMS)
SUMS="$(sha256sum "${FIX}/dl/SHA256SUMS" | cut -d' ' -f1)"
release_json() { # tag prerelease
  python3 -I -c 'import json,sys
names=["SHA256SUMS","pagespeed-optimizer_2.8.1_amd64.deb","pagespeed-optimizer-2.8.1-1.x86_64.rpm"]
print(json.dumps({"tag_name":sys.argv[1],"draft":False,"prerelease":sys.argv[2]=="true",
  "assets":[{"name":n,"browser_download_url":"https://dl.test/"+n} for n in names]}))' "$1" "$2"
}
# curl: serves the fixture by URL; -w prints the HTTP status of a probe.
cat > "${T}/bin/curl" <<'STUB'
#!/usr/bin/env bash
out="" remote=0 fmt="" url=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2 ;;
    -O) remote=1; shift ;;
    -w) fmt="$2"; shift 2 ;;
    -H|--retry) shift 2 ;;
    https://*) url="$1"; shift ;;
    *) shift ;;
  esac
done
case "$url" in
  https://raw.githubusercontent.com/We-Amp/pagespeed-optimizer/*) f="$FIX/raw/${url#https://raw.githubusercontent.com/We-Amp/pagespeed-optimizer/}" ;;
  https://api.github.com/repos/We-Amp/pagespeed-optimizer/releases/tags/*) f="$FIX/api/${url##*/}.json" ;;
  https://dl.test/*) f="$FIX/dl/${url##*/}" ;;
  *) echo "curl stub: unexpected URL $url" >&2; exit 9 ;;
esac
if [ -n "$fmt" ]; then
  if [ -f "$f" ]; then printf 200; else printf 404; fi
  exit 0
fi
[ -f "$f" ] || exit 22
if [ "$remote" = 1 ]; then cp "$f" "./${url##*/}"; else cp "$f" "$out"; fi
STUB
chmod +x "${T}/bin/curl"

fetch() { # lenient unpaired pin-tag VERSION.txt pinned-prerelease paired-exists -> exit status
  local d="${T}/f$RANDOM$RANDOM"
  mkdir -p "$d/install/debian" "$d/bazel" "$d/tools/ci" "$d/lane-tools/tools/ci"
  printf '{"tag": "%s", "optimizer_commit": "%s", "sha256sums_sha256": "%s"}\n' "$3" "$COMMIT" "$SUMS" \
    > "$d/install/debian/optimizer-pin.json"
  printf 'CYCLONE_COMMIT = "%s"\n' "$CYC" > "$d/bazel/repositories.bzl"
  cp "${ROOT}/tools/ci/check_optimizer_pin_pair.py" "$d/tools/ci/"
  cp "${ROOT}/tools/ci/check_module_optimizer_version.py" "$d/lane-tools/tools/ci/"
  printf '%s\n' "$4" > "${FIX}/raw/${COMMIT}/VERSION.txt"
  rm -f "${FIX}"/api/*.json
  release_json "$3" "$5" > "${FIX}/api/$3.json"
  [ "$6" = true ] && release_json optimizer-v2.8.2 false > "${FIX}/api/optimizer-v2.8.2.json"
  (cd "$d" && FIX="$FIX" PATH="${T}/bin:$PATH" GITHUB_OUTPUT="$d/out" GITHUB_STEP_SUMMARY="$d/summary" \
     OPTIMIZER_TAG=optimizer-v2.8.2 RELEASE_VERSION=2.8.2 PAIR_LENIENT="$1" PAIR_UNPAIRED="$2" GH_READ_TOKEN=x \
     bash "${T}/fetch.sh" > "$d/log" 2>&1)
  local rc=$?
  LAST_LOG="$d/log"; LAST_OUT="$d/out"
  return $rc
}
expect_fetch() { # name want(pass|fail) why(text the log must contain, or "") args...
  local name="$1" want="$2" why="$3"; shift 3
  if fetch "$@"; then got=pass; else got=fail; fi
  if [ "$got" = "$want" ] && { [ -z "$why" ] || grep -qF -- "$why" "$LAST_LOG"; }; then ok "fetch: $name"
  else bad "fetch: $name (got $got, want $want${why:+ with: $why})"; sed 's/^/      /' "$LAST_LOG" | tail -8 >&2; fi
}
expect_fetch "module-only release accepts the older pin" pass "module-only release: module 2.8.2 ships with optimizer 2.8.1" false true optimizer-v2.8.1 2.8.1 false false
if grep -qx 'pair_pkg_version=2.8.1' "$LAST_OUT" 2>/dev/null; then ok "fetch: module-only release hands on the pinned package version"
else bad "fetch: module-only release does not output pair_pkg_version=2.8.1"; fi
expect_fetch "a real paired run still refuses the older pin" fail "pins 'optimizer-v2.8.1' but this release needs 'optimizer-v2.8.2'" false false optimizer-v2.8.1 2.8.1 false false
expect_fetch "module-only: VERSION.txt must be the pinned version" fail "VERSION.txt at the pinned commit says '2.8.2'" false true optimizer-v2.8.1 2.8.2 false false
expect_fetch "module-only: a pre-release pinned release is refused" fail "is a draft or a pre-release" false true optimizer-v2.8.1 2.8.1 true false
expect_fetch "module-only: refused when optimizer-v2.8.2 exists" fail "the optimizer release optimizer-v2.8.2 exists" false true optimizer-v2.8.1 2.8.1 false true
expect_fetch "a rehearsal still takes the older pin with a warning" pass "no paired optimizer release for this version" true false optimizer-v2.8.1 2.8.2 false false

# ---- teeth: each check removed from a copy must fail this test -----------
if [ -z "${WORKFLOW:-}" ]; then
  mutate() { # name old new
    python3 -I - "$WF" "${T}/broken.yml" "$2" "$3" <<'PY' || { bad "TEETH $1: the text to remove is not in the workflow"; return; }
import sys
s = open(sys.argv[1]).read()
if s.count(sys.argv[3]) != 1:
    sys.exit(1)
open(sys.argv[2], "w").write(s.replace(sys.argv[3], sys.argv[4]))
PY
    if WORKFLOW="${T}/broken.yml" bash "${BASH_SOURCE[0]}" >/dev/null 2>&1; then
      bad "TEETH $1: the test passes a workflow without it"
    else
      ok "TEETH $1: the test fails a workflow without it"
    fi
  }
  mutate "record step honours PAIR_UNPAIRED" \
    'if [ "$PAIR_LENIENT" = true ] || [ "$PAIR_UNPAIRED" = true ]; then' 'if [ "$PAIR_LENIENT" = true ]; then'
  mutate "fetch accept branch" \
    '            if [ "$PAIR_UNPAIRED" = true ]; then
              code=' '            if false; then
              code='
  mutate "--pinned-version selection" \
    'VERSION_ARGS+=(--pinned-version "$PAIR_VERSION")' ':'
  mutate "draft or pre-release refusal" \
    'if os.environ.get("PAIR_UNPAIRED") == "true" and (doc.get("draft") or doc.get("prerelease")):' 'if False:'
  mutate "existing paired release refusal" \
    '                404) ;;' '                404|200) ;;'
fi

if [ "$fail" -ne 0 ]; then echo "test_release_unpaired_pin: FAILED" >&2; exit 1; fi
echo "test_release_unpaired_pin: all checks passed"
