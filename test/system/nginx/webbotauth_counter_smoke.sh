#!/bin/bash
# the design record Bar-A opt-in counter endpoint (EXPERIMENTAL, default off) live Day-0
# smoke for the nginx wiring. Mirrors ModPageSpeed 2.0's t/105-webbotauth-counter
# against a RUNNING nginx loading ngx_pagespeed_module.so, proving the
# /.well-known/webbotauth-counter endpoint honors the operator-configured mode
# (WebBotAuthPublicCounter) + the env-carried secret bearer token
# (PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN) and the response-hardening contract:
#
#   * mode off (default)         -> endpoint invisible (404, normal handling)
#   * mode public, no/bad token  -> COARSE doc (200, Cache-Control max-age=300,
#                                   Vary: Authorization, nosniff, no boot_id)
#   * mode public, valid token   -> EXACT doc (200, Cache-Control no-store,
#                                   boot_id present)
#   * mode private, no token     -> 404 (existence hidden)
#   * mode private, valid token  -> EXACT doc (200, no-store, since present)
#   * HEAD                        -> headers only, no body
#   * non-GET/HEAD method         -> not served (404, normal handling)
#
# Unlike 2.0's worker-less t/105, mod_pagespeed 1.15's nginx module IS the
# counter writer, so this smoke ALSO sends a real verified signed request first
# and then asserts the EXACT doc's per-signer breakdown -- including that an
# operator signer name containing `<script>` is JSON-escaped (<...>),
# never emitted literally.
#
# REPRODUCE (inside the pagespeed1.1-dev container, repo mounted at /workspace):
#   bazel build --config=ci \
#     //pagespeed/nginx:ngx_pagespeed_module.so \
#     //pagespeed/kernel/webbotauth:webbotauth_sign_tool
#   bash test/system/nginx/webbotauth_counter_smoke.sh
#
# Override the artifact locations with TOOL=, MOD=, NGINX=, PORT= if needed.
set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
TOOL="${TOOL:-$WORKSPACE/bazel-bin/pagespeed/kernel/webbotauth/webbotauth_sign_tool}"
MOD="${MOD:-$WORKSPACE/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so}"
NGINX="${NGINX:-/usr/local/src/nginx/objs/nginx}"
PORT="${PORT:-8998}"          # off-mode server
PORT_PRIV=$((PORT + 1))       # private-mode server
PORT_PUB=$((PORT + 2))        # public-mode server
KID="${KID:-test-key-1}"
TOKEN="${TOKEN:-secrettoken-counter-smoke}"
BASE="${BASE:-/tmp/wba_counter_smoke}"
# An operator signer name with an HTML-breakout payload to prove e2e escaping.
BOTNAME='<script>bot'

for f in "$TOOL" "$MOD" "$NGINX"; do
  [ -x "$f" ] || [ -f "$f" ] || { echo "MISSING: $f (build it first)"; exit 2; }
done

rm -rf "$BASE"
mkdir -p "$BASE"/{www,logs,cache,client_body_temp,proxy_temp,fastcgi_temp,uwsgi_temp,scgi_temp}
echo "ok" > "$BASE/www/wba-probe"

# Operator key directory + a fresh valid signature for a verified request.
"$TOOL" --emit=jwks --kid="$KID" > "$BASE/jwks.json"
"$TOOL" --emit=headers --kid="$KID" --method=GET --authority=localhost \
  --path=/wba-probe > "$BASE/valid.txt"
V_SIGINPUT=$(sed -n 's/^SIGINPUT=//p' "$BASE/valid.txt")
V_SIG=$(sed -n 's/^SIG=//p' "$BASE/valid.txt")

cat > "$BASE/nginx.conf" <<CONF
load_module $MOD;
worker_processes 1;
pid $BASE/nginx.pid;
error_log $BASE/logs/error.log info;
# The secret bearer token gating the EXACT doc: passed to the worker env.
env PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN=$TOKEN;
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
  pagespeed WebBotAuthKeyDirectoryFile $BASE/jwks.json;
  pagespeed WebBotAuthVerifiedBots "$KID=$BOTNAME";

  # Three server blocks, one per counter mode, sharing the single global
  # counter file (derived from the shared FileCachePath).
  server {
    listen 127.0.0.1:$PORT;
    server_name localhost;
    root $BASE/www;
    pagespeed WebBotAuthPublicCounter off;
    # The counter endpoint is served by pagespeed's content handler, so it needs
    # its own location: a catch-all try_files would 404 it in precontent before
    # pagespeed could route it (same requirement as the statistics page).
    location = /.well-known/webbotauth-counter { }
    location / { try_files \$uri =404; }
  }
  server {
    listen 127.0.0.1:$PORT_PRIV;
    server_name localhost;
    root $BASE/www;
    pagespeed WebBotAuthPublicCounter private;
    location = /.well-known/webbotauth-counter { }
    location / { try_files \$uri =404; }
  }
  server {
    listen 127.0.0.1:$PORT_PUB;
    server_name localhost;
    root $BASE/www;
    pagespeed WebBotAuthPublicCounter public;
    location = /.well-known/webbotauth-counter { }
    location / { try_files \$uri =404; }
  }
}
CONF

"$NGINX" -t -c "$BASE/nginx.conf" -p "$BASE"
"$NGINX" -c "$BASE/nginx.conf" -p "$BASE"
trap '"$NGINX" -c "$BASE/nginx.conf" -p "$BASE" -s stop 2>/dev/null || true' EXIT
sleep 1

CTR=/.well-known/webbotauth-counter

fail=0
check() {  # check <label> <expected> <actual>
  if [ "$3" = "$2" ]; then echo "PASS  $1 -> '$3'"; else echo "FAIL  $1 -> got '$3', want '$2'"; fail=1; fi
}
# HTTP status code for a request. Args after the URL go to curl.
code() {
  local url="$1"; shift
  curl -s -o /dev/null -w '%{http_code}' "$@" "$url"
}
# Full response (headers + body) for a request.
dump() {
  local url="$1"; shift
  curl -s -D - "$@" "$url"
}

# Send one verified signed request through the public server so the counter
# records a verified verdict + claims a per-signer slot for $KID.
curl -s -o /dev/null -H "Host: localhost" -H "User-Agent: TestBot/1.0" \
  -H "Signature-Input: $V_SIGINPUT" -H "Signature: $V_SIG" \
  "http://127.0.0.1:$PORT_PUB/wba-probe"

# --- Mode gating + hardening -------------------------------------------------
check "off-404          " "404" "$(code http://127.0.0.1:$PORT$CTR)"

pub_coarse=$(dump "http://127.0.0.1:$PORT_PUB$CTR")
check "public-coarse-200" "200" "$(code http://127.0.0.1:$PORT_PUB$CTR)"
echo "$pub_coarse" | grep -qi '^Cache-Control: public, max-age=300' \
  && check "coarse-maxage    " "y" "y" || check "coarse-maxage    " "y" "n"
echo "$pub_coarse" | grep -qi '^Vary: Authorization' \
  && check "coarse-vary      " "y" "y" || check "coarse-vary      " "y" "n"
echo "$pub_coarse" | grep -qi '^X-Content-Type-Options: nosniff' \
  && check "coarse-nosniff   " "y" "y" || check "coarse-nosniff   " "y" "n"
# Coarse doc: bucketed tiers, NO boot_id / since / kid.
echo "$pub_coarse" | grep -q '"verified_total":"' \
  && check "coarse-tiered    " "y" "y" || check "coarse-tiered    " "y" "n"
echo "$pub_coarse" | grep -Eq 'boot_id|"since"|"kid"' \
  && check "coarse-no-exact  " "y" "n" || check "coarse-no-exact  " "y" "y"

pub_exact=$(dump "http://127.0.0.1:$PORT_PUB$CTR" -H "Authorization: Bearer $TOKEN")
check "public-exact-200 " "200" \
  "$(code http://127.0.0.1:$PORT_PUB$CTR -H "Authorization: Bearer $TOKEN")"
echo "$pub_exact" | grep -qi '^Cache-Control: no-store' \
  && check "exact-nostore    " "y" "y" || check "exact-nostore    " "y" "n"
echo "$pub_exact" | grep -q '"boot_id":"' \
  && check "exact-boot_id    " "y" "y" || check "exact-boot_id    " "y" "n"
# The verified request above must be reflected + the per-signer name escaped.
echo "$pub_exact" | grep -Eq '"verified_total":[1-9]' \
  && check "exact-verified>=1" "y" "y" || check "exact-verified>=1" "y" "n"
echo "$pub_exact" | grep -q '\\u003cscript\\u003ebot' \
  && check "exact-name-escaped" "y" "y" || check "exact-name-escaped" "y" "n"
echo "$pub_exact" | grep -q '<script>' \
  && check "exact-no-raw-html" "y" "n" || check "exact-no-raw-html" "y" "y"

# Wrong token on the public server -> falls back to the coarse doc, not exact.
pub_wrong=$(dump "http://127.0.0.1:$PORT_PUB$CTR" -H "Authorization: Bearer nope")
echo "$pub_wrong" | grep -Eq 'boot_id|"since"|"kid"' \
  && check "wrongtoken-coarse" "y" "n" || check "wrongtoken-coarse" "y" "y"

# --- Private mode ------------------------------------------------------------
check "private-404       " "404" "$(code http://127.0.0.1:$PORT_PRIV$CTR)"
check "private-token-200 " "200" \
  "$(code http://127.0.0.1:$PORT_PRIV$CTR -H "Authorization: Bearer $TOKEN")"
priv_exact=$(dump "http://127.0.0.1:$PORT_PRIV$CTR" -H "Authorization: Bearer $TOKEN")
echo "$priv_exact" | grep -Eq '"since":"[0-9]{4}-[0-9]{2}-[0-9]{2}"' \
  && check "private-since     " "y" "y" || check "private-since     " "y" "n"

# --- Method + HEAD -----------------------------------------------------------
# HEAD: headers only, no body.
head_body=$(curl -s -I "http://127.0.0.1:$PORT_PUB$CTR" | tail -c 3 | tr -d '\r\n ')
check "head-no-body      " "" "$head_body"
check "post-404          " "404" "$(code http://127.0.0.1:$PORT_PUB$CTR -X POST)"

[ "$fail" -eq 0 ] && echo "WEBBOTAUTH COUNTER SMOKE: PASS" \
  || { echo "WEBBOTAUTH COUNTER SMOKE: FAIL"; exit 1; }
