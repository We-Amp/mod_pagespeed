#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# AgentPass A2 live smoke for the nginx Web-Bot-Auth NETWORK
# key-directory warm-fetch.
#
# Proves, against a RUNNING nginx loading ngx_pagespeed_module.so, that:
#   1. WARM PATH: with NO local WebBotAuthKeyDirectoryFile, the verifier resolves
#      a signer key purely from the background warm-fetch of a remote HTTPS JWKS
#      directory -> a valid Ed25519-signed request flips from "unknown" to
#      "<bot>, ed25519-verified" once the per-worker warmer thread has fetched.
#      (The request path itself reads CACHE-ONLY and never fetches; the flip is
#      driven entirely by the off-event-loop warmer populating the Cyclone cache.)
#   2. FAIL CLOSED: when the directory URL is NOT on the SSRF allowlist, the
#      warmer short-circuits, the cache stays empty, and the same valid-signature
#      request stays "unknown" (fail-closed; never a fetch on the request path).
#
# The remote directory is a self-signed HTTPS stub on 127.0.0.1; the warmer's
# curl fetch trusts it via `FetchHttps enable,allow_self_signed` (VERIFYPEER=0,
# but VERIFYHOST=2 still applies -> the stub cert carries an IP:127.0.0.1 SAN).
# Signatures + JWKS come from //pagespeed/kernel/webbotauth:webbotauth_sign_tool
# (same serializer the verifier checks; deterministic keypair).
#
# REPRODUCE (inside the pagespeed1.1-dev container, repo mounted at /workspace):
#   bazel build --config=ci \
#     //pagespeed/nginx:ngx_pagespeed_module.so \
#     //pagespeed/kernel/webbotauth:webbotauth_sign_tool
#   bash test/system/nginx/webbotauth_warmfetch_smoke.sh
#
# Override artifact locations / ports with TOOL=, MOD=, NGINX=, PORT=, STUBPORT=.
set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
TOOL="${TOOL:-$WORKSPACE/bazel-bin/pagespeed/kernel/webbotauth/webbotauth_sign_tool}"
MOD="${MOD:-$WORKSPACE/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so}"
NGINX="${NGINX:-/usr/local/src/nginx/objs/nginx}"
PORT="${PORT:-8998}"
STUBPORT="${STUBPORT:-9443}"
KID="${KID:-test-key-1}"
BOT="${BOT:-testbot}"
BASE="${BASE:-/tmp/wba_warmfetch_smoke}"
WARM_TIMEOUT_S="${WARM_TIMEOUT_S:-45}"  # warmer initial delay is (1+pid%30)s

DIR_HOST="127.0.0.1"
STUB_ORIGIN="https://127.0.0.1:$STUBPORT"
STUB_URL="$STUB_ORIGIN/jwks.json"

for f in "$TOOL" "$MOD" "$NGINX"; do
  [ -x "$f" ] || [ -f "$f" ] || { echo "MISSING: $f (build it first)"; exit 2; }
done
command -v python3 >/dev/null || { echo "MISSING: python3"; exit 2; }
command -v openssl >/dev/null || { echo "MISSING: openssl"; exit 2; }

rm -rf "$BASE"
mkdir -p "$BASE"/www
echo "ok" > "$BASE/www/wba-probe"

# Signer public key (JWKS the stub serves) + a fresh valid signed header set.
"$TOOL" --emit=jwks --kid="$KID" > "$BASE/jwks.json"
"$TOOL" --emit=headers --kid="$KID" --method=GET --authority=localhost --path=/wba-probe > "$BASE/valid.txt"
V_SIGINPUT=$(sed -n 's/^SIGINPUT=//p' "$BASE/valid.txt")
V_SIG=$(sed -n 's/^SIG=//p' "$BASE/valid.txt")

# Self-signed TLS cert for the stub with an IP SAN (curl VERIFYHOST=2 checks it).
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -keyout "$BASE/stub.key" -out "$BASE/stub.crt" \
  -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" >/dev/null 2>&1

# Minimal HTTPS JWKS stub.
cat > "$BASE/stub.py" <<'PY'
import http.server, ssl, sys
jwks, cert, key, port = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/jwks.json":
            with open(jwks, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404); self.end_headers()
    def log_message(self, *a):
        pass
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
httpd = http.server.HTTPServer(("127.0.0.1", port), H)
httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
httpd.serve_forever()
PY

STUB_PID=""
NGINX_RUNNING=""
cleanup() {
  [ -n "$NGINX_RUNNING" ] && "$NGINX" -c "$BASE/nginx.conf" -p "$BASE" -s stop 2>/dev/null || true
  [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null || true
}
trap cleanup EXIT

start_stub() {
  python3 "$BASE/stub.py" "$BASE/jwks.json" "$BASE/stub.crt" "$BASE/stub.key" "$STUBPORT" &
  STUB_PID=$!
  # Wait until the stub answers over TLS.
  for _ in $(seq 1 20); do
    if curl -fsS --cacert "$BASE/stub.crt" "$STUB_URL" >/dev/null 2>&1; then return 0; fi
    sleep 0.25
  done
  echo "stub did not come up"; return 1
}

# Render an nginx config. $1 = allowlist origin (use a wrong origin to force
# the fail-closed/off-allowlist path). No local key file -> warm-fetch is the
# ONLY key source.
render_conf() {
  local allowlist="$1"
  rm -rf "$BASE"/{cache,logs,client_body_temp,proxy_temp,fastcgi_temp,uwsgi_temp,scgi_temp}
  mkdir -p "$BASE"/{cache,logs,client_body_temp,proxy_temp,fastcgi_temp,uwsgi_temp,scgi_temp}
  cat > "$BASE/nginx.conf" <<CONF
load_module $MOD;
worker_processes 1;
pid $BASE/nginx.pid;
error_log $BASE/logs/error.log info;
events { worker_connections 64; }
http {
  access_log $BASE/logs/access.log;
  client_body_temp_path $BASE/client_body_temp;
  proxy_temp_path $BASE/proxy_temp;
  fastcgi_temp_path $BASE/fastcgi_temp;
  uwsgi_temp_path $BASE/uwsgi_temp;
  scgi_temp_path $BASE/scgi_temp;
  default_type text/plain;
  sendfile off;

  pagespeed on;
  pagespeed FileCachePath $BASE/cache;
  pagespeed FetchHttps "enable,allow_self_signed";

  pagespeed WebBotAuth on;
  pagespeed WebBotAuthTelemetry on;
  pagespeed WebBotAuthVerifiedBots "$KID=$BOT";
  # A2 warm-fetch: remote HTTPS JWKS, NO local file. Refresh floor is 60s; the
  # initial fetch fires (1 + pid%30)s after worker start.
  pagespeed WebBotAuthDirectoryHost $DIR_HOST;
  pagespeed WebBotAuthKeyDirectoryUrl $STUB_URL;
  pagespeed WebBotAuthKeyDirectoryAllowlist $allowlist;
  pagespeed WebBotAuthKeyDirectoryRefreshSec 60;

  server {
    listen 127.0.0.1:$PORT;
    server_name localhost;
    root $BASE/www;
    add_header X-Verified-Bot \$x_verified_bot always;
    location / { try_files \$uri =404; }
  }
}
CONF
}

start_nginx() {
  "$NGINX" -t -c "$BASE/nginx.conf" -p "$BASE"
  "$NGINX" -c "$BASE/nginx.conf" -p "$BASE"
  NGINX_RUNNING=1
  sleep 1
}
stop_nginx() {
  "$NGINX" -c "$BASE/nginx.conf" -p "$BASE" -s stop 2>/dev/null || true
  NGINX_RUNNING=""
  sleep 1
}

verdict() {  # valid-signature probe -> the X-Verified-Bot header value
  curl -fsS -D - -o /dev/null \
    -H "Host: localhost" -H "User-Agent: TestBot/1.0" \
    -H "Signature-Input: $V_SIGINPUT" -H "Signature: $V_SIG" \
    "http://127.0.0.1:$PORT/wba-probe" \
    | awk -F': ' 'tolower($1)=="x-verified-bot"{print $2}' | tr -d '\r'
}

fail=0

echo "=== Scenario 1: WARM PATH (stub up, allowlist correct) ==="
start_stub
render_conf "$STUB_ORIGIN"
start_nginx
got=""
for _ in $(seq 1 "$WARM_TIMEOUT_S"); do
  got="$(verdict || true)"
  [ "$got" = "$BOT, ed25519-verified" ] && break
  sleep 1
done
if [ "$got" = "$BOT, ed25519-verified" ]; then
  echo "PASS  warm-fetch resolved -> '$got'"
else
  echo "FAIL  warm-fetch never resolved (last='$got', want '$BOT, ed25519-verified')"
  echo "----- error.log tail -----"; tail -n 30 "$BASE/logs/error.log" 2>/dev/null || true
  fail=1
fi
stop_nginx

echo "=== Scenario 2: FAIL CLOSED (directory URL off the SSRF allowlist) ==="
# allowlist points at a DIFFERENT origin -> warmer short-circuits, cache empty.
render_conf "https://example.invalid"
start_nginx
bad=0
for _ in $(seq 1 8); do
  got="$(verdict || true)"
  if [ "$got" = "$BOT, ed25519-verified" ]; then bad=1; break; fi
  sleep 1
done
if [ "$bad" -eq 0 ]; then
  echo "PASS  off-allowlist stayed fail-closed -> '$got'"
else
  echo "FAIL  off-allowlist resolved a key (should never warm-fetch)"
  fail=1
fi
stop_nginx

[ "$fail" -eq 0 ] && echo "WEBBOTAUTH WARM-FETCH SMOKE: PASS" || { echo "WEBBOTAUTH WARM-FETCH SMOKE: FAIL"; exit 1; }
