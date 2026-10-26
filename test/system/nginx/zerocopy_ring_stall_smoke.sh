#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Smoke for slow-client protection on the zero-copy HTTP/2 serve path
# (issue #307).
#
# With CycloneZeroCopy on, a large cached response over HTTP/2 is served
# through a bounded ring of small windows that a periodic timer refills.
# The concern is that this timer keeps pushing back nginx's send_timeout, so
# a client that stops granting flow-control window is never timed out.
#
# The zero-copy ring is only used for cache hits on rewritten (.pagespeed.)
# resources, so the script enables extend_cache, serves a page that
# references a large script, reads the rewritten script URL out of that page
# and uses that URL for every phase below.
#
# This script runs an h2 client (h2_stall_client.py) against its own nginx:
#
#   precondition  a fast curl --http2 fetch returns the full body and moves
#                 zerocopy_serve_ring_refills, so the ring path is engaged.
#   stall         a stream whose window is never reopened must be ended by
#                 the server (RST_STREAM, GOAWAY or close) within
#                 send_timeout plus a margin, measured from the last DATA
#                 frame the client received.
#   control       a client that reopens the window more often than
#                 send_timeout must receive the whole body and must not be
#                 reset.
#
# Reading the result of a manual run:
#   PASS            current master already times the stalled stream out:
#                   the concern in #307 does not reproduce and the issue can
#                   be closed with this output as the evidence.
#   FAIL (stall)    the stalled stream was still open long after
#                   send_timeout: the concern is real and the fix described
#                   in #307 is needed.
#   FAIL (control)  the progressing client was cut off or did not finish:
#                   the timeout is too eager (or the setup is wrong); look
#                   at the error log before drawing conclusions about stall.
#
# Standalone smoke: CI does not run it. Run it by hand on a host that has
# a built module.
#
# Usage:
#   ./test/system/nginx/zerocopy_ring_stall_smoke.sh
#
# Requires: a built ngx_pagespeed_module.so in bazel-bin (or MODULE=... env),
# nginx built with SSL and HTTP/2, openssl, curl with HTTP/2, python3.
# Runs its own nginx and only stops the nginx it started.
set -u

WORKSPACE="${WORKSPACE:-/workspace}"
MODULE="${MODULE:-$WORKSPACE/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so}"
NGINX_BIN="${NGINX_BINARY:-/usr/local/src/nginx/objs/nginx}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT="$SCRIPT_DIR/h2_stall_client.py"
D="$(mktemp -d "${TMPDIR:-/tmp}/zerocopy-ring-stall-smoke.XXXXXX")" \
  || { echo "FAIL: could not create a temp directory"; exit 1; }
PORT="${PORT:-8459}"
SEND_TIMEOUT="${SEND_TIMEOUT:-2}"     # seconds
BODY_KB="${BODY_KB:-384}"             # must exceed 128 KB (ring threshold)
SOURCE=/stall/big.js                  # rewritten URL is read from the page

FAILURES=0
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }

# shellcheck disable=SC2329  # invoked through the EXIT trap
stop_nginx() {
  if [ -s "$D/nginx.pid" ]; then
    kill "$(cat "$D/nginx.pid")" 2>/dev/null
  fi
}

mkdir -p "$D/logs" "$D/certs" "$D/cache" "$D/www/stall"

openssl req -x509 -newkey rsa:2048 -keyout "$D/certs/server.key" \
  -out "$D/certs/server.crt" -days 2 -nodes -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" 2>/dev/null \
  || { echo "FAIL: could not generate a certificate"; exit 1; }

# Incompressible-looking text so size on the wire equals size on disk.
head -c $((BODY_KB * 1024 * 3 / 4)) /dev/urandom | base64 -w0 2>/dev/null \
  > "$D/www$SOURCE" \
  || head -c $((BODY_KB * 1024 * 3 / 4)) /dev/urandom | base64 \
     | tr -d '\n' > "$D/www$SOURCE"
cat > "$D/www/index.html" <<HTML
<html><head><script src="$SOURCE"></script></head><body>zero-copy stall smoke</body></html>
HTML
BODY_BYTES=$(wc -c < "$D/www$SOURCE" | tr -d ' ')

cat > "$D/nginx.conf" <<CONF
worker_processes 1;
pid $D/nginx.pid;
error_log $D/logs/error.log info;
load_module $MODULE;
events { worker_connections 256; }
http {
    types { text/html html; application/javascript js; }
    default_type application/octet-stream;
    access_log off;
    sendfile off;
    send_timeout ${SEND_TIMEOUT}s;
    client_body_temp_path $D/client_body_temp;
    proxy_temp_path $D/proxy_temp;

    pagespeed FileCachePath $D/cache;
    pagespeed StatisticsPath /pagespeed_statistics;
    pagespeed CycloneZeroCopy on;
    pagespeed CycloneZeroCopyServe on;

    server {
        listen $PORT ssl;
        http2 on;
        server_name localhost;
        ssl_certificate $D/certs/server.crt;
        ssl_certificate_key $D/certs/server.key;
        root $D/www;

        pagespeed on;
        pagespeed RewriteLevel PassThrough;
        pagespeed EnableFilters extend_cache;

        location /pagespeed_statistics { }
        location / { add_header Cache-Control "public, max-age=3600"; }
    }
}
CONF

"$NGINX_BIN" -p "$D" -c "$D/nginx.conf" \
  || { echo "FAIL: nginx would not start"; exit 1; }
trap stop_nginx EXIT
sleep 1

BASE="https://localhost:$PORT"
ring_refills() {
  curl -sk --http2 "$BASE/pagespeed_statistics" \
    | awk '/^zerocopy_serve_ring_refills:/ {print $2}'
}

# Fetch the page over HTTP/1.1 until the rewritten script URL appears; the
# first fetches only start the rewrite.
RESOURCE=""
for _ in $(seq 1 30); do
  FOUND=$(curl -sk --http1.1 --max-time 20 "$BASE/index.html" \
          | grep -oE '/?stall/big\.js\.pagespeed\.ce\.[A-Za-z0-9_-]+\.js' \
          | head -n 1)
  if [ -n "$FOUND" ]; then RESOURCE="/${FOUND#/}"; break; fi
  sleep 1
done
if [ -z "$RESOURCE" ]; then
  echo "FAIL: the page never referenced a rewritten script URL (check extend_cache and the module build)"
  echo "error log: $D/logs/error.log"
  exit 1
fi
echo "rewritten resource: $RESOURCE"
# Warm the rewritten URL into the cache.
for _ in 1 2; do
  curl -sk --http1.1 --max-time 20 -o /dev/null "$BASE$RESOURCE"
done

# Precondition: a fast HTTP/2 fetch of the cached rewritten URL must return
# the whole body through the ring.
BEFORE=0
GOT=0
for _ in $(seq 1 10); do
  BEFORE=$(ring_refills); BEFORE=${BEFORE:-0}
  GOT=$(curl -sk --http2 --max-time 20 -o /dev/null -w '%{size_download}' \
        "$BASE$RESOURCE")
  AFTER=$(ring_refills); AFTER=${AFTER:-0}
  [ "$AFTER" -gt "$BEFORE" ] && break
  sleep 1
done
echo "precondition: fast fetch got $GOT of $BODY_BYTES bytes," \
     "ring refills $BEFORE -> ${AFTER:-0}"
if [ "$GOT" != "$BODY_BYTES" ]; then
  fail "precondition: fast HTTP/2 fetch did not return the full body"
fi
if [ "${AFTER:-0}" -le "$BEFORE" ]; then
  fail "precondition: zerocopy_serve_ring_refills did not move; the ring path is not engaged (check the module build and CycloneZeroCopy)"
fi
if [ "$FAILURES" -ne 0 ]; then
  echo "precondition failed: the stall result below would prove nothing"
  echo "error log: $D/logs/error.log"
  exit 1
fi

result_field() { # field, line
  echo "$2" | tr ' ' '\n' | awk -F= -v k="$1" '$1 == k {print $2}'
}
ge() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a >= b) }'; }
le() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a <= b) }'; }

# Stall: window never reopened. The server has 4x send_timeout to end it.
MAX_STALL=$((SEND_TIMEOUT * 4))
LINE=$(python3 "$CLIENT" --port "$PORT" --path "$RESOURCE" --mode stall \
       --max-wait "$MAX_STALL" | tail -n 1)
echo "stall:   $LINE"
OUTCOME=$(result_field outcome "$LINE")
SINCE=$(result_field since_last_data "$LINE")
SBYTES=$(result_field bytes "$LINE")
if [ "${SBYTES:-0}" -eq 0 ]; then
  fail "stall: the client received no DATA at all, so the stall window cannot be judged: $LINE"
fi
case "$OUTCOME" in
  reset|goaway|eof)
    LOW=$(awk -v t="$SEND_TIMEOUT" 'BEGIN { print t - 0.5 }')
    HIGH=$(awk -v t="$SEND_TIMEOUT" 'BEGIN { print t + 3 }')
    if ! ge "$SINCE" "$LOW" || ! le "$SINCE" "$HIGH"; then
      fail "stall: stream ended $SINCE s after the last data, expected between $LOW and $HIGH s"
    fi
    ;;
  alive)
    fail "stall: the stalled stream was still open ${MAX_STALL}s after it stopped (send_timeout ${SEND_TIMEOUT}s): the stall is never timed out (#307)"
    ;;
  complete)
    fail "stall: the response completed although the window was never reopened; the test setup is wrong"
    ;;
  *)
    fail "stall: could not parse the client result: $LINE"
    ;;
esac
if [ -n "$SBYTES" ] && [ "$SBYTES" -ge "$BODY_BYTES" ]; then
  fail "stall: client received the whole body despite the 1 KB window"
fi

# Control: progressing slowly (a grant every 0.5 s, well under send_timeout).
# Must finish with the whole body and must not be reset.
LINE=$(python3 "$CLIENT" --port "$PORT" --path "$RESOURCE" --mode slow \
       --grant-bytes 32768 --grant-interval 0.5 \
       --max-wait $((SEND_TIMEOUT * 15)) | tail -n 1)
echo "control: $LINE"
OUTCOME=$(result_field outcome "$LINE")
CBYTES=$(result_field bytes "$LINE")
if [ "$OUTCOME" != "complete" ] || [ "$CBYTES" != "$BODY_BYTES" ]; then
  fail "control: a slowly progressing client did not receive the full body (outcome=$OUTCOME bytes=$CBYTES, want complete $BODY_BYTES)"
fi

CRASH=$(grep -cE "exited on signal|signal [0-9]+ .SIG(SEGV|ABRT|BUS|ILL|FPE)|Check failed|CRITICAL" "$D/logs/error.log" || true)
[ "$CRASH" -eq 0 ] || fail "$CRASH crash markers in error.log"

if [ "$FAILURES" -eq 0 ]; then
  echo "PASS: a stalled zero-copy HTTP/2 stream is ended within send_timeout and a progressing stream is not (#307 does not reproduce)"
  exit 0
fi
echo "$FAILURES failure(s); error log preserved at $D/logs/error.log"
exit 1
