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
# stable optimized variant, then through each cache in that order, then once
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
  "$ROOT/install/debian/optimizer-pin.json") || exit 2
docker build -q -t "$IMAGE" --build-arg "PAIR_VERSION=$PAIR" \
  -f "$HERE/Dockerfile.$SERVER" "$HERE" >/dev/null || exit 2
docker network create "$NET" >/dev/null || exit 2
EXTRA=()
if [ -n "$SO" ]; then
  EXTRA=(-v "$(cd "$(dirname "$SO")" && pwd)/$(basename "$SO"):/module.so:ro" -e MODULE_SO=/module.so)
fi
# The caches reach the origin by its network alias, so their configurations
# carry no per-run name.
docker run -d --name "$ORIGIN" --network "$NET" --network-alias origin "${EXTRA[@]}" \
  -v "$HERE:/harness:ro" "$IMAGE" bash /harness/start-origin.sh "$SERVER" >/dev/null || exit 2
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
# takes seconds for a large image; the bytes change when it lands.
for _ in $(seq 1 30); do
  in_origin curl -s --max-time 30 -o /dev/null http://localhost/ && break
  sleep 1
done
for c in $CLIENTS; do
  deadline=$((SECONDS + READY_TIMEOUT_S))
  last="" same=0
  while :; do
    fetch http://localhost/big.jpg "$c"
    if [ -n "$(header age)" ] && [ -n "$(header vary)" ]; then
      h=$(body_hash)
      if [ "$h" = "$last" ]; then same=$((same + 1)); else same=0; last=$h; fi
      [ "$same" -ge "$STABLE_POLLS" ] && break
    fi
    if [ "$SECONDS" -ge "$deadline" ]; then
      echo "FAIL: $c got no stable optimized variant within ${READY_TIMEOUT_S}s"
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
    if [ "$(body_hash)" != "${want[$c]}" ]; then
      echo "  FAIL: $c was served another client's variant"
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
