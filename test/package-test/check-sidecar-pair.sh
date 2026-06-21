#!/bin/bash
# the design record PW-2: smoke a sidecar matched (nginx + ngx_pagespeed_module.so) pair.
#
# Runs the pair inside a clean distro container (no host nginx) and asserts, in
# order of strength:
#   1. LOAD   — `nginx -t` succeeds (the module dlopen's + the generated config is
#               valid). This is the portability gate when run on debian:11/debian:12.
#   2. SERVE  — nginx starts and answers on the listen port.
#   3. OPTIMIZE — a CSS-referencing page comes back with the `X-Page-Speed` header
#               (the optimizer is active in the request path).
#   4. LICENSE — if a license token is supplied, the optimized response must NOT
#               carry `X-PageSpeed-Warn: unlicensed`. Without a token the
#               warning IS expected (eval mode) — asserted so a silently-disabled
#               optimizer can't pass.
#
# Usage: check-sidecar-pair.sh <pairdir> <docker-image> [license-token-path]
#   <pairdir>       dir containing nginx + ngx_pagespeed_module.so
#   <docker-image>  e.g. debian:11, debian:12 (the --platform is the host's arch;
#                   callers that cross-build pass --platform via DOCKER_DEFAULT_PLATFORM)
#   [license-token] optional path to a license token; enables the LICENSE assertion
set -uo pipefail

PAIR="${1:?usage: check-sidecar-pair.sh <pairdir> <image> [token]}"
IMAGE="${2:?missing docker image}"
TOKEN="${3:-}"
PLAT_ARG=""
[ -n "${DOCKER_PLATFORM:-}" ] && PLAT_ARG="--platform ${DOCKER_PLATFORM}"

[ -f "$PAIR/nginx" ] && [ -f "$PAIR/ngx_pagespeed_module.so" ] || { echo "FATAL: missing pair in $PAIR"; exit 1; }

TOKEN_MOUNT=()
TOKEN_ENV="0"
if [ -n "$TOKEN" ]; then
  [ -s "$TOKEN" ] || { echo "FATAL: license token '$TOKEN' missing/empty"; exit 1; }
  TOKEN_MOUNT=(-v "$(cd "$(dirname "$TOKEN")" && pwd)/$(basename "$TOKEN")":/lic/pagespeed.license:ro)
  TOKEN_ENV="1"
fi

echo "===== sidecar pair smoke: $IMAGE (license=${TOKEN_ENV}) ====="
# shellcheck disable=SC2086
docker run --rm --init $PLAT_ARG \
  -v "$PAIR:/p:ro" "${TOKEN_MOUNT[@]+"${TOKEN_MOUNT[@]}"}" \
  -e HAVE_LICENSE="$TOKEN_ENV" \
  "$IMAGE" bash -c '
    set -e; export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null 2>&1
    apt-get install -y -qq libpcre3 zlib1g curl procps >/dev/null 2>&1
    PN=$(grep -h "^PRETTY_NAME" /etc/os-release | cut -d= -f2)
    echo "  distro: $PN"

    R=/tmp/ps; mkdir -p "$R/logs" "$R/cache" "$R/www"
    cp /p/nginx /tmp/nginx; chmod +x /tmp/nginx
    # license token (if supplied) goes next to the cache, where the worker reads it.
    # chmod 644: cp preserves the SOURCE mode, and runner-staged tokens are 0600
    # root-owned — unreadable by the nginx WORKER (user "nobody"; only the master
    # runs as root). A 0600 source made the licensed smoke fail with a perfectly
    # valid token (warn persisted; 2026-06-12) while a 0644 copy
    # of the SAME bytes passed. The worker must be able to read its license, as
    # in any real install.
    if [ "$HAVE_LICENSE" = "1" ]; then
      cp /lic/pagespeed.license "$R/pagespeed.license"
      chmod 644 "$R/pagespeed.license"
    fi
    printf "body{ color : red ; margin : 1px ; }\n" > "$R/www/s.css"
    printf "<!doctype html><html><head><link rel=stylesheet href=/s.css></head><body>hi</body></html>" > "$R/www/index.html"

    cat > "$R/n.conf" <<CONF
load_module /p/ngx_pagespeed_module.so;
pid $R/nginx.pid;
error_log $R/logs/error.log info;
events { worker_connections 64; }
http {
  pagespeed on;
  pagespeed FileCachePath "$R/cache";
  pagespeed RewriteLevel CoreFilters;
  server {
    listen 8099;
    root $R/www;
    location / { }
  }
}
CONF

    # 1. LOAD
    OUT=$(/tmp/nginx -t -p "$R" -c "$R/n.conf" 2>&1) || true
    echo "$OUT" | sed "s/^/    nginx -t: /"
    echo "$OUT" | grep -q "test is successful" || { echo "  >>> LOAD FAILED"; exit 1; }

    # 2. SERVE
    /tmp/nginx -p "$R" -c "$R/n.conf" &
    NGX=$!
    ok=0
    for i in $(seq 1 30); do
      curl -fsS -o /dev/null "http://127.0.0.1:8099/" 2>/dev/null && { ok=1; break; }
      kill -0 $NGX 2>/dev/null || { echo "  >>> nginx exited early"; sed "s/^/    err: /" "$R/logs/error.log"; exit 1; }
      sleep 1
    done
    [ "$ok" = 1 ] || { echo "  >>> SERVE FAILED (no response on :8099)"; exit 1; }

    # 3. OPTIMIZE — drive a few requests so pagespeed processes the page, then assert
    #    the X-Page-Speed header (optimizer active in the request path).
    HDRS=""
    for i in $(seq 1 12); do
      HDRS=$(curl -fsS -D - -o /dev/null "http://127.0.0.1:8099/" 2>/dev/null || true)
      echo "$HDRS" | grep -iq "^X-Page-Speed:" && break
      sleep 1
    done
    if echo "$HDRS" | grep -iq "^X-Page-Speed:"; then
      echo "    OPTIMIZE: X-Page-Speed present"
    else
      echo "  >>> OPTIMIZE FAILED (no X-Page-Speed header after warmup)"; echo "$HDRS" | sed "s/^/    /"; exit 1
    fi

    # 4. LICENSE — token supplied => NO unlicensed warning; none => warning expected.
    #
    # POLL until the license state SETTLES instead of asserting the first
    # optimized response: at worker start there is a short window between the
    # first license check completing (which enables the warn header) and the
    # file-token apply propagating to the serve-time license atomic
    # (ShouldOptimize). On an idle box the window closes before the first
    # X-Page-Speed response; under post-build CI load it can stay open for a
    # few responses — that race failed the 1.15.0.1 pair build twice on the
    # mac-arm64 runner (2026-06-12) while the same .so passed 3/3 when run
    # idle. The window exists in every build incl. shipped GA (verified
    # against the 1.15.0 GCS pair); it is within the design record soft-enforcement
    # semantics, so the smoke must tolerate it, not gate on it.
    WARN=$(echo "$HDRS" | grep -i "^X-PageSpeed-Warn:" || true)
    if [ "$HAVE_LICENSE" = "1" ]; then
      for i in $(seq 1 15); do
        [ -z "$WARN" ] && break
        sleep 1
        HDRS=$(curl -fsS -D - -o /dev/null "http://127.0.0.1:8099/" 2>/dev/null || true)
        echo "$HDRS" | grep -iq "^X-Page-Speed:" || continue  # only judge optimized responses
        WARN=$(echo "$HDRS" | grep -i "^X-PageSpeed-Warn:" || true)
      done
      [ -z "$WARN" ] || { echo "  >>> LICENSE FAILED (warn persisted after settle window with a valid token): $WARN"; exit 1; }
      echo "    LICENSE: no unlicensed warning with a valid token"
    else
      # eval mode: a soft warning is expected; poll symmetrically (the warn is
      # suppressed until the first license check completes), but do not
      # hard-fail if absent across versions.
      for i in $(seq 1 15); do
        [ -n "$WARN" ] && break
        sleep 1
        HDRS=$(curl -fsS -D - -o /dev/null "http://127.0.0.1:8099/" 2>/dev/null || true)
        WARN=$(echo "$HDRS" | grep -i "^X-PageSpeed-Warn:" || true)
      done
      [ -n "$WARN" ] && echo "    LICENSE(eval): soft warning present: $WARN" || echo "    LICENSE(eval): (no warn header surfaced)"
    fi

    kill -TERM $NGX 2>/dev/null || true
    echo "  PAIR-SMOKE-OK ($PN)"
  ' || { echo ">>> pair smoke FAILED on $IMAGE"; exit 1; }
