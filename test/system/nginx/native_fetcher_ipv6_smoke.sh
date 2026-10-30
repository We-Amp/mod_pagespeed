#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Smoke test for IPv6 upstreams in the nginx native fetcher
# (UseNativeFetcher on). A tiny DNS stub answers for three names and an
# "origin" server listens on [::1] only, so a subresource can only be
# fetched if the fetcher connects over IPv6:
#
#   v6only.test  AAAA ::1 only
#   dual.test    A 127.0.0.1 and AAAA ::1 (origin listens on both)
#   literal      http://[::1]:PORT/ (no DNS involved)
#
# Vhosts on one worker (each one a page that references two CSS files from
# the origin; combine_css rewrites them only if both fetches succeed):
#
#   A: resolver ... ipv6=on,  v6only.test  -> must rewrite
#   B: resolver ... ipv6=on,  dual.test    -> must rewrite
#   C: resolver ... ipv6=on,  [::1] literal -> must rewrite
#   D: resolver ... ipv6=off, v6only.test  -> must NOT rewrite, no crash
#
# The resolver directive is per http block, so D uses a second nginx run.
#
# Usage:
#   ./test/system/nginx/native_fetcher_ipv6_smoke.sh
#
# Requires: a built ngx_pagespeed_module.so in bazel-bin (or MODULE=... env),
# nginx, python3, curl, and an IPv6 loopback (::1). Runs its own nginx and
# DNS stub and stops only the processes it started (by pid).
set -u

WORKSPACE="${WORKSPACE:-/workspace}"
MODULE="${MODULE:-$WORKSPACE/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so}"
NGINX_BIN="${NGINX_BINARY:-/usr/local/src/nginx/objs/nginx}"
D="${D:-/tmp/native-ipv6-smoke}"
BASE_PORT="${BASE_PORT:-8456}"
DNS_PORT="${DNS_PORT:-$((BASE_PORT + 20))}"
ORIGIN_PORT=$((BASE_PORT + 10))

FAILURES=0
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }

if ! python3 -c 'import socket; s=socket.socket(socket.AF_INET6); s.bind(("::1",0))' \
    2>/dev/null; then
  echo "SKIP: no IPv6 loopback (::1) available"
  exit 0
fi

# The fetcher parses each URL with the C library resolver before the nginx
# resolver runs, so the two test names must also resolve through /etc/hosts.
# Add them when we may (root, or a container started with --add-host).
if ! getent hosts v6only.test >/dev/null 2>&1; then
  if ! { echo "::1 v6only.test" >> /etc/hosts && \
         echo "127.0.0.1 dual.test" >> /etc/hosts; } 2>/dev/null; then
    echo "SKIP: v6only.test / dual.test do not resolve and /etc/hosts is not"
    echo "      writable; start the container with --add-host v6only.test:::1"
    echo "      --add-host dual.test:127.0.0.1"
    exit 0
  fi
fi

DNS_PID=""
NGINX_PID_FILE="$D/nginx.pid"
stop_all() {
  if [ -f "$NGINX_PID_FILE" ]; then
    kill "$(cat "$NGINX_PID_FILE")" 2>/dev/null
  fi
  if [ -n "$DNS_PID" ]; then kill "$DNS_PID" 2>/dev/null; fi
}
trap stop_all EXIT

rm -rf "$D"; mkdir -p "$D/logs" "$D/www/css" "$D/cache-a" "$D/cache-b" \
  "$D/cache-c" "$D/cache-d" "$D/cache-origin"

# --- DNS stub: UDP, A and AAAA only -------------------------------------
cat > "$D/dns_stub.py" <<'PY'
import socket, struct, sys
port = int(sys.argv[1])
A = {"dual.test": ["127.0.0.1"]}
AAAA = {"v6only.test": ["::1"], "dual.test": ["::1"]}
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", port))
while True:
    data, addr = s.recvfrom(512)
    tid = data[:2]
    i = 12
    labels = []
    while data[i] != 0:
        n = data[i]; labels.append(data[i+1:i+1+n].decode()); i += n + 1
    qname = ".".join(labels)
    qtype = struct.unpack(">H", data[i+1:i+3])[0]
    question = data[12:i+5]
    answers = b""
    count = 0
    if qtype == 1:
        for ip in A.get(qname, []):
            answers += b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + socket.inet_pton(socket.AF_INET, ip); count += 1
    elif qtype == 28:
        for ip in AAAA.get(qname, []):
            answers += b"\xc0\x0c" + struct.pack(">HHIH", 28, 1, 60, 16) + socket.inet_pton(socket.AF_INET6, ip); count += 1
    hdr = tid + struct.pack(">HHHHH", 0x8180, 1, count, 0, 0)
    s.sendto(hdr + question + answers, addr)
PY
python3 "$D/dns_stub.py" "$DNS_PORT" &
DNS_PID=$!

# --- content ------------------------------------------------------------
echo '.a { color: red; }' > "$D/www/css/a.css"
echo '.b { color: blue; }' > "$D/www/css/b.css"
page() { # host:port -> html referencing both css files on that origin
  cat <<HTML
<html><head>
<link rel="stylesheet" type="text/css" href="http://$1/css/a.css">
<link rel="stylesheet" type="text/css" href="http://$1/css/b.css">
</head><body><p>ipv6 smoke</p></body></html>
HTML
}
page "v6only.test:$ORIGIN_PORT" > "$D/www/v6only.html"
page "dual.test:$ORIGIN_PORT" > "$D/www/dual.html"
page "[::1]:$ORIGIN_PORT" > "$D/www/literal.html"

PORT_A=$BASE_PORT; PORT_B=$((BASE_PORT + 1)); PORT_C=$((BASE_PORT + 2))
PORT_D=$((BASE_PORT + 3))

write_conf() { # ipv6 setting -> nginx.conf
cat > "$D/nginx.conf" <<CONF
worker_processes 1;
pid $NGINX_PID_FILE;
error_log $D/logs/error.log info;
load_module $MODULE;
events { worker_connections 512; }
http {
    types { text/html html; text/css css; }
    default_type application/octet-stream;
    access_log off;
    client_body_temp_path $D/client_body_temp;
    proxy_temp_path $D/proxy_temp;
    resolver 127.0.0.1:$DNS_PORT ipv6=$1 valid=1s;
    resolver_timeout 3s;
    pagespeed UseNativeFetcher on;
    pagespeed FileCachePath $D/cache-origin;
    pagespeed StatisticsPath /pagespeed_statistics;

    server {   # the origin: IPv6 loopback only, plus IPv4 for dual.test
        listen [::1]:$ORIGIN_PORT ipv6only=on;
        listen 127.0.0.1:$ORIGIN_PORT;
        server_name _;
        root $D/www;
    }
    server {
        listen 127.0.0.1:$PORT_A;
        root $D/www;
        pagespeed on;
        pagespeed FileCachePath $D/cache-a;
        pagespeed RewriteLevel PassThrough;
        pagespeed EnableFilters combine_css;
        pagespeed Domain v6only.test:$ORIGIN_PORT;
    }
    server {
        listen 127.0.0.1:$PORT_B;
        root $D/www;
        pagespeed on;
        pagespeed FileCachePath $D/cache-b;
        pagespeed RewriteLevel PassThrough;
        pagespeed EnableFilters combine_css;
        pagespeed Domain dual.test:$ORIGIN_PORT;
    }
    server {
        listen 127.0.0.1:$PORT_C;
        root $D/www;
        pagespeed on;
        pagespeed FileCachePath $D/cache-c;
        pagespeed RewriteLevel PassThrough;
        pagespeed EnableFilters combine_css;
        pagespeed Domain [::1]:$ORIGIN_PORT;
    }
}
CONF
}

combined() { # port page -> combined-css marker count after up to 15 tries
  local n=0 i
  for i in $(seq 1 15); do
    n=$(curl -s "http://127.0.0.1:$1/$2" | grep -c "css+" || true)
    [ "$n" -gt 0 ] && break
    sleep 1
  done
  echo "$n"
}

start_nginx() {
  "$NGINX_BIN" -p "$D" -c "$D/nginx.conf" || { echo "FAIL: nginx would not start"; exit 1; }
  sleep 1
}
stop_nginx() {
  kill "$(cat "$NGINX_PID_FILE")" 2>/dev/null
  for i in 1 2 3 4 5; do [ -f "$NGINX_PID_FILE" ] && sleep 1; done
}

# Sanity: the origin really is reachable over IPv6 and the stub answers.
write_conf on
start_nginx
if [ "$(curl -s -g -o /dev/null -w '%{http_code}' "http://[::1]:$ORIGIN_PORT/css/a.css")" != "200" ]; then
  fail "origin not reachable over [::1]"
fi

A=$(combined "$PORT_A" v6only.html)
B=$(combined "$PORT_B" dual.html)
C=$(combined "$PORT_C" literal.html)
echo "A(ipv6=on, AAAA only)     combined=$A (want >0)"
echo "B(ipv6=on, A + AAAA)      combined=$B (want >0)"
echo "C(literal [::1])          combined=$C (want >0)"
[ "$A" != "0" ] || fail "AAAA-only host was not fetched over IPv6"
[ "$B" != "0" ] || fail "dual-stack host was not fetched"
[ "$C" != "0" ] || fail "literal IPv6 host was not fetched"
CRASH=$(grep -cE "exited on signal|signal [0-9]+ .SIG(SEGV|ABRT|BUS|ILL|FPE)|Check failed|CRITICAL" "$D/logs/error.log" || true)
[ "$CRASH" -eq 0 ] || fail "$CRASH crash markers in error.log (ipv6=on)"
stop_nginx

# ipv6=off: the AAAA-only host has no usable address and must fail cleanly.
write_conf off
sed -i.bak -e "s/listen 127.0.0.1:$PORT_A;/listen 127.0.0.1:$PORT_D;/" \
  -e "s|FileCachePath $D/cache-a;|FileCachePath $D/cache-d;|" "$D/nginx.conf"
start_nginx
Dn=$(combined "$PORT_D" v6only.html)
echo "D(ipv6=off, AAAA only)    combined=$Dn (want 0)"
[ "$Dn" = "0" ] || fail "ipv6=off still reached an IPv6-only host"
grep -qE "no suitable address|failed to resolve host" "$D/logs/error.log" \
  || fail "ipv6=off: no resolver failure logged"
CRASH=$(grep -cE "exited on signal|signal [0-9]+ .SIG(SEGV|ABRT|BUS|ILL|FPE)|Check failed|CRITICAL" "$D/logs/error.log" || true)
[ "$CRASH" -eq 0 ] || fail "$CRASH crash markers in error.log (ipv6=off)"
stop_nginx

if [ "$FAILURES" -eq 0 ]; then
  echo "PASS: native fetcher IPv6 smoke"
  exit 0
fi
echo "$FAILURES failure(s); error log preserved at $D/logs/error.log"
exit 1
