#!/bin/bash
# AgentPass A1 live Day-0 smoke for the nginx Web-Bot-Auth wiring.
#
# Proves, against a RUNNING nginx loading ngx_pagespeed_module.so, that the
# observe-only RFC 9421 / Web Bot Auth verifier surfaces the right verdict in
# the $x_verified_bot variable (exposed here via `add_header X-Verified-Bot`):
#   1. a request with a VALID Ed25519 signature whose kid is an operator
#      verified bot      -> "testbot, ed25519-verified"
#   2. a request with NO signature + a spoofed bot User-Agent -> "human"
#   3. a request with a TAMPERED signature                    -> "unknown"
#
# Signatures are produced by //pagespeed/kernel/webbotauth:webbotauth_sign_tool,
# which uses the SAME serializer the verifier checks, so the bytes are
# bit-identical. The signer keypair is deterministic, so the committed-style
# JWKS is stable while each signature is minted fresh (within the verifier
# freshness window).
#
# REPRODUCE (inside the pagespeed1.1-dev container, repo mounted at /workspace):
#   bazel build --config=ci \
#     //pagespeed/nginx:ngx_pagespeed_module.so \
#     //pagespeed/kernel/webbotauth:webbotauth_sign_tool
#   bash test/system/nginx/webbotauth_smoke.sh
#
# Override the artifact locations with TOOL=, MOD=, NGINX=, PORT= if needed.
set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
TOOL="${TOOL:-$WORKSPACE/bazel-bin/pagespeed/kernel/webbotauth/webbotauth_sign_tool}"
MOD="${MOD:-$WORKSPACE/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so}"
NGINX="${NGINX:-/usr/local/src/nginx/objs/nginx}"
PORT="${PORT:-8999}"
KID="${KID:-test-key-1}"
BOT="${BOT:-testbot}"
BASE="${BASE:-/tmp/wba_smoke}"

for f in "$TOOL" "$MOD" "$NGINX"; do
  [ -x "$f" ] || [ -f "$f" ] || { echo "MISSING: $f (build it first)"; exit 2; }
done

rm -rf "$BASE"
mkdir -p "$BASE"/{www,logs,cache,client_body_temp,proxy_temp,fastcgi_temp,uwsgi_temp,scgi_temp}
echo "ok" > "$BASE/www/wba-probe"

# Operator key directory (signer public key) + fresh signed/tampered headers.
"$TOOL" --emit=jwks --kid="$KID" > "$BASE/jwks.json"
"$TOOL" --emit=headers --kid="$KID" --method=GET --authority=localhost --path=/wba-probe > "$BASE/valid.txt"
"$TOOL" --emit=headers --kid="$KID" --method=GET --authority=localhost --path=/wba-probe --tamper > "$BASE/tampered.txt"
V_SIGINPUT=$(sed -n 's/^SIGINPUT=//p' "$BASE/valid.txt")
V_SIG=$(sed -n 's/^SIG=//p' "$BASE/valid.txt")
T_SIGINPUT=$(sed -n 's/^SIGINPUT=//p' "$BASE/tampered.txt")
T_SIG=$(sed -n 's/^SIG=//p' "$BASE/tampered.txt")

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
  pagespeed WebBotAuth on;
  pagespeed WebBotAuthTelemetry on;
  pagespeed WebBotAuthKeyDirectoryFile $BASE/jwks.json;
  pagespeed WebBotAuthVerifiedBots "$KID=$BOT";

  server {
    listen 127.0.0.1:$PORT;
    server_name localhost;
    root $BASE/www;
    add_header X-Verified-Bot \$x_verified_bot always;
    location / { try_files \$uri =404; }
  }
}
CONF

"$NGINX" -t -c "$BASE/nginx.conf" -p "$BASE"
"$NGINX" -c "$BASE/nginx.conf" -p "$BASE"
trap '"$NGINX" -c "$BASE/nginx.conf" -p "$BASE" -s stop 2>/dev/null || true' EXIT
sleep 1

verdict() {
  curl -fsS -D - -o /dev/null "$@" "http://127.0.0.1:$PORT/wba-probe" \
    | awk -F': ' 'tolower($1)=="x-verified-bot"{print $2}' | tr -d '\r'
}

fail=0
check() {  # check <label> <expected> <actual>
  if [ "$3" = "$2" ]; then echo "PASS  $1 -> '$3'"; else echo "FAIL  $1 -> got '$3', want '$2'"; fail=1; fi
}

c1=$(verdict -H "Host: localhost" -H "User-Agent: TestBot/1.0" -H "Signature-Input: $V_SIGINPUT" -H "Signature: $V_SIG")
c2=$(verdict -H "Host: localhost" -H "User-Agent: GPTBot")
c3=$(verdict -H "Host: localhost" -H "User-Agent: TestBot/1.0" -H "Signature-Input: $T_SIGINPUT" -H "Signature: $T_SIG")

check "valid-signature " "$BOT, ed25519-verified" "$c1"
check "no-signature    " "human"                  "$c2"
check "tampered-sig    " "unknown"                 "$c3"

[ "$fail" -eq 0 ] && echo "WEBBOTAUTH SMOKE: PASS" || { echo "WEBBOTAUTH SMOKE: FAIL"; exit 1; }
