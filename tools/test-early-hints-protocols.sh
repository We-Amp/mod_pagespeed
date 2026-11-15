#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# System test: HTTP 103 Early Hints reach clients over HTTP/1.1, HTTP/2 and
# HTTP/3, for .html URLs and for clean (extensionless) URLs, and are withheld
# where they do not belong (non-HTML URLs, extensionless URLs whose client
# does not ask for HTML).
#
# Needs an nginx 1.29.0 or newer built with --with-compat, the SSL, v2 and v3
# modules, the module built against that same nginx source, and curl with
# HTTP/2 and HTTP/3 support.
#
# Usage:
#   NGINX_BIN=/path/to/nginx MODULE_SO=/path/to/ngx_pagespeed_module.so \
#   SEED_TOOL=/path/to/seed_test_cache CURL=/path/to/curl \
#     tools/test-early-hints-protocols.sh
#
# CURL must support --http2 and --http3-only.  Set SKIP_H3=1 to skip HTTP/3.
set -uo pipefail

: "${NGINX_BIN:?set NGINX_BIN}"
: "${MODULE_SO:?set MODULE_SO}"
: "${SEED_TOOL:?set SEED_TOOL}"
CURL="${CURL:-curl}"
PORT="${PORT:-18443}"
PLAIN_PORT="${PLAIN_PORT:-18445}"
ORIGIN_PORT="${ORIGIN_PORT:-18444}"

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

# The cache key carries the Host header as sent, port included.
# Stored hint sets for HTML pages (.html and clean URLs) and, as a control,
# for a stylesheet URL (which must never get hints, whatever is stored).
VOL="${WORK}/cache.vol"
"${SEED_TOOL}" --cache "${VOL}" --create_only >/dev/null || exit 2
# A MISS is recorded as it passes through, so every case below gets its own
# URL; reusing one would turn the second request into a cache HIT.
URLS=""
for tag in h1 h2 h3; do
  URLS="${URLS} /page-${tag}.html /about-${tag} /style-${tag}.css /noaccept-${tag}"
done
# Each server block reads only its own entries: seed each for its block.
for u in ${URLS}; do
  "${SEED_TOOL}" --cache "${VOL}" --url "${u}" --host "localhost:${PORT}" \
    --scheme https --content "/hero.css" --content_type other \
    --site_listen "127.0.0.1:${PORT}" --site_names localhost \
    --mask 0x1C >/dev/null || { echo "seeding ${u} failed"; exit 2; }
  "${SEED_TOOL}" --cache "${VOL}" --url "${u}" --host "localhost:${PLAIN_PORT}" \
    --scheme http --content "/hero.css" --content_type other \
    --site_listen "127.0.0.1:${PLAIN_PORT}" --site_names localhost \
    --mask 0x1C >/dev/null || { echo "seeding ${u} failed"; exit 2; }
done

# nginx workers may run as another user: let them reach the work directory.
chmod -R a+rwX "${WORK}"

cat >"${WORK}/nginx.conf" <<CONF
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
    location / { default_type text/html; return 200 '<html></html>'; }
  }
  server {
    # Plain HTTP/1.1 (cache keys use the "http" scheme here).
    listen 127.0.0.1:${PLAIN_PORT};
    server_name localhost;
    pagespeed on;
    pagespeed_cache_path ${VOL};
    location / {
      proxy_pass http://127.0.0.1:${ORIGIN_PORT};
      proxy_set_header Accept-Encoding "";
    }
  }
  server {
    listen 127.0.0.1:${PORT} ssl;
    listen 127.0.0.1:${PORT} quic reuseport;
    http2 on;
    http3 on;
    server_name localhost;
    ssl_certificate ${WORK}/cert.pem;
    ssl_certificate_key ${WORK}/key.pem;
    ssl_protocols TLSv1.3;
    add_header Alt-Svc 'h3=":${PORT}"; ma=60';
    pagespeed on;
    pagespeed_cache_path ${VOL};
    location / {
      proxy_pass http://127.0.0.1:${ORIGIN_PORT};
      proxy_set_header Accept-Encoding "";
    }
  }
}
CONF

"${NGINX_BIN}" -t -c "${WORK}/nginx.conf" -p "${WORK}" || { echo "nginx -t failed"; exit 2; }
"${NGINX_BIN}" -c "${WORK}/nginx.conf" -p "${WORK}" &
NGINX_PID=$!
for _ in $(seq 1 50); do
  "${CURL}" -sk -o /dev/null "http://127.0.0.1:${PLAIN_PORT}/" 2>/dev/null && break
  sleep 0.2
done

# fetch <proto-flag> <path> <accept>: prints the verbose response headers.
fetch() {
  local proto="$1" path="$2" accept="$3" base="https://localhost:${PORT}"
  # HTTP/1.1 runs against the plain listener; h2 and h3 against TLS/QUIC.
  [ "${proto}" = "--http1.1" ] && base="http://localhost:${PLAIN_PORT}"
  "${CURL}" -sk -v -o /dev/null ${proto} -H "Accept: ${accept}" \
    --resolve "localhost:${PORT}:127.0.0.1" \
    --resolve "localhost:${PLAIN_PORT}:127.0.0.1" \
    "${base}${path}" 2>&1
}

expect_hint() {  # name proto-flag version path accept
  local out
  out="$(fetch "$2" "$4" "$5")"
  if echo "${out}" | grep -q "^< HTTP/$3 103" \
     && echo "${out}" | grep -qi "^< link: </hero.css>; rel=preload; as=style" \
     && echo "${out}" | grep -q "^< HTTP/$3 200"; then
    pass "$1: 103 with Link before the 200"
  else
    fail "$1: expected 103 + Link + 200"; echo "${out}" | grep '^< ' | sed 's/^/    /'
  fi
}

expect_no_hint() {  # name proto-flag version path accept
  local out
  out="$(fetch "$2" "$4" "$5")"
  if echo "${out}" | grep -q "^< HTTP/$3 200" \
     && ! echo "${out}" | grep -q "^< HTTP/$3 103"; then
    pass "$1: no 103"
  else
    fail "$1: expected a plain 200"; echo "${out}" | grep '^< ' | sed 's/^/    /'
  fi
}

HTML_ACCEPT="text/html,application/xhtml+xml,*/*;q=0.8"

expect_hint    "HTTP/1.1 .html URL"            "--http1.1"      1.1 /page-h1.html "${HTML_ACCEPT}"
expect_hint    "HTTP/1.1 extensionless URL"    "--http1.1"      1.1 /about-h1     "${HTML_ACCEPT}"
expect_hint    "HTTP/2 .html URL"              "--http2"        2   /page-h2.html "${HTML_ACCEPT}"
expect_hint    "HTTP/2 extensionless URL"      "--http2"        2   /about-h2     "${HTML_ACCEPT}"
expect_no_hint "HTTP/2 extensionless, no HTML accept" "--http2" 2   /noaccept-h2  "*/*"
expect_no_hint "HTTP/2 stylesheet URL"         "--http2"        2   /style-h2.css "${HTML_ACCEPT}"
if [ "${SKIP_H3:-0}" != "1" ]; then
  expect_hint    "HTTP/3 .html URL"            "--http3-only"   3   /page-h3.html "${HTML_ACCEPT}"
  expect_hint    "HTTP/3 extensionless URL"    "--http3-only"   3   /about-h3     "${HTML_ACCEPT}"
  expect_no_hint "HTTP/3 stylesheet URL"       "--http3-only"   3   /style-h3.css "${HTML_ACCEPT}"
fi

if grep -q '\[error\]\|\[crit\]\|\[alert\]\|\[emerg\]' "${WORK}/logs/error.log"; then
  fail "error log has error/crit/alert/emerg entries"; grep '\[error\]\|\[crit\]\|\[alert\]\|\[emerg\]' "${WORK}/logs/error.log"
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
