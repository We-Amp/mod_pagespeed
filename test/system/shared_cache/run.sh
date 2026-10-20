#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Shared-cache check for daemon-served image variants.
#
#   test/system/shared_cache/run.sh apache|nginx [path/to/module.so]
#
# Runs the web server with the published module and optimizer packages
# (Dockerfile.apache / Dockerfile.nginx), optionally with a locally built
# module swapped in, behind two real shared caches: nginx proxy_cache and
# Varnish, both with their stock Vary handling.  One URL is requested as a
# desktop browser, a phone and a desktop with Save-Data, first directly (once
# the worker has written each variant) and then through each cache in that
# order.  The check fails when a cache hands a client bytes that differ from
# what the origin sends that client, or when a response's Vary does not name
# Accept, Save-Data, User-Agent and Sec-CH-DPR, or when the 304 names a
# different set than its 200.  Needs docker; containers and the network it
# creates are named shared-cache-* and removed on exit.
set -u
SERVER=${1:?usage: run.sh apache|nginx [module.so]}
SO=${2:-}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NET=shared-cache-net
IMAGE=shared-cache-$SERVER
cleanup() {
  docker rm -f shared-cache-origin shared-cache-nginx shared-cache-varnish >/dev/null 2>&1 || true
  docker network rm $NET >/dev/null 2>&1 || true
}
trap cleanup EXIT
cleanup
docker build -q -t "$IMAGE" -f "$HERE/Dockerfile.$SERVER" "$HERE" >/dev/null || exit 2
docker network create $NET >/dev/null || exit 2
EXTRA=()
if [ -n "$SO" ]; then
  EXTRA=(-v "$(cd "$(dirname "$SO")" && pwd)/$(basename "$SO"):/module.so:ro" -e MODULE_SO=/module.so)
fi
docker run -d --name shared-cache-origin --network $NET "${EXTRA[@]}" -v "$HERE:/harness:ro" \
  "$IMAGE" bash /harness/start-origin.sh "$SERVER" >/dev/null || exit 2
docker run -d --name shared-cache-nginx --network $NET \
  -v "$HERE/nginx-cache.conf:/etc/nginx/conf.d/default.conf:ro" nginx:1.27-alpine >/dev/null || exit 2
docker run -d --name shared-cache-varnish --network $NET \
  -v "$HERE/varnish.vcl:/etc/varnish/default.vcl:ro" varnish:7.5 >/dev/null || exit 2
sleep 4

DESKTOP='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36'
PHONE='Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Mobile Safari/537.36'
CLIENTS="desktop phone save-data"
in_origin() { docker exec shared-cache-origin "$@"; }
client_args() {
  case $1 in
    desktop) printf '%s\n' -H "User-Agent: $DESKTOP" ;;
    phone) printf '%s\n' -H "User-Agent: $PHONE" ;;
    save-data) printf '%s\n' -H "User-Agent: $DESKTOP" -H "Save-Data: on" ;;
  esac
}
# fetch URL CLIENT: request as CLIENT (from inside the origin container, which
# has curl and identify); leaves the body in /tmp/b and headers in /tmp/h.
fetch() {
  local args=()
  mapfile -t args < <(client_args "$2")
  in_origin curl -s -D /tmp/h -o /tmp/b -H 'Host: localhost' -H 'Accept: image/jpeg' "${args[@]}" "$1"
}
header() { in_origin sh -c "grep -i '^$1:' /tmp/h | cut -d' ' -f2- | tr -d '\r'"; }
describe() {
  printf '%-9s %-9s %7s bytes  %-4s  Vary: %s\n' "$1" \
    "$(in_origin identify -format '%wx%h' /tmp/b 2>/dev/null)" "$(in_origin stat -c %s /tmp/b)" \
    "$(header x-proxy-cache)" "$(header vary)"
}

# Let the worker write each client's variant.
for _ in $(seq 1 40); do
  for c in $CLIENTS; do fetch http://localhost/big.jpg "$c"; done
  sleep 1
done

fail=0
declare -A want
echo "== $SERVER: direct"
for c in $CLIENTS; do
  fetch http://localhost/big.jpg "$c"
  want[$c]=$(in_origin sha256sum /tmp/b | cut -c1-16)
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
in_origin curl -s -D /tmp/h -o /dev/null -H 'Accept: image/jpeg' "${args[@]}" \
  -H "If-None-Match: $etag" http://localhost/big.jpg
echo "== $SERVER: desktop revalidation: $(in_origin head -1 /tmp/h | tr -d '\r')  Vary: $(header vary)"
if ! in_origin head -1 /tmp/h | grep -q ' 304'; then
  echo "  FAIL: the revalidation was not answered 304"
  fail=1
elif [ "$(header vary)" != "$vary_200" ]; then
  echo "  FAIL: the 304 names a different Vary than its 200"
  fail=1
fi
for cache in shared-cache-nginx shared-cache-varnish; do
  echo "== $SERVER: through ${cache#shared-cache-} (in this order)"
  for c in $CLIENTS; do
    fetch "http://$cache/big.jpg" "$c"
    describe "$c"
    got=$(in_origin sha256sum /tmp/b | cut -c1-16)
    if [ "$got" != "${want[$c]}" ]; then
      echo "  FAIL: $c was served another client's variant"
      fail=1
    fi
  done
done
[ $fail -eq 0 ] && echo "PASS" || echo "FAIL"
exit $fail
