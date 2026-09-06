#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Regression smoke for the 2014 stale-event race, per HTTP protocol version.
#
# History: upstream ngx_pagespeed #788 (SPDY + IPRO -> protocol errors, broken
# images, hangs) was diagnosed in #792 as the pipe read handler processing
# stale events for already-finalized requests, and fixed in 43d1706e2: the old
# handler drained the whole pipe and dispatched ONCE, coalescing IPRO's
# HeadersComplete+Done events. The event-connection code has carried a
# one-event-per-read caution ever since; the batch-drain change keeps one
# dispatch per event (in pipe order), and THIS smoke reconstructs the original
# crash topology to prove the bug class stays dead:
#
#   client --h1/h2--> nginx(pagespeed + IPRO) --proxy_pass--> local origin
#
# with unique-URL IPRO storms and client-abort storms (finalize mid-flight).
# Historic failure signatures checked: non-200s on completed fetches,
# "http request count is zero" alerts, crash markers, worker deaths.
#
# Usage:
#   ./test/system/nginx/protocol_proxy_ipro_race_smoke.sh
#
# Requires: a built ngx_pagespeed_module.so in bazel-bin (or MODULE=... env)
# and curl with HTTP/2 support. The docroot and TLS certs are bootstrapped
# in place when missing (a docroot copy plus a self-signed cert -- no pytest
# harness needed, so this also runs on bare runners).
#
# HTTP/3: add "--http3-only" to PROTOCOLS and point NGINX_BINARY at an nginx
# built --with-http_v3_module and CURL at an HTTP/3-capable curl. The QUIC
# listener is added automatically when PROTOCOLS mentions --http3.
set -u

WORKSPACE="${WORKSPACE:-/workspace}"
MODULE="${MODULE:-$WORKSPACE/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so}"
NGINX_BIN="${NGINX_BINARY:-/usr/local/src/nginx/objs/nginx}"
HARNESS_DIR="${NGINX_CONFIG_DIR:-/tmp/nginx_pagespeed_test}"
DOC_ROOT="$HARNESS_DIR/www"
CERTS="$HARNESS_DIR/certs"
CONF_DIR=/tmp/protocol-race-smoke
FRONT_PORT="${FRONT_PORT:-8445}"
ORIGIN_PORT="${ORIGIN_PORT:-8090}"
PROTOCOLS="${PROTOCOLS:---http1.1 --http2}"
# Override with an HTTP/3-capable curl for --http3/--http3-only passes.
CURL="${CURL:-curl}"

# HTTP/3 needs a QUIC listener (and an nginx built --with-http_v3_module;
# startup fails loudly on the directive otherwise).
H3_LISTEN=""
case " $PROTOCOLS " in
  *--http3*) H3_LISTEN="listen $FRONT_PORT quic reuseport;" ;;
esac

FAILURES=0
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }

# Bootstrap docroot + certs in place if missing (mirrors what the system
# test harness generates, without needing its python dependencies).
if [ ! -d "$DOC_ROOT/mod_pagespeed_example" ]; then
  echo "bootstrapping docroot from install/mod_pagespeed_example..."
  mkdir -p "$DOC_ROOT"
  cp -r "$WORKSPACE/install/mod_pagespeed_example" "$DOC_ROOT/"
fi
if [ ! -f "$CERTS/server.crt" ] || \
   ! openssl x509 -checkend 3600 -noout -in "$CERTS/server.crt" 2>/dev/null; then
  echo "generating self-signed test certificate..."
  mkdir -p "$CERTS"
  openssl req -x509 -newkey rsa:2048 \
    -keyout "$CERTS/server.key" -out "$CERTS/server.crt" \
    -days 365 -nodes \
    -subj "/C=US/ST=Test/L=Test/O=PageSpeed/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,DNS:127.0.0.1,IP:127.0.0.1" \
    2>/dev/null
fi
pkill -x nginx 2>/dev/null; sleep 1

rm -rf "$CONF_DIR" /tmp/protocol-race-cache
mkdir -p "$CONF_DIR/logs" /tmp/protocol-race-cache

cat > "$CONF_DIR/nginx.conf" <<EOF
worker_processes 1;
pid $CONF_DIR/nginx.pid;
error_log $CONF_DIR/logs/error.log info;
load_module $MODULE;
# QUIC needs the headroom: clients killed mid-handshake (storm 2) send no
# CONNECTION_CLOSE, so their server-side connections linger until timeout —
# unlike TCP aborts (RST frees the slot immediately). At 1024 a fast box
# hits "worker_connections are not enough" in storm 3 and nginx reclaims
# live connections, which shows up as client-side 000s on h3.
events { worker_connections 4096; }
http {
    types {
        text/html html;
        text/css css;
        application/javascript js;
        image/jpeg jpeg jpg;
        image/png png;
        image/gif gif;
    }
    default_type application/octet-stream;
    access_log off;
    sendfile off;
    client_body_temp_path $CONF_DIR/client_body_temp;
    proxy_temp_path $CONF_DIR/proxy_temp;

    pagespeed FileCachePath /tmp/protocol-race-cache;
    pagespeed StatisticsPath /pagespeed_statistics;
    pagespeed MessageBufferSize 100000;

    # Origin: static files, pagespeed off, throttled so responses stream
    # slowly enough to keep multiple events in flight per request.
    server {
        listen 127.0.0.1:$ORIGIN_PORT;
        pagespeed off;
        root $DOC_ROOT;
        location / { limit_rate 400k; }
    }

    # Front: TLS + h2 + pagespeed with IPRO, proxying to the origin.
    # This is the 2014 crash topology (SPDY -> h2). h1 requests over the
    # same listener cover the HTTP/1.1 code path.
    server {
        listen $FRONT_PORT ssl;
        $H3_LISTEN
        http2 on;
        server_name localhost;
        ssl_certificate $CERTS/server.crt;
        ssl_certificate_key $CERTS/server.key;

        pagespeed on;
        pagespeed RewriteLevel CoreFilters;
        pagespeed FetchHttps enable,allow_self_signed;
        pagespeed MapOriginDomain "http://127.0.0.1:$ORIGIN_PORT" "https://localhost:$FRONT_PORT";

        location ~ "\\.pagespeed\\.([a-z]\\.)?[a-z]{2}\\.[^.]{10}\\.[^.]+" { proxy_pass http://127.0.0.1:$ORIGIN_PORT; }
        location ~ "^/pagespeed_static/" { }
        location ~ "^/ngx_pagespeed_beacon\$" { }
        location /pagespeed_statistics { }
        location / { proxy_pass http://127.0.0.1:$ORIGIN_PORT; }
    }
}
EOF

"$NGINX_BIN" -p "$CONF_DIR" -c "$CONF_DIR/nginx.conf" || { echo "FAIL: nginx would not start"; exit 1; }
sleep 1
WORKER_BEFORE=$(pgrep -x nginx | sort | tr '\n' ' ')
URL="https://localhost:$FRONT_PORT/mod_pagespeed_example"

# Sanity: pagespeed must actually be active on the front.
if ! "$CURL" -sk -D- -o /dev/null "$URL/index.html" | grep -qi "x-page-speed"; then
  fail "front vhost is not serving through pagespeed (no X-Page-Speed header)"
fi

for PROTO in $PROTOCOLS; do
  echo "=== protocol: $PROTO"
  SEEN_PROTO=$("$CURL" -sk $PROTO -o /dev/null -w "%{http_version}" "$URL/index.html")
  echo "negotiated http version: $SEEN_PROTO"

  echo "--- storm 1: 900 unique IPRO-eligible fetches, 60-way"
  seq 1 900 | xargs -P 60 -I{} sh -c '
    case $(( {} % 3 )) in
      0) U="'"$URL"'/images/Puzzle.jpg?v='"$SEEN_PROTO"'{}";;
      1) U="'"$URL"'/index.html?v='"$SEEN_PROTO"'{}";;
      2) U="'"$URL"'/images/BikeCrashIcn.png?v='"$SEEN_PROTO"'{}";;
    esac
    '"$CURL"' -sk '"$PROTO"' -o /dev/null --max-time 30 -w "%{http_code}\n" "$U"' > /tmp/protocol-race-codes.txt
  BAD=$(grep -cv "^200$" /tmp/protocol-race-codes.txt || true)
  if [ "$BAD" -ne 0 ]; then
    fail "$PROTO storm 1: $BAD non-200 responses"
    echo "    distribution: $(sort /tmp/protocol-race-codes.txt | uniq -c | sort -rn | tr '\n' ' ')"
  fi

  echo "--- storm 2: 400 aborted requests (finalize mid-flight), 40-way"
  seq 1 400 | xargs -P 40 -I{} sh -c '
    '"$CURL"' -sk '"$PROTO"' -o /dev/null --max-time 0.08 "'"$URL"'/images/Puzzle.jpg?a='"$SEEN_PROTO"'{}" 2>/dev/null; true' || true
  sleep 2

  echo "--- storm 3: 600 cache-hit refetches interleaved with aborts, 60-way"
  seq 1 600 | xargs -P 60 -I{} sh -c '
    if [ $(( {} % 4 )) -eq 0 ]; then
      '"$CURL"' -sk '"$PROTO"' -o /dev/null --max-time 0.05 "'"$URL"'/images/Puzzle.jpg" 2>/dev/null; true
    else
      '"$CURL"' -sk '"$PROTO"' -o /dev/null --max-time 30 -w "%{http_code}\n" "'"$URL"'/images/Puzzle.jpg"
    fi' > /tmp/protocol-race-codes3.txt
  BAD3=$(grep -cv "^200$" /tmp/protocol-race-codes3.txt || true)
  if [ "$BAD3" -ne 0 ]; then
    fail "$PROTO storm 3: $BAD3 non-200 responses"
    echo "    distribution: $(sort /tmp/protocol-race-codes3.txt | uniq -c | sort -rn | tr '\n' ' ')"
  fi
done

WORKER_AFTER=$(pgrep -x nginx | sort | tr '\n' ' ')
[ "$WORKER_BEFORE" = "$WORKER_AFTER" ] || fail "worker set changed: '$WORKER_BEFORE' -> '$WORKER_AFTER'"

RCOUNT=$(grep -cE "http request count is zero" "$CONF_DIR/logs/error.log" || true)
[ "$RCOUNT" -eq 0 ] || fail "$RCOUNT 'request count is zero' alerts (2014 signature)"
CRASH=$(grep -cE "exited on signal|signal [0-9]+ .SIG(SEGV|ABRT|BUS|ILL|FPE)|Check failed|CRITICAL" "$CONF_DIR/logs/error.log" || true)
[ "$CRASH" -eq 0 ] || fail "$CRASH crash markers in error.log"

FINAL=$("$CURL" -sk --http2 -o /dev/null -w "%{http_code}" "$URL/index.html")
[ "$FINAL" = "200" ] || fail "final sanity fetch returned $FINAL"

pkill -x nginx 2>/dev/null
if [ "$FAILURES" -eq 0 ]; then
  echo "PASS: protocol proxy IPRO race smoke ($PROTOCOLS)"
  exit 0
fi
echo "$FAILURES failure(s); error log preserved at $CONF_DIR/logs/error.log"
exit 1
