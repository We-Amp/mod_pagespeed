#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The CI run and the release lane of one commit may run on the same build
# runners at the same time only if neither can pick up host state the other
# left (docs/ci/lane-write-sets.md). This test replays the two interleavings
# that can go wrong on one runner host, using the real workflow steps and the
# real tools/ci/ensure-workspace.sh. Hermetic: `docker` is a shim, the CI hub
# extract is a stub, no daemon and no network are needed.
#
#   1. vendor-tree     the release vendor job leaves its workspace: the commit
#                      stamp of the release commit, a version-stamped VERSION,
#                      vendor/. A CI reuse step of the same commit on that
#                      runner must re-extract, not reuse it.
#   2. extracted-tree  a release build job extracts the release tree and
#                      leaves it. Same rule.
#   3. ci-tree         a tree CI extracted itself (stamp only) is still reused:
#                      the warm path stays warm.
#   4. per-runner      every host-level cache directory a workflow mounts
#                      (CACHE_ROOT under ~/.cache) resolves to a different
#                      directory for two runner instances that share one home.
#   5. writers         two release legs on two runner instances of one host
#                      write the same entry name into their nginx cache
#                      directory, interleaved; each must read back its own.
#
# Run: tools/ci/test_lane_write_sets.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
RELEASE_WF="$ROOT/.github/workflows/pipeline-release-module.yml"
ENSURE="$HERE/ensure-workspace.sh"
MARKER=.release-lane-tree
FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

for f in "$RELEASE_WF" "$ENSURE"; do
  [ -f "$f" ] || { echo "FAIL: missing $f"; exit 1; }
done
command -v zstd >/dev/null 2>&1 || { echo "FAIL: zstd is required"; exit 1; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

# The run: body of the workflow step that carries the YAML anchor $2.
step_body() {
  python3 -I - "$1" "$2" <<'PY'
import re, sys
path, anchor = sys.argv[1], sys.argv[2]
lines = open(path).read().split("\n")
try:
    start = next(i for i, l in enumerate(lines)
                 if re.match(r"^\s*- &%s\s*$" % re.escape(anchor), l))
except StopIteration:
    sys.exit("no step anchored &%s" % anchor)
ind = len(lines[start]) - len(lines[start].lstrip())
for i in range(start + 1, len(lines)):
    l = lines[i]
    if l.strip() and len(l) - len(l.lstrip()) <= ind:
        break
    m = re.match(r"^(\s*)run:\s*(.*)$", l)
    if not m or len(m.group(1)) != ind + 2:
        continue
    if m.group(2) not in ("|", "|-"):
        print(m.group(2))
        sys.exit(0)
    body, bind = [], None
    for b in lines[i + 1:]:
        if not b.strip():
            body.append("")
            continue
        bi = len(b) - len(b.lstrip())
        if bind is None:
            bind = bi
        if bi < bind:
            break
        body.append(b[bind:])
    print("\n".join(body))
    sys.exit(0)
sys.exit("step &%s has no run:" % anchor)
PY
}

SHA=0123456789abcdef0123456789abcdef01234567

# Shims: docker (the stale-tree cleanup container) and the CI hub extract,
# which writes the tree CI would get from the hub: the stamp and an
# unstamped VERSION, no marker.
mkdir -p "$T/bin" "$T/ci/_ci-tools/tools/ci"
cat > "$T/bin/docker" <<'SH'
#!/usr/bin/env bash
exit 0
SH
cat > "$T/ci/_ci-tools/tools/ci/extract-vendor-tarball.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
dest=""
while [ $# -gt 0 ]; do
  case "$1" in --dest) dest="$2"; shift 2 ;; *) shift ;; esac
done
echo "extract $dest" >> "$EXTRACT_LOG"
mkdir -p "$dest/net/instaweb/public"
printf 'MAJOR=2\nMINOR=8\nBUILD=0\nPATCH=0\nPRERELEASE=\n' > "$dest/net/instaweb/public/VERSION"
echo -n "$GITHUB_SHA" > "$dest/GIT_COMMIT"
SH
chmod +x "$T/bin/docker" "$T/ci/_ci-tools/tools/ci/extract-vendor-tarball.sh"

# A checkout of the release commit as the vendor job has it after the
# version stamp: VERSION carries the release version, vendor/ is filled.
make_release_checkout() {
  mkdir -p "$1/net/instaweb/public" "$1/vendor"
  printf 'MAJOR=2\nMINOR=8\nBUILD=0\nPATCH=0\nPRERELEASE=pre.1\n' > "$1/net/instaweb/public/VERSION"
  echo dep > "$1/vendor/dep.txt"
}

# ensure-workspace.sh as a CI reuse job of $SHA runs it on workspace $1.
ci_reuse() {
  : > "$T/extract.log"
  ( cd "$T/ci" && PATH="$T/bin:$PATH" EXTRACT_LOG="$T/extract.log" \
      WORKSPACE_DIR="$1" GITHUB_SHA="$SHA" GITHUB_WORKSPACE="$T/ci" \
      bash "$ENSURE" "${SHA:0:7}" ) > "$T/reuse.out" 2>&1
}

# The CI job re-extracted: the stub ran, and the tree is CI's (no marker, no
# release version stamp, the right commit).
expect_reextracted() {
  local ws="$1" what="$2" ok=1
  grep -q "extract $ws" "$T/extract.log" || { fail "$what: the CI step reused the release tree"; sed 's/^/    /' "$T/reuse.out"; ok=0; }
  if [ "$ok" = 1 ]; then
    [ ! -e "$ws/$MARKER" ] || { fail "$what: $MARKER survived the re-extract"; ok=0; }
    ! grep -q 'PRERELEASE=pre' "$ws/net/instaweb/public/VERSION" || { fail "$what: the version-stamped VERSION survived"; ok=0; }
    [ "$(cat "$ws/GIT_COMMIT")" = "$SHA" ] || { fail "$what: wrong commit stamp after the re-extract"; ok=0; }
  fi
  [ "$ok" = 1 ] && pass "$what: the CI step re-extracted"
}

# --- 1. the vendor job's own workspace ------------------------------------
W1="$T/runner/_work/mod_pagespeed/mod_pagespeed"
make_release_checkout "$W1"
GITCOMMIT_STEP="$(step_body "$RELEASE_WF" v-gitcommit)" || { echo "FAIL: $GITCOMMIT_STEP"; exit 1; }
( cd "$W1" && RELEASE_SHA="$SHA" bash -euo pipefail -c "$GITCOMMIT_STEP" ) \
  || fail "vendor-tree: the commit stamp step failed"
[ "$(cat "$W1/GIT_COMMIT")" = "$SHA" ] || fail "vendor-tree: the stamp step did not stamp the commit"
# Keep a copy as the tree tarball the build jobs download.
mkdir -p "$T/rt/tree"
tar -C "$W1" -cf - . | zstd -q -o "$T/rt/tree/module-tree.tar.zst"
ci_reuse "$W1"
expect_reextracted "$W1" "vendor-tree"

# --- 2. a build job's extracted release tree -------------------------------
W2="$T/runner2/_work/mod_pagespeed/mod_pagespeed"
mkdir -p "$W2"
EXTRACT_STEP="$(step_body "$RELEASE_WF" tree-extract)" || { echo "FAIL: $EXTRACT_STEP"; exit 1; }
( cd "$W2" && RUNNER_TEMP="$T/rt" GITHUB_WORKSPACE="$W2" RELEASE_SHA="$SHA" \
    bash -euo pipefail -c "$EXTRACT_STEP" ) > "$T/extract-step.out" 2>&1 \
  || { fail "extracted-tree: the release tree-extract step failed"; sed 's/^/    /' "$T/extract-step.out"; }
ci_reuse "$W2"
expect_reextracted "$W2" "extracted-tree"

# --- 3. CI's own warm tree is reused --------------------------------------
W3="$T/runner3/ws"
mkdir -p "$W3"
echo -n "$SHA" > "$W3/GIT_COMMIT"
ci_reuse "$W3"
if [ -s "$T/extract.log" ]; then
  fail "ci-tree: a CI-prepared tree of the same commit was re-extracted"
elif grep -q 'reusing warm tree' "$T/reuse.out"; then
  pass "ci-tree: a CI-prepared tree of the same commit is reused"
else
  fail "ci-tree: unexpected output"; sed 's/^/    /' "$T/reuse.out"
fi

# --- 4. host-level cache directories are per runner instance ---------------
# Each CACHE_ROOT under ~/.cache, with the assignments of its own run block
# that lead to it, evaluated for two runner instances that share one home.
python3 -I - "$ROOT/.github/workflows" > "$T/roots.txt" <<'PY'
import glob, os, re, sys
for path in sorted(glob.glob(os.path.join(sys.argv[1], "*.yml"))):
    lines = open(path).read().split("\n")
    for i, l in enumerate(lines):
        if not re.match(r'^\s*CACHE_ROOT="\$\{HOME\}/\.cache/', l):
            continue
        j = i
        while j > 0 and not re.match(r"^\s*(- )?run:\s*\|", lines[j]):
            j -= 1
        assigns = [x.strip() for x in lines[j:i + 1]
                   if re.match(r"^\s*(RUNNER_TAG|CACHE_ROOT)=", x)]
        print("%s:%d\t%s" % (os.path.basename(path), i + 1, "; ".join(assigns)))
PY
n=0; shared=0; NGINX_A=""; NGINX_B=""
while IFS=$'\t' read -r where code; do
  n=$((n + 1))
  # Inputs a step gets from its job env (the same for both runners).
  a="$(HOME=/home/ci RUNNER_NAME='runner 1' EL=el8 bash -c "$code"'; echo "$CACHE_ROOT"')"
  b="$(HOME=/home/ci RUNNER_NAME='runner 2' EL=el8 bash -c "$code"'; echo "$CACHE_ROOT"')"
  if [ "$a" = "$b" ]; then
    fail "per-runner: $where: two runner instances share $a"
    shared=$((shared + 1))
  fi
  case "$a" in */mps-release-nginx|*/mps-release-nginx-runner*) [ -n "$NGINX_A" ] || { NGINX_A="$a"; NGINX_B="$b"; } ;; esac
done < "$T/roots.txt"
if [ "$n" -eq 0 ]; then
  fail "per-runner: no CACHE_ROOT found in the workflows"
elif [ "$shared" -eq 0 ]; then
  pass "per-runner: all $n host cache directories are per runner instance"
fi

# --- 5. interleaved nginx cache writers ------------------------------------
# Leg A writes its entry and waits; leg B writes the same entry name; leg A
# reads it back. Shared directory: A reads B's bytes.
if [ -z "$NGINX_A" ]; then
  fail "writers: no nginx cache directory found in the release lane"
else
  H="$T/home"
  da="$H${NGINX_A#/home/ci}"; db="$H${NGINX_B#/home/ci}"
  mkdir -p "$da" "$db"
  (
    echo "leg A" > "$da/entry"; : > "$T/a-wrote"
    while [ ! -e "$T/b-wrote" ]; do sleep 0.05; done
    cat "$da/entry" > "$T/a-read"
  ) &
  pa=$!
  (
    while [ ! -e "$T/a-wrote" ]; do sleep 0.05; done
    echo "leg B" > "$db/entry"; : > "$T/b-wrote"
  ) &
  pb=$!
  wait "$pa" "$pb"
  if [ "$(cat "$T/a-read")" = "leg A" ]; then
    pass "writers: interleaved legs on two runner instances keep their own cache entries"
  else
    fail "writers: leg A read '$(cat "$T/a-read")' from the nginx cache directory both runners share"
  fi
fi

if [ "$FAILS" -ne 0 ]; then
  echo "$FAILS check(s) failed"
  exit 1
fi
echo "all lane write-set checks passed"
