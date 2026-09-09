#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Smoke a sidecar matched (nginx + ngx_pagespeed_module.so) pair.
#
# Runs the pair inside a clean distro container (no host nginx) and asserts, in
# order of strength:
#   1. LOAD   — `nginx -t` succeeds (the module dlopen's + the generated config is
#               valid). This is the portability gate when run on debian:11/debian:12.
#   2. SERVE  — nginx starts and answers on the listen port.
#   3. OPTIMIZE — a CSS-referencing page comes back with the `X-Page-Speed` header
#               (the optimizer is active in the request path), and the header VALUE
#               is exactly the version this tree stamps (derived from
#               net/instaweb/public/VERSION; override via EXPECTED_XPS, fallback
#               "1.15.0" when run detached from a checkout). Catches a pair built
#               with a stale/unpatched VERSION (a "-beta.1" sidecar).
#   4. NO-WARN — no optimized response carries an `X-PageSpeed-Warn` header. The
#               module has no license apparatus, so there is nothing to
#               stage and no licensed/eval split: the header must be ABSENT on
#               every optimized response, and its presence is a regression.
#
# Usage: [EXPECTED_XPS=<version>] check-sidecar-pair.sh <pairdir> <docker-image>
#   <pairdir>       dir containing nginx + ngx_pagespeed_module.so
#   <docker-image>  e.g. debian:11, debian:12 (the --platform is the host's arch;
#                   callers that cross-build pass --platform via DOCKER_DEFAULT_PLATFORM)
#   EXPECTED_XPS    optional expected X-Page-Speed value; overrides the VERSION-file
#                   derivation
set -uo pipefail

PAIR="${1:?usage: check-sidecar-pair.sh <pairdir> <image>}"
IMAGE="${2:?missing docker image}"
PLAT_ARG=""
[ -n "${DOCKER_PLATFORM:-}" ] && PLAT_ARG="--platform ${DOCKER_PLATFORM}"

[ -f "$PAIR/nginx" ] && [ -f "$PAIR/ngx_pagespeed_module.so" ] || { echo "FATAL: missing pair in $PAIR"; exit 1; }

# Expected X-Page-Speed value: the version the tree stamps into the module
# (version.h.in: MAJOR.MINOR.BUILD plus "-PRERELEASE" when non-empty). The
# workflow patches net/instaweb/public/VERSION before building, so deriving
# from the same file asserts the pair matches the tree that built it.
if [ -z "${EXPECTED_XPS:-}" ]; then
  VERSION_FILE="$(cd "$(dirname "$0")/../.." 2>/dev/null && pwd)/net/instaweb/public/VERSION"
  if [ -f "$VERSION_FILE" ]; then
    EXPECTED_XPS=$(
      . "$VERSION_FILE"
      printf '%s.%s.%s%s' "$MAJOR" "$MINOR" "$BUILD" "${PRERELEASE:+-$PRERELEASE}"
    )
  else
    EXPECTED_XPS="1.15.0"
  fi
fi

echo "===== sidecar pair smoke: $IMAGE (expect X-Page-Speed ${EXPECTED_XPS}) ====="
# shellcheck disable=SC2086
docker run --rm --init $PLAT_ARG \
  -v "$PAIR:/p:ro" \
  -e EXPECTED_XPS="$EXPECTED_XPS" \
  "$IMAGE" bash -c '
    set -e; export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null 2>&1
    apt-get install -y -qq libpcre3 zlib1g curl procps >/dev/null 2>&1
    PN=$(grep -h "^PRETTY_NAME" /etc/os-release | cut -d= -f2)
    echo "  distro: $PN"

    R=/tmp/ps; mkdir -p "$R/logs" "$R/cache" "$R/www"
    cp /p/nginx /tmp/nginx; chmod +x /tmp/nginx
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
    #    the X-Page-Speed header (optimizer active in the request path) AND that its
    #    value is exactly the expected version (a presence-only check let a pair
    #    built from an unpatched VERSION ship a prerelease stamp —).
    HDRS=""
    for i in $(seq 1 12); do
      HDRS=$(curl -fsS -D - -o /dev/null "http://127.0.0.1:8099/" 2>/dev/null || true)
      echo "$HDRS" | grep -iq "^X-Page-Speed:" && break
      sleep 1
    done
    if echo "$HDRS" | grep -iq "^X-Page-Speed:"; then
      XPS=$(echo "$HDRS" | grep -i "^X-Page-Speed:" | head -1 | cut -d: -f2- | tr -d "[:space:]")
      if [ "$XPS" = "$EXPECTED_XPS" ]; then
        echo "    OPTIMIZE: X-Page-Speed: $XPS (version matches)"
      else
        echo "  >>> VERSION FAILED: X-Page-Speed is \"$XPS\", expected \"$EXPECTED_XPS\""; exit 1
      fi
    else
      echo "  >>> OPTIMIZE FAILED (no X-Page-Speed header after warmup)"; echo "$HDRS" | sed "s/^/    /"; exit 1
    fi

    # 4. NO-WARN — the module has no license apparatus, so no optimized response
    #    may carry X-PageSpeed-Warn. Judge the response that passed step 3 plus a
    #    handful of follow-up requests (the retired license state used to settle
    #    over the first few responses; a regression that reintroduced the header
    #    would surface in the same window), and only judge OPTIMIZED responses —
    #    a non-optimized reply has nothing to assert on either way.
    JUDGED=0
    for i in $(seq 1 6); do
      if [ "$i" -gt 1 ]; then
        sleep 1
        HDRS=$(curl -fsS -D - -o /dev/null "http://127.0.0.1:8099/" 2>/dev/null || true)
      fi
      echo "$HDRS" | grep -iq "^X-Page-Speed:" || continue
      JUDGED=$((JUDGED + 1))
      WARN=$(echo "$HDRS" | grep -i "^X-PageSpeed-Warn:" || true)
      [ -z "$WARN" ] || { echo "  >>> NO-WARN FAILED (X-PageSpeed-Warn on an optimized response): $WARN"; exit 1; }
    done
    [ "$JUDGED" -ge 1 ] || { echo "  >>> NO-WARN FAILED (no optimized response to judge)"; exit 1; }
    echo "    NO-WARN: ${JUDGED} optimized response(s), no X-PageSpeed-Warn"

    kill -TERM $NGX 2>/dev/null || true
    echo "  PAIR-SMOKE-OK ($PN)"
  ' || { echo ">>> pair smoke FAILED on $IMAGE"; exit 1; }
