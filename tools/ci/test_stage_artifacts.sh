#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic test of stage-artifacts.sh's reject -> per-job refetch -> install
# path. docker and rsync are shims; the "hub" is a local directory.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/stage-artifacts-test.XXXXXX")"
trap 'rm -rf "$ROOT"' EXIT
fail() { echo "FAIL: $1" >&2; exit 1; }
pass() { echo "ok: $1"; }

SHA=$(printf 'a%.0s' {1..40})
mkdir -p "$ROOT/tools/ci" "$ROOT/docker" "$ROOT/bin" "$ROOT/hub/$SHA/linux-x64" "$ROOT/src/.ci-artifacts" "$ROOT/run/tmp"
cp "$HERE/stage-artifacts.sh" "$HERE/validate-local-bundle.sh" "$HERE/invalidate-stale-artifacts.sh" "$ROOT/tools/ci/"
printf '#!/bin/sh\nexit 0\n' > "$ROOT/tools/ci/docker-preflight.sh"
chmod +x "$ROOT"/tools/ci/*.sh
echo base > "$ROOT/docker/Dockerfile.base-runtime"
# docker shim: image inspect reports the right label hash; run prints a version.
cat > "$ROOT/bin/docker" <<'SH'
#!/bin/sh
case "$1" in
  image) sha256sum "$FAKE_ROOT/docker/Dockerfile.base-runtime" 2>/dev/null | cut -d' ' -f1 || shasum -a 256 "$FAKE_ROOT/docker/Dockerfile.base-runtime" | cut -d' ' -f1 ;;
  run) echo 1.2.3 ;;
esac
SH
# rsync shim: last two args are src/ dst/ ; copy directory contents.
cat > "$ROOT/bin/rsync" <<'SH'
#!/bin/sh
for a; do dst="$a"; done
n=$#; i=0; for a; do i=$((i+1)); [ "$i" -eq $((n-1)) ] && src="$a"; done
cp -R "${src#*:}". "$dst"
SH
chmod +x "$ROOT/bin/docker" "$ROOT/bin/rsync"

# Hub bundle: complete.
echo hubworker > "$ROOT/hub/$SHA/linux-x64/factory_worker"
echo hubmod > "$ROOT/hub/$SHA/linux-x64/ngx_pagespeed_module.so"
echo 1.2.3 > "$ROOT/hub/$SHA/linux-x64/nginx-version"
echo brotli > "$ROOT/hub/$SHA/linux-x64/ngx_http_brotli_filter_module.so"

# Shared runner dir holds a PARTIAL bundle (a Release-shaped set / a Build in progress).
echo partial > "$ROOT/src/.ci-artifacts/factory_worker"
echo mid-build > "$ROOT/src/.ci-artifacts/build-all.log"

mkdir -p "$ROOT/co"
(cd "$ROOT/co" && cp -R "$ROOT/docker" . &&
  PATH="$ROOT/bin:$PATH" FAKE_ROOT="$ROOT" RUNNER_TEMP="$ROOT/run/tmp" GITHUB_SHA="$SHA" \
  CI_HUB_SSH=hub CI_HUB_ARTIFACTS_ROOT="$ROOT/hub" CI_ARTIFACTS_PLATFORM=linux-x64 \
  bash "$ROOT/tools/ci/stage-artifacts.sh" "$ROOT/src" >"$ROOT/out.log" 2>&1) || { cat "$ROOT/out.log" >&2; fail "stage-artifacts exited non-zero"; }

[ "$(cat "$ROOT/co/.ci-artifacts/factory_worker")" = hubworker ] || fail "1: installed worker is not the hub copy"
[ -f "$ROOT/co/.ci-artifacts/ngx_pagespeed_module.so" ] || fail "1: module not installed"
pass "1: partial local bundle replaced by a complete hub bundle"
[ -f "$ROOT/src/.ci-artifacts/build-all.log" ] && [ "$(cat "$ROOT/src/.ci-artifacts/factory_worker")" = partial ] \
  || fail "2: shared runner dir was modified"
pass "2: shared ${ROOT##*/}/src/.ci-artifacts left untouched"

# Producer published a partial set: must fail, not install.
rm "$ROOT/hub/$SHA/linux-x64/ngx_pagespeed_module.so"; rm -rf "$ROOT/co/.ci-artifacts"
set +e
(cd "$ROOT/co" && PATH="$ROOT/bin:$PATH" FAKE_ROOT="$ROOT" RUNNER_TEMP="$ROOT/run/tmp" GITHUB_SHA="$SHA" \
  CI_HUB_SSH=hub CI_HUB_ARTIFACTS_ROOT="$ROOT/hub" CI_ARTIFACTS_PLATFORM=linux-x64 \
  bash "$ROOT/tools/ci/stage-artifacts.sh" "$ROOT/src" >"$ROOT/out2.log" 2>&1)
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "3: partial hub bundle was accepted"
grep -q "incomplete" "$ROOT/out2.log" || fail "3: no incomplete-bundle error"
pass "3: partial hub bundle fails with a clear error"
echo "ALL PASS"
