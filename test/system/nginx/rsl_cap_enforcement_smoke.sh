#!/bin/bash
# AgentPass A3 live Day-0 smoke for the nginx RSL-CAP enforcement
# wiring. PAID sibling of webbotauth_smoke.sh (the FREE observe-only A1 verifier).
#
# Proves, against a RUNNING nginx loading ngx_pagespeed_module.so, that the
# RSL-CAP capability-token validator's verdict is mapped to inline HTTP status:
#   default-OFF (no RslCapEnforcement directive)        -> 200 (passthrough)
#   enabled + valid token granting the route's lic+scope -> 200 (allow)
#   enabled + NO Authorization token                     -> 401
#   enabled + valid token but lic/scope NOT granted      -> 402
#   enabled + EXPIRED token                              -> 401
#   enabled + TAMPERED signature                         -> 401
#   enabled + valid token but iss != pinned issuer       -> 401 (issuer pin)
#   enabled + oversized Authorization header             -> 401 (header cap)
#
# Tokens are minted by //pagespeed/kernel/webbotauth:webbotauth_sign_tool
# (--emit=rslcap), signed by the SAME deterministic key the emitted JWKS
# publishes, so the bytes the validator checks are bit-identical. NOT shipped.
#
# REPRODUCE (inside the pagespeed1.1-dev container, repo mounted at /workspace):
#   bazel build --config=ci \
#     //pagespeed/nginx:ngx_pagespeed_module.so \
#     //pagespeed/kernel/webbotauth:webbotauth_sign_tool
#   bash test/system/nginx/rsl_cap_enforcement_smoke.sh
#
# Override the artifact locations with TOOL=, MOD=, NGINX=, PORT= if needed.
set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
TOOL="${TOOL:-$WORKSPACE/bazel-bin/pagespeed/kernel/webbotauth/webbotauth_sign_tool}"
MOD="${MOD:-$WORKSPACE/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so}"
NGINX="${NGINX:-/usr/local/src/nginx/objs/nginx}"
PORT="${PORT:-8998}"        # enforcement-ON server
PORT_OFF="${PORT_OFF:-8997}"  # enforcement-OFF (default) server
KID="${KID:-test-key-1}"
ISS="${ISS:-issuer.example}"
LIC="${LIC:-premium}"
SCOPE="${SCOPE:-render}"
BASE="${BASE:-/tmp/rce_smoke}"

for f in "$TOOL" "$MOD" "$NGINX"; do
  [ -x "$f" ] || [ -f "$f" ] || { echo "MISSING: $f (build it first)"; exit 2; }
done

rm -rf "$BASE"
mkdir -p "$BASE"/{www,logs,cache,client_body_temp,proxy_temp,fastcgi_temp,uwsgi_temp,scgi_temp}
echo "ok" > "$BASE/www/rce-probe"

# Operator key directory (issuer public key).
"$TOOL" --emit=jwks --kid="$KID" > "$BASE/jwks.json"

# Mint tokens (each prints "TOKEN=License <token>"); strip the TOKEN= label.
mint() { "$TOOL" --emit=rslcap --kid="$KID" "$@" | sed -n 's/^TOKEN=//p'; }
T_OK=$(mint   --iss="$ISS"        --license="$LIC"   --scope="$SCOPE")
T_UNLIC=$(mint --iss="$ISS"        --license=basic    --scope=other)
T_EXP=$(mint  --iss="$ISS"        --license="$LIC"   --scope="$SCOPE" --exp-in=-3600)
T_TAMP=$(mint --iss="$ISS"        --license="$LIC"   --scope="$SCOPE" --tamper)
T_WISS=$(mint --iss=evil.example  --license="$LIC"   --scope="$SCOPE")

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

  # Enforcement-ON server.
  server {
    listen 127.0.0.1:$PORT;
    server_name localhost;
    root $BASE/www;
    pagespeed RslCapEnforcement on;
    pagespeed RslCapKeyDirectoryFile $BASE/jwks.json;
    pagespeed RslCapRequestedLicense "$LIC";
    pagespeed RslCapRequestedScope "$SCOPE";
    pagespeed RslCapIssuer "$ISS";
    # Allow a large Authorization header line through nginx's own buffer so the
    # oversized-header case reaches the handler's RSL-CAP cap (-> 401) instead of
    # being rejected by nginx with 414/400 first.
    large_client_header_buffers 4 64k;
    location / { try_files \$uri =404; }
  }

  # Default-OFF server (no RslCap directives): must pass through untouched.
  server {
    listen 127.0.0.1:$PORT_OFF;
    server_name localhost;
    root $BASE/www;
    location / { try_files \$uri =404; }
  }
}
CONF

# Invoke nginx with the NGINX env var cleared: nginx treats a set NGINX
# environment variable as a list of inherited socket fds (its hot-upgrade
# protocol), which suppresses daemonization and would make this script block
# forever on a foreground nginx. (This matters when the harness exports NGINX=
# to point at the binary, as the reproduce command does.)
NGINX_RUN() { env -u NGINX "$NGINX" "$@"; }
NGINX_RUN -t -c "$BASE/nginx.conf" -p "$BASE"
NGINX_RUN -c "$BASE/nginx.conf" -p "$BASE"
trap 'NGINX_RUN -c "$BASE/nginx.conf" -p "$BASE" -s stop 2>/dev/null || true' EXIT
sleep 1

# Returns the HTTP status code for a request to the enforcement-ON server.
code() {
  curl -s --max-time 10 -o /dev/null -w '%{http_code}' -H "Host: localhost" "$@" \
    "http://127.0.0.1:$PORT/rce-probe"
}
code_off() {
  curl -s --max-time 10 -o /dev/null -w '%{http_code}' -H "Host: localhost" "$@" \
    "http://127.0.0.1:$PORT_OFF/rce-probe"
}

fail=0
check() {  # check <label> <expected> <actual>
  if [ "$3" = "$2" ]; then echo "PASS  $1 -> $3"; else echo "FAIL  $1 -> got $3, want $2"; fail=1; fi
}

BIG="License $(head -c 9000 < /dev/zero | tr '\0' 'A')"

check "default-off passthrough" "200" "$(code_off)"
check "authorized             " "200" "$(code -H "Authorization: $T_OK")"
check "no-token               " "401" "$(code)"
check "unlicensed             " "402" "$(code -H "Authorization: $T_UNLIC")"
check "expired                " "401" "$(code -H "Authorization: $T_EXP")"
check "tampered               " "401" "$(code -H "Authorization: $T_TAMP")"
check "wrong-issuer (pin)     " "401" "$(code -H "Authorization: $T_WISS")"
check "oversized-header       " "401" "$(code -H "Authorization: $BIG")"

[ "$fail" -eq 0 ] && echo "RSL-CAP ENFORCEMENT SMOKE: PASS" || { echo "RSL-CAP ENFORCEMENT SMOKE: FAIL"; exit 1; }
