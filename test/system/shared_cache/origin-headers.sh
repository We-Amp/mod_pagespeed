#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
#
# Store decisions use the stricter of the origin's and the server's caching
# headers: a response is kept in the optimizer's shared cache only when both
# allow it, for the shorter of the two lifetimes.
#
#   test/system/shared_cache/origin-headers.sh apache|nginx [path/to/module.so]
#
# The web server and the optimizer come from the packages run.sh uses (the
# same Dockerfiles, PACKAGE_DIR and OPTIMIZER_PIN), optionally with a locally
# built module swapped in.  An application origin on a loopback port serves
# images, each with its own Cache-Control; the server in front proxies them
# and sets its own caching headers for browsers (the site confs next to this
# script).  Every request is made from inside the container.  The check fails
# when
#   * a response that the origin or the server does not allow a shared cache
#     to keep is answered from the shared cache (an Age header) or with other
#     bytes than the origin's, on any of the repeated requests;
#   * the field the client gets is not the server's (the client-facing
#     response must stay as the server configures it);
#   * a response both allow storing is never answered from the shared cache
#     (the positive control; the field a served copy carries is printed, not
#     checked);
#   * a stored copy is served with an Age past the shorter of the two
#     lifetimes.
#
# Needs bash 4 and docker.  Containers and the image carry a prefix unique to
# the run and are removed on exit.
if [ -z "${BASH_VERSINFO:-}" ] || [ "${BASH_VERSINFO[0]}" -lt 4 ]; then
  echo "origin-headers.sh needs bash 4 or newer" >&2
  exit 2
fi
set -u
SERVER=${1:?usage: origin-headers.sh apache|nginx [module.so]}
SO=${2:-}
case "$SERVER" in apache|nginx) ;; *) echo "usage: origin-headers.sh apache|nginx [module.so]" >&2; exit 2 ;; esac
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
REPEATS=${REPEATS:-12}
STORE_TIMEOUT_S=${STORE_TIMEOUT_S:-90}
PREFIX="${SHARED_CACHE_PREFIX:-origin-headers-$$-${RANDOM}}"
ORIGIN="$PREFIX-origin"
IMAGE="$PREFIX-$SERVER"
cleanup() {
  docker rm -f "$ORIGIN" >/dev/null 2>&1 || true
  docker image rm "$IMAGE" >/dev/null 2>&1 || true
  [ -n "${CTX:-}" ] && rm -rf "$CTX"
}
trap cleanup EXIT

PAIR=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['tag'].removeprefix('optimizer-v'))" \
  "${OPTIMIZER_PIN:-$ROOT/install/debian/optimizer-pin.json}") || exit 2
CTX=$(mktemp -d) || exit 2
mkdir "$CTX/pkgs"
cp "$HERE/Dockerfile.$SERVER" "$CTX/"
PACKAGES=published
if [ -n "${PACKAGE_DIR:-}" ]; then
  PACKAGES=local
  if [ -n "$SO" ]; then
    echo "origin-headers.sh: PACKAGE_DIR and a module.so argument are exclusive" >&2
    exit 2
  fi
  case $SERVER in apache) MODPKG=mod-pagespeed ;; nginx) MODPKG=nginx-module-pagespeed ;; esac
  tilde='~'
  for pat in "${MODPKG}_*.deb" "pagespeed-optimizer_${PAIR/-/$tilde}_*.deb"; do
    # shellcheck disable=SC2086  # the pattern is a glob
    set -- "$PACKAGE_DIR"/$pat
    if [ $# -ne 1 ] || [ ! -f "$1" ]; then
      echo "origin-headers.sh: PACKAGE_DIR $PACKAGE_DIR must hold exactly one $pat" >&2
      exit 2
    fi
    cp "$1" "$CTX/pkgs/"
  done
fi
docker build -q -t "$IMAGE" --build-arg "PAIR_VERSION=$PAIR" --build-arg "PACKAGES=$PACKAGES" \
  -f "$CTX/Dockerfile.$SERVER" "$CTX" >/dev/null || exit 2
EXTRA=()
if [ -n "$SO" ]; then
  EXTRA=(-v "$(cd "$(dirname "$SO")" && pwd)/$(basename "$SO"):/module.so:ro" -e MODULE_SO=/module.so)
fi
docker run -d --name "$ORIGIN" -e SITE=origin-headers "${EXTRA[@]}" \
  -v "$HERE:/harness:ro" "$IMAGE" bash /harness/start-origin.sh "$SERVER" >/dev/null || exit 2

DESKTOP='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36'
in_origin() { docker exec "$ORIGIN" "$@"; }
# fetch NAME [PREFIX]: the front server's /PREFIX/NAME.jpg (PREFIX e by
# default) as a desktop browser; leaves the body in /tmp/b and the headers in
# /tmp/h.
fetch() {
  in_origin curl -s --max-time 30 -D /tmp/h -o /tmp/b -H 'Host: localhost' \
    -H 'Accept: image/jpeg' -H "User-Agent: $DESKTOP" "http://localhost/${2:-e}/$1.jpg"
}
header() { in_origin sh -c "grep -i '^$1:' /tmp/h | cut -d' ' -f2- | tr -d '\r'"; }
status() { in_origin sh -c "head -1 /tmp/h | cut -d' ' -f2"; }
body_hash() { in_origin sha256sum /tmp/b | cut -c1-16; }
origin_hash() { in_origin sha256sum "/var/www/oh-origin/$1.jpg" | cut -c1-16; }
DIRECTIVE="max-age=3600"

for _ in $(seq 1 60); do
  in_origin curl -s --max-time 5 -o /dev/null http://127.0.0.1:8081/public.jpg && \
    in_origin curl -s --max-time 5 -o /dev/null http://localhost/ && break
  sleep 1
done

fail=0
say_fail() { echo "  FAIL: $*"; fail=1; }

# never_stored NAME LABEL [PREFIX [FIELD]]: the origin or the server does not
# allow a shared cache to keep the response, so every request reaches the
# origin and gets its bytes, under the server's field (one Cache-Control line
# equal to DIRECTIVE by default; with FIELD, any Cache-Control line equal to
# it).
never_stored() {
  local name=$1 prefix=${3:-e} want_field=${4:-} want
  want=$(origin_hash "$name")
  echo "== $SERVER: /$prefix/$name.jpg ($2), $REPEATS requests a second apart"
  local i stored=0 wrong=0 field=0
  for i in $(seq 1 "$REPEATS"); do
    fetch "$name" "$prefix"
    [ "$(status)" = 200 ] || { say_fail "request $i: status $(status)"; return; }
    [ -n "$(header age)" ] && stored=$((stored + 1))
    [ "$(body_hash)" = "$want" ] || wrong=$((wrong + 1))
    if [ -z "$want_field" ]; then
      [ "$(header cache-control)" = "$DIRECTIVE" ] || field=$((field + 1))
    else
      header cache-control | grep -qx -- "$want_field" || field=$((field + 1))
    fi
    sleep 1
  done
  echo "   last: Cache-Control: $(header cache-control | paste -sd'|')  Age: $(header age)  bytes: $(in_origin stat -c %s /tmp/b)"
  [ $stored -eq 0 ] || say_fail "$stored of $REPEATS answered from the shared cache (Age)"
  [ $wrong -eq 0 ] || say_fail "$wrong of $REPEATS not the origin's bytes"
  [ $field -eq 0 ] || say_fail "$field of $REPEATS without the server's Cache-Control"
}

# Polls NAME until it is answered from the shared cache; 0 when it was.
until_stored() {
  local deadline=$((SECONDS + STORE_TIMEOUT_S))
  while [ $SECONDS -lt "$deadline" ]; do
    fetch "$1"
    [ -n "$(header age)" ] && return 0
    sleep 1
  done
  return 1
}

never_stored private "origin: private, max-age=600"
never_stored nostore "origin: no-store"
never_stored guard "origin: public, max-age=600; server: no-store" n no-store

echo "== $SERVER: /e/public.jpg (origin: public, max-age=600)"
if until_stored public; then
  # Not compared with the server's field: a copy served from the shared
  # cache states its own stored freshness on Apache, where the server's
  # caching headers do not reach the served response.
  echo "   Cache-Control: $(header cache-control)  Age: $(header age)"
else
  say_fail "never answered from the shared cache within ${STORE_TIMEOUT_S}s"
fi

echo "== $SERVER: /e/short.jpg (origin: public, max-age=15), 45 s of requests"
if until_stored short; then
  max_age=0
  end=$((SECONDS + 45))
  while [ $SECONDS -lt $end ]; do
    fetch short
    a=$(header age)
    if [ -n "$a" ] && [ "$a" -gt "$max_age" ]; then max_age=$a; fi
    sleep 1
  done
  echo "   largest Age seen: $max_age"
  [ "$max_age" -le 16 ] || say_fail "served from the shared cache at Age $max_age, past the shorter lifetime (15 s)"
else
  say_fail "never answered from the shared cache within ${STORE_TIMEOUT_S}s"
fi

if [ $fail -eq 0 ]; then echo "PASS"; else echo "FAIL"; fi
exit $fail
