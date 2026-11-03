#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Runs ../hydration_test.mjs in a real browser against a web server with a
# locally built module, and again with DetectHydratedPages off as a control.
#
#   test/browser/hydration/run_module_check.sh apache|nginx path/to/module.so
#
# Uses the published server and optimizer packages (test/system/shared_cache)
# with the module replaced. A nginx module must be built for Ubuntu 24.04's
# packaged nginx. Needs docker and
# python3. Every container and the network carry a prefix unique to this run and
# are removed on exit.
set -u
SERVER=${1:?usage: run_module_check.sh apache|nginx module.so}
SO=${2:?usage: run_module_check.sh apache|nginx module.so}
case "$SERVER" in apache|nginx) ;; *) echo "server must be apache or nginx" >&2; exit 2 ;; esac
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SO="$(cd "$(dirname "$SO")" && pwd)/$(basename "$SO")"
PREFIX="${HYDRATION_PREFIX:-hydration-$$-${RANDOM}}"
NET="$PREFIX-net"
IMAGE="$PREFIX-origin:local"
# Pinned by digest (Scorecard); the tag must match playwright in ../package.json.
BROWSER_IMAGE="mcr.microsoft.com/playwright:v1.58.2-noble@sha256:6446946a1d9fd62d9ae501312a2d76a43ee688542b21622056a372959b65d63d"
[ -f "$HERE/out/next/index.html" ] && [ -f "$HERE/out/nuxt/index.html" ] || "$HERE/build_all.sh" || exit 2
[ -d "$ROOT/test/browser/node_modules/playwright" ] || (cd "$ROOT/test/browser" && npm ci --no-fund --no-audit) || exit 2

CTX=""
cleanup() {
  docker rm -f "$PREFIX-on" "$PREFIX-off" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
  docker image rm "$IMAGE" >/dev/null 2>&1 || true
  [ -n "$CTX" ] && rm -rf "$CTX"
}
trap cleanup EXIT

PAIR=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['tag'].removeprefix('optimizer-v'))" \
  "$ROOT/install/debian/optimizer-pin.json") || exit 2
# The shared-cache image takes its packages from a pkgs/ directory in the build
# context; this check installs the published pair, so the context holds the
# Dockerfile and an empty pkgs/ (as test/system/shared_cache/run.sh stages it).
CTX=$(mktemp -d) || exit 2
mkdir "$CTX/pkgs"
cp "$ROOT/test/system/shared_cache/Dockerfile.$SERVER" "$CTX/" || exit 2
docker build -q -t "$IMAGE" --build-arg "PAIR_VERSION=$PAIR" --build-arg PACKAGES=published \
  -f "$CTX/Dockerfile.$SERVER" "$CTX" >/dev/null || exit 2
docker network create "$NET" >/dev/null || exit 2

start_server() { # name alias switch
  docker run -d --name "$1" --network "$NET" --network-alias "$2" \
    -e DETECT="$3" -e HOST_ALIAS="$2" -e MODULE_SO=/module.so \
    -v "$SO:/module.so:ro" -v "$HERE/out:/fixtures:ro" -v "$HERE:/harness:ro" \
    "$IMAGE" bash /harness/start-server.sh "$SERVER" >/dev/null || exit 2
}
start_server "$PREFIX-on" origin-on on
start_server "$PREFIX-off" origin-off off

run_browser() { # alias expect
  docker run --rm --name "$PREFIX-browser-$1" --network "$NET" --init \
    -v "$ROOT/test/browser:/browser:ro" -v "$HERE/out:/fixtures:ro" -w /browser \
    -e HYDRATION_ORIGIN="http://$1" -e HYDRATION_EXPECT="$2" -e HYDRATION_FIXTURES=/fixtures \
    "$BROWSER_IMAGE" node hydration_test.mjs
}
wait_ready() { # alias
  docker run --rm --network "$NET" "$BROWSER_IMAGE" bash -c "for i in \$(seq 1 120); do
    curl -fsS -o /dev/null http://$1/a.css && exit 0; sleep 1; done; exit 1" || \
    { docker logs --tail 40 "$PREFIX-${1#origin-}" >&2; exit 2; }
}
wait_ready origin-on
wait_ready origin-off
echo "=== $SERVER, DetectHydratedPages on"
run_browser origin-on on
on_rc=$?
echo "=== $SERVER, DetectHydratedPages off (control)"
run_browser origin-off off
off_rc=$?
echo "RESULT server=$SERVER on_rc=$on_rc off_rc=$off_rc"
[ "$on_rc" -eq 0 ] && [ "$off_rc" -eq 0 ]
