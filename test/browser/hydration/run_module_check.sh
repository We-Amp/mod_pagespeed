#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Runs ../hydration_test.mjs in a real browser against a web server with a
# locally built module, and again with DetectHydratedPages off as a control.
#
#   test/browser/hydration/run_module_check.sh apache|nginx path/to/module.so
#   PACKAGE_DIR=<dir> test/browser/hydration/run_module_check.sh apache|nginx
#
# Uses the published server and optimizer packages (test/system/shared_cache)
# with the module replaced. With PACKAGE_DIR=<dir> (as in
# test/system/shared_cache/run.sh) the module package and the optimizer package
# come from that directory instead, nothing is installed from the package
# repository, and the module comes from its package (no module.so argument).
# A nginx module must be built for Ubuntu 24.04's packaged nginx. Needs docker
# and python3. Every container and the network carry a prefix unique to this run and
# are removed on exit.
set -u
# shellcheck source=../../../tools/ci/docker_pull_retry.sh
source "$(dirname "${BASH_SOURCE[0]}")/../../../tools/ci/docker_pull_retry.sh"
SERVER=${1:?usage: run_module_check.sh apache|nginx module.so}
# A job that means local packages (LOCAL_PACKAGES=true) must have staged them:
# an empty PACKAGE_DIR would otherwise run the published mode under a local name.
if [ "${LOCAL_PACKAGES:-}" = true ] && [ -z "${PACKAGE_DIR:-}" ]; then
  echo "run_module_check.sh: LOCAL_PACKAGES=true but PACKAGE_DIR is empty" >&2
  exit 2
fi
SO=${2:-}
if [ -n "${PACKAGE_DIR:-}" ]; then
  if [ -n "$SO" ]; then
    echo "run_module_check.sh: PACKAGE_DIR and a module.so argument are exclusive (the module comes from its package)" >&2
    exit 2
  fi
elif [ -z "$SO" ]; then
  echo "usage: run_module_check.sh apache|nginx module.so" >&2
  exit 2
fi
case "$SERVER" in apache|nginx) ;; *) echo "server must be apache or nginx" >&2; exit 2 ;; esac
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
[ -z "$SO" ] || SO="$(cd "$(dirname "$SO")" && pwd)/$(basename "$SO")"
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
# context; by default this check installs the published pair, so the context
# holds the Dockerfile and an empty pkgs/. With PACKAGE_DIR it holds the two
# packages (staged as test/system/shared_cache/run.sh stages them).
CTX=$(mktemp -d) || exit 2
mkdir "$CTX/pkgs"
cp "$ROOT/test/system/shared_cache/Dockerfile.$SERVER" "$CTX/" || exit 2
PACKAGES=published
if [ -n "${PACKAGE_DIR:-}" ]; then
  PACKAGES=local
  case $SERVER in apache) MODPKG=mod-pagespeed ;; nginx) MODPKG=nginx-module-pagespeed ;; esac
  tilde='~'
  for pat in "${MODPKG}_*.deb" "pagespeed-optimizer_${PAIR/-/$tilde}_*.deb"; do
    # shellcheck disable=SC2086  # the pattern is a glob
    set -- "$PACKAGE_DIR"/$pat
    if [ $# -ne 1 ] || [ ! -f "$1" ]; then
      echo "run_module_check.sh: PACKAGE_DIR $PACKAGE_DIR must hold exactly one $pat" >&2
      exit 2
    fi
    cp "$1" "$CTX/pkgs/" || exit 2
  done
fi
docker build -q -t "$IMAGE" --build-arg "PAIR_VERSION=$PAIR" --build-arg "PACKAGES=$PACKAGES" \
  -f "$CTX/Dockerfile.$SERVER" "$CTX" >/dev/null || exit 2
docker network create "$NET" >/dev/null || exit 2

MODULE_ARGS=()
[ -z "$SO" ] || MODULE_ARGS=(-e MODULE_SO=/module.so -v "$SO:/module.so:ro")
start_server() { # name alias switch
  # local-image: built by docker build above
  docker run -d --name "$1" --network "$NET" --network-alias "$2" \
    -e DETECT="$3" -e HOST_ALIAS="$2" ${MODULE_ARGS[@]+"${MODULE_ARGS[@]}"} \
    -v "$HERE/out:/fixtures:ro" -v "$HERE:/harness:ro" \
    "$IMAGE" bash /harness/start-server.sh "$SERVER" >/dev/null || exit 2
}
start_server "$PREFIX-on" origin-on on
start_server "$PREFIX-off" origin-off off
# Local packages mean nothing came from the package repository: the image
# must carry no source of it (the published install adds one, a package
# built here must not).
if [ "$PACKAGES" = local ] && docker exec "$PREFIX-on" grep -rqsi modpagespeed /etc/apt; then
  echo "FAIL: PACKAGE_DIR mode, but the image has a modpagespeed package source under /etc/apt"
  exit 1
fi

run_browser() { # alias expect
  docker_pull_retry "$BROWSER_IMAGE" || exit 1
  docker run --rm --name "$PREFIX-browser-$1" --network "$NET" --init \
    -v "$ROOT/test/browser:/browser:ro" -v "$HERE/out:/fixtures:ro" -w /browser \
    -e HYDRATION_ORIGIN="http://$1" -e HYDRATION_EXPECT="$2" -e HYDRATION_FIXTURES=/fixtures \
    "$BROWSER_IMAGE" node hydration_test.mjs
}
wait_ready() { # alias
  docker_pull_retry "$BROWSER_IMAGE" || exit 1
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
