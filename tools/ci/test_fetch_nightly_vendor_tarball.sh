#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for the walk in tools/ci/fetch-nightly-vendor-tarball.sh. Builds a
# throwaway git history and substitutes the extractor (EXTRACT_VENDOR_SCRIPT)
# with a fake that succeeds only for chosen short SHAs, so no network or real
# tarball is involved.
#
# Run:  tools/ci/test_fetch_nightly_vendor_tarball.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FETCH="${HERE}/fetch-nightly-vendor-tarball.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

git init -q "$T/repo"
cd "$T/repo"
git config user.email t@example.invalid; git config user.name t
for i in 1 2 3 4; do echo "$i" > f; git add f; git commit -q -m "c$i"; done
SHAS=(); while IFS= read -r l; do SHAS+=("$l"); done < <(git rev-list --first-parent HEAD)   # newest first

# Fake extractor: argument --sha <short> --dest <dir>; succeeds only when the
# short SHA is listed in $HAVE (space separated), stamping GIT_COMMIT.
cat > "$T/fake-extract.sh" <<'F'
#!/usr/bin/env bash
sha="" dest=""
while [ $# -gt 0 ]; do
  case "$1" in --sha) sha="$2"; shift 2 ;; --dest) dest="$2"; shift 2 ;; *) shift ;; esac
done
for h in $HAVE; do
  if [ "$h" = "$sha" ]; then
    full="$(git rev-parse "$sha")"
    echo "$full" > "$dest/GIT_COMMIT"; exit 0
  fi
done
echo "fake: no tarball for $sha" >&2; exit 1
F
chmod +x "$T/fake-extract.sh"

fails=0
# run_case <name> <event> <ref> <have-list> <expected-exit> <expected-stamp-index|->
run_case() {
  local name="$1" event="$2" ref="$3" have="$4" want_rc="$5" want_idx="$6" rc=0
  rm -rf "$T/dest"
  HAVE="$have" EXTRACT_VENDOR_SCRIPT="$T/fake-extract.sh" GITHUB_EVENT_NAME="$event" \
    GITHUB_REF="$ref" bash "$FETCH" --dest "$T/dest" --walk 4 --head HEAD \
    >"$T/out.log" 2>&1 || rc=$?
  local ok=1
  [ "$rc" = "$want_rc" ] || ok=0
  if [ "$ok" = 1 ] && [ "$want_idx" != "-" ]; then
    [ "$(tr -d '[:space:]' < "$T/dest/GIT_COMMIT")" = "${SHAS[$want_idx]}" ] || ok=0
  fi
  if [ "$ok" = 1 ]; then echo "PASS  $name"; else echo "FAIL  $name (rc=$rc, want $want_rc)"; tail -8 "$T/out.log"; fails=$((fails + 1)); fi
}

S0="${SHAS[0]:0:7}"; S1="${SHAS[1]:0:7}"; S3="${SHAS[3]:0:7}"
run_case "schedule: HEAD has a tarball"                    schedule          refs/heads/master "$S0"     0 0
run_case "schedule: HEAD missing, walks to parent"         schedule          refs/heads/master "$S1"     0 1
run_case "schedule: walks three back"                      schedule          refs/heads/master "$S3"     0 3
run_case "schedule: none within window -> error"           schedule          refs/heads/master ""        1 -
run_case "dispatch on master: HEAD missing, walks"         workflow_dispatch refs/heads/master "$S1"     0 1
run_case "dispatch on a branch: HEAD missing -> error"     workflow_dispatch refs/heads/feat   "$S1"     1 -
run_case "dispatch on a branch: HEAD present"              workflow_dispatch refs/heads/feat   "$S0"     0 0

if [ "$fails" -ne 0 ]; then echo "FAILED: $fails case(s)" >&2; exit 1; fi
echo "fetch-nightly-vendor-tarball walk: all cases passed"
