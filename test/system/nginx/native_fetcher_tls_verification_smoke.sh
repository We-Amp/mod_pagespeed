#!/bin/bash
# Fail-closed smoke for native-fetcher TLS certificate verification
# (UseNativeFetcher + FetchHttps). Three vhosts on one worker:
#
#   A: FetchHttps enable (strict) against a self-signed cert
#      -> rewriter fetches must FAIL closed, reason logged.
#   B: FetchHttps enable,allow_self_signed, cert with matching SAN
#      -> must rewrite (positive control; combined CSS served).
#   C: allow_self_signed, cert WITHOUT a SAN for the host
#      -> hostname check must FAIL even though chain verification is
#         waived (CURLOPT_SSL_VERIFYHOST=2 parity).
#
# B exercises the IP-SAN path: pagespeed loopback-routes subresource
# fetches through its own vhost by IP, so the certificate is matched via
# X509_check_ip_asc, not DNS-name checking.
#
# Usage:
#   ./test/system/nginx/native_fetcher_tls_verification_smoke.sh
#
# Requires: a built ngx_pagespeed_module.so in bazel-bin (or MODULE=... env),
# nginx built with SSL, openssl, curl. Docroot is bootstrapped in place when
# missing. Runs its own nginx; kills stray nginx processes.
set -u

WORKSPACE="${WORKSPACE:-/workspace}"
MODULE="${MODULE:-$WORKSPACE/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so}"
NGINX_BIN="${NGINX_BINARY:-/usr/local/src/nginx/objs/nginx}"
HARNESS_DIR="${NGINX_CONFIG_DIR:-/tmp/nginx_pagespeed_test}"
DOC_ROOT="$HARNESS_DIR/www"
D=/tmp/native-tls-verification-smoke
BASE_PORT="${BASE_PORT:-8446}"

FAILURES=0
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }

if [ ! -d "$DOC_ROOT/mod_pagespeed_example" ]; then
  echo "bootstrapping docroot from install/mod_pagespeed_example..."
  mkdir -p "$DOC_ROOT"
  cp -r "$WORKSPACE/install/mod_pagespeed_example" "$DOC_ROOT/"
fi

pkill -x nginx 2>/dev/null; sleep 1
rm -rf "$D"; mkdir -p "$D/logs" "$D/certs" "$D/cache-a" "$D/cache-b" "$D/cache-c"

# Cert 1: SANs for localhost + 127.0.0.1. Cert 2: unrelated host only.
openssl req -x509 -newkey rsa:2048 -keyout "$D/certs/lh.key" \
  -out "$D/certs/lh.crt" -days 2 -nodes -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" 2>/dev/null
openssl req -x509 -newkey rsa:2048 -keyout "$D/certs/other.key" \
  -out "$D/certs/other.crt" -days 2 -nodes -subj "/CN=otherhost.invalid" \
  -addext "subjectAltName=DNS:otherhost.invalid" 2>/dev/null

RESOLVER=$(awk '/^nameserver/{print $2; exit}' /etc/resolv.conf)
PORT_A=$BASE_PORT; PORT_B=$((BASE_PORT + 1)); PORT_C=$((BASE_PORT + 2))

cat > "$D/nginx.conf" <<EOF
worker_processes 1;
pid $D/nginx.pid;
error_log $D/logs/error.log info;
load_module $MODULE;
events { worker_connections 512; }
http {
    types { text/html html; text/css css; }
    default_type application/octet-stream;
    access_log off;
    client_body_temp_path $D/client_body_temp;
    proxy_temp_path $D/proxy_temp;
    resolver ${RESOLVER:-127.0.0.1} ipv6=off;
    pagespeed UseNativeFetcher on;
    pagespeed StatisticsPath /pagespeed_statistics;

    server {
        listen $PORT_A ssl;
        server_name localhost;
        ssl_certificate $D/certs/lh.crt;
        ssl_certificate_key $D/certs/lh.key;
        root $DOC_ROOT;
        pagespeed on;
        pagespeed FileCachePath $D/cache-a;
        pagespeed RewriteLevel CoreFilters;
        pagespeed FetchHttps enable;
    }

    server {
        listen $PORT_B ssl;
        server_name localhost;
        ssl_certificate $D/certs/lh.crt;
        ssl_certificate_key $D/certs/lh.key;
        root $DOC_ROOT;
        pagespeed on;
        pagespeed FileCachePath $D/cache-b;
        pagespeed RewriteLevel CoreFilters;
        pagespeed FetchHttps enable,allow_self_signed;
    }

    server {
        listen $PORT_C ssl;
        server_name localhost;
        ssl_certificate $D/certs/other.crt;
        ssl_certificate_key $D/certs/other.key;
        root $DOC_ROOT;
        pagespeed on;
        pagespeed FileCachePath $D/cache-c;
        pagespeed RewriteLevel CoreFilters;
        pagespeed FetchHttps enable,allow_self_signed;
    }
}
EOF

"$NGINX_BIN" -p "$D" -c "$D/nginx.conf" || { echo "FAIL: nginx would not start"; exit 1; }
sleep 1

combined() { # port -> combined-css marker count after up to 20 attempts
  local port=$1 n=0 i
  for i in $(seq 1 20); do
    n=$(curl -sk "https://localhost:$port/mod_pagespeed_example/combine_css.html" \
        | grep -c "css+" || true)
    [ "$n" -gt 0 ] && break
    sleep 1
  done
  echo "$n"
}

A=$(combined "$PORT_A")
B=$(combined "$PORT_B")
C=$(combined "$PORT_C")
echo "A(strict, self-signed)        combined=$A (want 0)"
echo "B(allow_self_signed, match)   combined=$B (want >0)"
echo "C(allow_self_signed, no SAN)  combined=$C (want 0)"

[ "$A" = "0" ] || fail "strict mode rewrote via an unverified TLS peer"
[ "$B" != "0" ] || fail "allow_self_signed with matching SAN did not rewrite"
[ "$C" = "0" ] || fail "hostname-mismatched peer was rewritten anyway"

grep -q "certificate verification failed" "$D/logs/error.log" \
  || fail "no 'certificate verification failed' logged for the strict vhost"
grep -q "certificate does not match host" "$D/logs/error.log" \
  || fail "no 'certificate does not match host' logged for the SAN-less vhost"

CRASH=$(grep -cE "exited on signal|signal [0-9]+ .SIG(SEGV|ABRT|BUS|ILL|FPE)|Check failed|CRITICAL" "$D/logs/error.log" || true)
[ "$CRASH" -eq 0 ] || fail "$CRASH crash markers in error.log"

pkill -x nginx 2>/dev/null
if [ "$FAILURES" -eq 0 ]; then
  echo "PASS: native fetcher TLS verification smoke"
  exit 0
fi
echo "$FAILURES failure(s); error log preserved at $D/logs/error.log"
exit 1
