#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Shared-cache check for daemon-served image variants.
#
#   test/system/shared_cache/run.sh apache|nginx [path/to/module.so]
#
# Runs the web server with the published module and optimizer packages
# (Dockerfile.apache / Dockerfile.nginx; the optimizer release is the one
# install/debian/optimizer-pin.json pins, and the module package is its
# pair), optionally with a locally built module swapped in, behind two real
# shared caches: nginx proxy_cache and Varnish, both with their stock Vary
# handling.  One URL is requested as a desktop browser, a phone and a desktop
# with Save-Data: first directly, until each client is answered with a
# stable optimized variant of its own (no two clients may share a baseline:
# a client still served another client's bytes is not ready), then through each cache in that order, then once
# more as the desktop as a positive control.  Two more caches carry the
# device-class configurations the deployment docs give (nginx-device-class.conf,
# varnish-device-class.vcl); through those, a second phone with a different
# User-Agent must be a cache hit with the phone's bytes.  The check fails when a cache
# hands a client bytes that differ from what the origin sends that client,
# when the positive control is not a cache hit with the same bytes (a
# response no cache stores would pass the first check trivially), when a
# response's Vary does not name Accept, Save-Data, User-Agent and Sec-CH-DPR,
# or when the 304 names a different set than its 200.
#
# With PACKAGE_DIR=<dir> the packages come from that directory instead of the
# repository: the module package of the tree under test (for apache,
# mod-pagespeed_*.deb; for nginx, nginx-module-pagespeed_*.deb) and the
# pinned optimizer's pagespeed-optimizer_<pin version>_*.deb.  Nothing is
# fetched from the package repository then, and the module comes from its
# package, so a module.so argument is refused.  A missing package is an error,
# never a fall back to the repository.  What this cannot show: that the
# repository carries the packages, that they are signed, and that
# install.sh and apt resolve them as a customer's install does.
#
# Needs bash 4, python3 and docker.  Every container, the network and the
# origin image carry a prefix unique to this run and are removed on exit;
# nothing else is touched.
# A nginx module must be built for the packaged nginx (Ubuntu 24.04's
# nginx 1.24.0), as the release lane builds it.
if [ -z "${BASH_VERSINFO:-}" ] || [ "${BASH_VERSINFO[0]}" -lt 4 ]; then
  echo "run.sh needs bash 4 or newer (associative arrays, mapfile)" >&2
  exit 2
fi
set -u
SERVER=${1:?usage: run.sh apache|nginx [module.so]}
SO=${2:-}
# A job that means local packages (LOCAL_PACKAGES=true) must have staged them:
# an empty PACKAGE_DIR would otherwise run the published mode under a local name.
if [ "${LOCAL_PACKAGES:-}" = true ] && [ -z "${PACKAGE_DIR:-}" ]; then
  echo "run.sh: LOCAL_PACKAGES=true but PACKAGE_DIR is empty" >&2
  exit 2
fi
case "$SERVER" in apache|nginx) ;; *) echo "usage: run.sh apache|nginx [module.so]" >&2; exit 2 ;; esac
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
READY_TIMEOUT_S=${READY_TIMEOUT_S:-180}
STABLE_POLLS=${STABLE_POLLS:-10}
# SHARED_CACHE_PREFIX names this run's containers and network; it must be
# unique to the run (a CI job passes its own job-scoped name).
PREFIX="${SHARED_CACHE_PREFIX:-shared-cache-$$-${RANDOM}}"
ORIGIN="$PREFIX-origin"
NGINX_CACHE="$PREFIX-nginx"
VARNISH="$PREFIX-varnish"
NGINX_CLASS="$PREFIX-nginx-device-class"
VARNISH_CLASS="$PREFIX-varnish-device-class"
NET="$PREFIX-net"
IMAGE="$PREFIX-$SERVER"
# shellcheck disable=SC2329  # invoked by the EXIT trap
cleanup() {
  docker rm -f "$ORIGIN" "$NGINX_CACHE" "$VARNISH" "$NGINX_CLASS" "$VARNISH_CLASS" \
    >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
  docker image rm "$IMAGE" >/dev/null 2>&1 || true
}
trap cleanup EXIT

PAIR=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['tag'].removeprefix('optimizer-v'))" \
  "${OPTIMIZER_PIN:-$ROOT/install/debian/optimizer-pin.json}") || exit 2
# The staging directory is the build context; pkgs/ is empty unless the
# packages come from PACKAGE_DIR.
CTX=$(mktemp -d) || exit 2
trap 'rm -rf "$CTX"; cleanup' EXIT
mkdir "$CTX/pkgs"
cp "$HERE/Dockerfile.$SERVER" "$CTX/"
PACKAGES=published
if [ -n "${PACKAGE_DIR:-}" ]; then
  PACKAGES=local
  if [ -n "$SO" ]; then
    echo "run.sh: PACKAGE_DIR and a module.so argument are exclusive (the module comes from its package)" >&2
    exit 2
  fi
  case $SERVER in apache) MODPKG=mod-pagespeed ;; nginx) MODPKG=nginx-module-pagespeed ;; esac
  tilde='~'
  for pat in "${MODPKG}_*.deb" "pagespeed-optimizer_${PAIR/-/$tilde}_*.deb"; do
    # shellcheck disable=SC2086  # the pattern is a glob
    set -- "$PACKAGE_DIR"/$pat
    if [ $# -ne 1 ] || [ ! -f "$1" ]; then
      echo "run.sh: PACKAGE_DIR $PACKAGE_DIR must hold exactly one $pat" >&2
      exit 2
    fi
    cp "$1" "$CTX/pkgs/"
  done
fi
docker build -q -t "$IMAGE" --build-arg "PAIR_VERSION=$PAIR" --build-arg "PACKAGES=$PACKAGES" \
  -f "$CTX/Dockerfile.$SERVER" "$CTX" >/dev/null || exit 2
docker network create "$NET" >/dev/null || exit 2
EXTRA=()
if [ -n "$SO" ]; then
  EXTRA=(-v "$(cd "$(dirname "$SO")" && pwd)/$(basename "$SO"):/module.so:ro" -e MODULE_SO=/module.so)
fi
# The caches reach the origin by its network alias, so their configurations
# carry no per-run name.
docker run -d --name "$ORIGIN" --network "$NET" --network-alias origin "${EXTRA[@]}" \
  -v "$HERE:/harness:ro" "$IMAGE" bash /harness/start-origin.sh "$SERVER" >/dev/null || exit 2
# Local packages mean nothing came from the package repository: the image
# must carry no source of it (the published install adds one, a package
# built here must not).
if [ "$PACKAGES" = local ] && docker exec "$ORIGIN" grep -rqsi modpagespeed /etc/apt; then
  echo "FAIL: PACKAGE_DIR mode, but the image has a modpagespeed package source under /etc/apt"
  exit 1
fi
docker run -d --name "$NGINX_CACHE" --network "$NET" \
  -v "$HERE/nginx-cache.conf:/etc/nginx/conf.d/default.conf:ro" nginx:1.27-alpine >/dev/null || exit 2
docker run -d --name "$VARNISH" --network "$NET" \
  -v "$HERE/varnish.vcl:/etc/varnish/default.vcl:ro" varnish:7.5 >/dev/null || exit 2
docker run -d --name "$NGINX_CLASS" --network "$NET" \
  -v "$HERE/nginx-device-class.conf:/etc/nginx/conf.d/default.conf:ro" nginx:1.27-alpine >/dev/null || exit 2
docker run -d --name "$VARNISH_CLASS" --network "$NET" \
  -v "$HERE/varnish-device-class.vcl:/etc/varnish/default.vcl:ro" varnish:7.5 >/dev/null || exit 2

DESKTOP='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36'
PHONE='Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Mobile Safari/537.36'
PHONE2='Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1'
CLIENTS="desktop phone save-data"
in_origin() { docker exec "$ORIGIN" "$@"; }
client_args() {
  case $1 in
    desktop) printf '%s\n' -H "User-Agent: $DESKTOP" ;;
    phone) printf '%s\n' -H "User-Agent: $PHONE" ;;
    phone2) printf '%s\n' -H "User-Agent: $PHONE2" ;;
    save-data) printf '%s\n' -H "User-Agent: $DESKTOP" -H "Save-Data: on" ;;
  esac
}
# fetch URL CLIENT: request as CLIENT (from inside the origin container, which
# has curl and identify); leaves the body in /tmp/b and headers in /tmp/h.
fetch() {
  local args=()
  mapfile -t args < <(client_args "$2")
  in_origin curl -s --max-time 30 -D /tmp/h -o /tmp/b -H 'Host: localhost' -H 'Accept: image/jpeg' "${args[@]}" "$1"
}
header() { in_origin sh -c "grep -i '^$1:' /tmp/h | cut -d' ' -f2- | tr -d '\r'"; }
body_hash() { in_origin sha256sum /tmp/b | cut -c1-16; }
describe() {
  printf '%-9s %-9s %7s bytes  %-4s  Vary: %s\n' "$1" \
    "$(in_origin identify -format '%wx%h' /tmp/b 2>/dev/null)" "$(in_origin stat -c %s /tmp/b)" \
    "$(header x-proxy-cache)" "$(header vary)"
}

# Readiness, per client: the response came from the optimizer's cache (Age)
# as a selected variant (Vary), and its bytes held still for STABLE_POLLS
# polls a second apart.  A client whose own variant is not written yet is
# first served another one (a fallback) while the worker writes it, which
# takes seconds for a large image; the bytes change when it lands.  The
# fallback also carries Age and Vary and holds still, so stability alone is
# not enough: a client whose stable bytes equal an already-ready client's
# baseline is still on the fallback and is polled until it diverges.  The
# origin's own bytes are never a baseline either: the recorded copy can carry
# Age and Vary and hold still before the worker has written anything.
for _ in $(seq 1 30); do
  in_origin curl -s --max-time 30 -o /dev/null http://localhost/ && break
  sleep 1
done
origin_hash=$(in_origin sha256sum /var/www/html/big.jpg | cut -c1-16)
declare -A baseline
for c in $CLIENTS; do
  deadline=$((SECONDS + READY_TIMEOUT_S))
  last="" same=0 shared=""
  while :; do
    fetch http://localhost/big.jpg "$c"
    if [ -n "$(header age)" ] && [ -n "$(header vary)" ]; then
      h=$(body_hash)
      if [ "$h" = "$last" ]; then same=$((same + 1)); else same=0; last=$h; fi
      if [ "$same" -ge "$STABLE_POLLS" ]; then
        shared=""
        [ "$h" = "$origin_hash" ] && shared=" the origin"
        for o in "${!baseline[@]}"; do
          [ "${baseline[$o]}" = "$h" ] && shared="$shared $o"
        done
        if [ -z "$shared" ]; then
          baseline[$c]=$h
          break
        fi
      fi
    fi
    if [ "$SECONDS" -ge "$deadline" ]; then
      if [ -n "$shared" ]; then
        echo "FAIL: $c still serves the bytes of${shared} (hash $last) after ${READY_TIMEOUT_S}s"
      else
        echo "FAIL: $c got no stable optimized variant within ${READY_TIMEOUT_S}s"
      fi
      exit 1
    fi
    sleep 1
  done
done

fail=0
declare -A want
echo "== $SERVER: direct"
for c in $CLIENTS; do
  fetch http://localhost/big.jpg "$c"
  want[$c]=$(body_hash)
  describe "$c"
  if [ "$c" = desktop ]; then
    etag=$(header etag)
    vary_200=$(header vary)
  fi
  vary=",$(header vary | tr -d ' ' | tr '[:upper:]' '[:lower:]'),"
  for t in accept save-data user-agent sec-ch-dpr; do
    case $vary in *",$t,"*) ;; *) echo "  FAIL: Vary does not name $t"; fail=1 ;; esac
  done
done
mapfile -t args < <(client_args desktop)
in_origin curl -s --max-time 30 -D /tmp/h -o /dev/null -H 'Accept: image/jpeg' "${args[@]}" \
  -H "If-None-Match: $etag" http://localhost/big.jpg
echo "== $SERVER: desktop revalidation: $(in_origin head -1 /tmp/h | tr -d '\r')  Vary: $(header vary)"
if ! in_origin head -1 /tmp/h | grep -q ' 304'; then
  echo "  FAIL: the revalidation was not answered 304"
  fail=1
elif [ "$(header vary)" != "$vary_200" ]; then
  echo "  FAIL: the 304 names a different Vary than its 200"
  fail=1
fi
for cache in "$NGINX_CACHE" "$VARNISH" "$NGINX_CLASS" "$VARNISH_CLASS"; do
  echo "== $SERVER: through ${cache#"$PREFIX"-} (in this order, then desktop again)"
  for c in $CLIENTS; do
    fetch "http://$cache/big.jpg" "$c"
    describe "$c"
    got=$(body_hash)
    if [ "$got" != "${want[$c]}" ]; then
      who=none
      for o in $CLIENTS; do [ "${want[$o]}" = "$got" ] && who=$o; done
      echo "  FAIL: $c was served another client's variant (got the bytes of: $who, hash $got; its own baseline is ${want[$c]})"
      fail=1
    fi
  done
  # Positive control: the cache does store these responses.
  fetch "http://$cache/big.jpg" desktop
  describe desktop
  if [ "$(header x-proxy-cache)" != HIT ] || [ "$(body_hash)" != "${want[desktop]}" ]; then
    echo "  FAIL: the repeated desktop request was not a cache hit with its own bytes"
    fail=1
  fi
  case $cache in
    "$NGINX_CLASS"|"$VARNISH_CLASS")
      # Another phone: same class, so the phone's stored copy, from the cache.
      fetch "http://$cache/big.jpg" phone2
      describe phone2
      if [ "$(header x-proxy-cache)" != HIT ] || [ "$(body_hash)" != "${want[phone]}" ]; then
        echo "  FAIL: a second phone was not a cache hit with the phone's bytes"
        fail=1
      fi
      ;;
  esac
done
if [ $fail -eq 0 ]; then echo "PASS"; else echo "FAIL"; fi
exit $fail
