#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# System test: the cache key scheme.  A page fetched over HTTP/1.1 with TLS
# and the same page fetched over HTTP/2 with TLS must share one cache entry
# (scheme "https"); the same host and URL over plain HTTP is a different entry
# (scheme "http").  With pagespeed_trust_x_forwarded_proto on, a valid
# X-Forwarded-Proto decides the scheme instead; with it off the header is
# ignored.
#
# The cache is seeded under one scheme only and every request is sent with the
# same Host header, so an "X-PageSpeed: HIT" shows which scheme the request was
# keyed under.
#
# This is a manual system test (not run in CI).  Build the module the way it
# ships: against nginx source configured with --with-compat only.  Load it into
# an nginx binary of the same version built with --with-compat plus the SSL and
# v2 modules.
#
# Needs an nginx built with --with-compat, the SSL and v2 modules, the module
# built against that same nginx source, and curl with HTTP/2 support.
#
# Usage:
#   NGINX_BIN=/path/to/nginx MODULE_SO=/path/to/ngx_pagespeed_module.so \
#   SEED_TOOL=/path/to/seed_test_cache CURL=/path/to/curl \
#     tools/test-scheme-cache-key.sh
set -uo pipefail

: "${NGINX_BIN:?set NGINX_BIN}"
: "${MODULE_SO:?set MODULE_SO}"
: "${SEED_TOOL:?set SEED_TOOL}"
CURL="${CURL:-curl}"
TLS_PORT="${TLS_PORT:-18453}"
PLAIN_PORT="${PLAIN_PORT:-18455}"
ORIGIN_PORT="${ORIGIN_PORT:-18454}"
FWD_TLS_PORT="${FWD_TLS_PORT:-18456}"
FWD_PLAIN_PORT="${FWD_PLAIN_PORT:-18457}"
HOST_HDR="scheme.test"

WORK="$(mktemp -d)"
NGINX_PID=""
FAILED=0

cleanup() {
  if [ -n "${NGINX_PID}" ] && kill -0 "${NGINX_PID}" 2>/dev/null; then
    kill "${NGINX_PID}" 2>/dev/null || true
    wait "${NGINX_PID}" 2>/dev/null || true
  fi
  rm -rf "${WORK}"
}
trap cleanup EXIT

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILED=1; }

mkdir -p "${WORK}/logs"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=localhost" \
  -keyout "${WORK}/key.pem" -out "${WORK}/cert.pem" >/dev/null 2>&1 \
  || { echo "openssl could not create a certificate"; exit 2; }

# One volume shared by all listeners (the module opens a single cache).  A MISS
# is recorded as it passes through, so every case uses its own URL.
VOL="${WORK}/cache.vol"
"${SEED_TOOL}" --cache "${VOL}" --create_only >/dev/null || exit 2
# Each server block reads only its own entries, so every entry is seeded for
# the block (listener) the case asks; a MISS below is then the scheme's doing.
seed() {  # volume url scheme port
  "${SEED_TOOL}" --cache "$1" --url "$2" --host "${HOST_HDR}" --scheme "$3" \
    --site_listen "127.0.0.1:$4" --site_names "localhost,${HOST_HDR}" \
    --content "h1{color:red}" --content_type css --mask 0x08 >/dev/null \
    || { echo "seeding $2 failed"; exit 2; }
}
for u in /https-only-a.css /https-only-b.css /https-only-e.css; do
  seed "${VOL}" "${u}" https "${TLS_PORT}"
done
for u in /https-only-c.css /https-only-d.css /https-only-f.css; do
  seed "${VOL}" "${u}" https "${PLAIN_PORT}"
done
seed "${VOL}" /http-only-a.css http "${PLAIN_PORT}"
for u in /http-only-a.css /http-only-b.css /http-only-c.css; do
  seed "${VOL}" "${u}" http "${TLS_PORT}"
done
# Entries for the listeners that trust X-Forwarded-Proto.
seed "${VOL}" /fwd-https-a.css https "${FWD_PLAIN_PORT}"
seed "${VOL}" /fwd-https-b.css https "${FWD_TLS_PORT}"
seed "${VOL}" /fwd-http-a.css http "${FWD_TLS_PORT}"
seed "${VOL}" /fwd-http-b.css http "${FWD_PLAIN_PORT}"

chmod -R a+rwX "${WORK}"

server_block() {  # tls-or-plain port volume trust
  local listen="listen 127.0.0.1:$2;" http2=""
  if [ "$1" = tls ]; then
    listen="listen 127.0.0.1:$2 ssl;"
    http2="http2 on;
    ssl_certificate ${WORK}/cert.pem;
    ssl_certificate_key ${WORK}/key.pem;
    ssl_protocols TLSv1.3;"
  fi
  cat <<BLOCK
  server {
    ${listen}
    ${http2}
    server_name localhost ${HOST_HDR};
    pagespeed on;
    pagespeed_cache_path $3;
    pagespeed_trust_x_forwarded_proto $4;
    location / {
      proxy_pass http://127.0.0.1:${ORIGIN_PORT};
      proxy_set_header Accept-Encoding "";
    }
  }
BLOCK
}

{
cat <<CONF
load_module ${MODULE_SO};
user $(id -un);
daemon off;
pid ${WORK}/nginx.pid;
error_log ${WORK}/logs/error.log info;
events { worker_connections 64; }
http {
  access_log off;
  client_body_temp_path ${WORK}/body;
  proxy_temp_path ${WORK}/proxy;
  fastcgi_temp_path ${WORK}/fcgi;
  uwsgi_temp_path ${WORK}/uwsgi;
  scgi_temp_path ${WORK}/scgi;
  server {
    listen 127.0.0.1:${ORIGIN_PORT};
    location / { default_type text/css; return 200 'origin'; }
  }
CONF
server_block tls "${TLS_PORT}" "${VOL}" off
server_block plain "${PLAIN_PORT}" "${VOL}" off
server_block tls "${FWD_TLS_PORT}" "${VOL}" on
server_block plain "${FWD_PLAIN_PORT}" "${VOL}" on
echo "}"
} >"${WORK}/nginx.conf"

"${NGINX_BIN}" -t -c "${WORK}/nginx.conf" -p "${WORK}" || { echo "nginx -t failed"; exit 2; }
"${NGINX_BIN}" -c "${WORK}/nginx.conf" -p "${WORK}" &
NGINX_PID=$!
for _ in $(seq 1 50); do
  "${CURL}" -sk -o /dev/null "http://127.0.0.1:${PLAIN_PORT}/" 2>/dev/null && break
  sleep 0.2
done

# status <proto-flag> <tls|plain> <port> <path> [extra curl args]:
# prints the X-PageSpeed response header value (HIT or MISS).
status() {
  local proto="$1" kind="$2" port="$3" path="$4"; shift 4
  local base="http://127.0.0.1:${port}"
  [ "${kind}" = tls ] && base="https://127.0.0.1:${port}"
  "${CURL}" -sk -D - -o /dev/null "${proto}" -H "Host: ${HOST_HDR}" "$@" \
    "${base}${path}" | tr -d '\r' | awk -F': ' 'tolower($1)=="x-pagespeed"{print $2}'
}

expect() {  # name expected proto kind port path [extra]
  local name="$1" want="$2"; shift 2
  local got
  got="$(status "$@")"
  if [ "${got}" = "${want}" ]; then
    pass "${name}: ${want}"
  else
    fail "${name}: expected ${want}, got '${got}'"
  fi
}

# Same entry for HTTP/1.1 and HTTP/2 over TLS (keyed https).
expect "HTTP/1.1 over TLS finds the https entry"  HIT  --http1.1 tls   "${TLS_PORT}"   /https-only-a.css
expect "HTTP/2 over TLS finds the https entry"    HIT  --http2   tls   "${TLS_PORT}"   /https-only-b.css
# Plain HTTP uses the http scheme: it does not see https entries, and it finds
# its own.
expect "plain HTTP does not find the https entry" MISS --http1.1 plain "${PLAIN_PORT}" /https-only-c.css
expect "plain HTTP finds the http entry"          HIT  --http1.1 plain "${PLAIN_PORT}" /http-only-a.css
# TLS does not see http entries, over either protocol.
expect "HTTP/1.1 over TLS does not find the http entry" MISS --http1.1 tls "${TLS_PORT}" /http-only-a.css
expect "HTTP/2 over TLS does not find the http entry"   MISS --http2   tls "${TLS_PORT}" /http-only-b.css
# An X-Forwarded-Proto header is ignored when the directive is off.
expect "forwarded header ignored (plain, directive off)" MISS --http1.1 plain "${PLAIN_PORT}" /https-only-d.css \
  -H "X-Forwarded-Proto: https"
expect "forwarded header ignored (TLS, directive off)"   HIT  --http1.1 tls   "${TLS_PORT}"   /https-only-e.css \
  -H "X-Forwarded-Proto: http"
# The request line does not choose the scheme; the connection does.
expect "absolute-form https on plain HTTP stays http" MISS --http1.1 plain "${PLAIN_PORT}" /https-only-f.css \
  --request-target "https://${HOST_HDR}/https-only-f.css"
expect "absolute-form http over TLS stays https"      MISS --http1.1 tls   "${TLS_PORT}"   /http-only-c.css \
  --request-target "http://${HOST_HDR}/http-only-c.css"
# With the directive on, a valid header decides; without it the connection does.
expect "trusted forwarded https over plain HTTP"  HIT  --http1.1 plain "${FWD_PLAIN_PORT}" /fwd-https-a.css \
  -H "X-Forwarded-Proto: https"
expect "trusted forwarded http over TLS"          HIT  --http1.1 tls   "${FWD_TLS_PORT}"   /fwd-http-a.css \
  -H "X-Forwarded-Proto: http"
expect "no forwarded header: TLS keyed https"     HIT  --http1.1 tls   "${FWD_TLS_PORT}"   /fwd-https-b.css
expect "no forwarded header: plain keyed http"    HIT  --http1.1 plain "${FWD_PLAIN_PORT}" /fwd-http-b.css

if grep -q '\[error\]\|\[crit\]\|\[alert\]\|\[emerg\]' "${WORK}/logs/error.log"; then
  fail "error log has error/crit/alert/emerg entries"
  grep '\[error\]\|\[crit\]\|\[alert\]\|\[emerg\]' "${WORK}/logs/error.log"
else
  pass "no error/crit/alert/emerg in the error log"
fi

if [ "${FAILED}" -ne 0 ]; then
  echo "--- nginx error log (last 30 lines) ---"
  tail -30 "${WORK}/logs/error.log"
  echo "RESULT: FAILED"
  exit 1
fi
echo "RESULT: PASSED"
