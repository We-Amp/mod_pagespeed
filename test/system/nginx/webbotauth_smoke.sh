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
#   4. a SCANNER-PROBE-shaped signature: covered components
#      ("@authority" "signature-agent"), created/expires/nonce/
#      tag="web-bot-auth", plus a Signature-Agent request header
#                                              -> "testbot, ed25519-verified"
#   5. a valid-crypto signature with a NON-web-bot-auth tag   -> "human"
#      (not our material; classified as if unsigned)
#   6. the RFC 9421 section 4.1 intermediary case: a CDN-style signature on
#      the FIRST Signature-Input/Signature field lines and the tagged bot
#      signature on SECOND field lines -- only a wiring that ", "-joins
#      repeated lines (RFC 9110) can see it
#                                              -> "testbot, ed25519-verified"
#   7. a bare Signature header without Signature-Input        -> "unknown"
#      (fail-closed; e.g. draft-cavage/ActivityPub signers)
#   8. a signature over an ESCAPED wire path (/wba%2Dprobe) -- @path must
#      bind the raw request-target bytes, not nginx's decoded r->uri
#                                              -> "testbot, ed25519-verified"
#   9. a SAME-LINE multi-signature dictionary (CDN member + tagged member in
#      one Signature-Input/Signature line)     -> "testbot, ed25519-verified"
#  10. a warmed key whose kid is NOT in the verified-bot registry
#                                              -> "signed-agent"
#  11. an INDEX internal redirect (GET /dir/ -> /dir/index.html): classified
#      once, verdict header preserved, and -- asserted via the statistics
#      page -- each telemetry counter ticks EXACTLY ONCE per client
#      transaction across the whole run (internal redirects and repeated
#      phase entries never double-count).
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
# The JWKS carries TWO kids over the same deterministic key: the registered
# verified-bot kid and an unregistered one (-> "signed-agent").
J1=$("$TOOL" --emit=jwks --kid="$KID")
J2=$("$TOOL" --emit=jwks --kid=unreg-kid)
K1=${J1#'{"keys":['}; K1=${K1%']}'}
K2=${J2#'{"keys":['}; K2=${K2%']}'}
printf '{"keys":[%s,%s]}' "$K1" "$K2" > "$BASE/jwks.json"
"$TOOL" --emit=headers --kid="$KID" --method=GET --authority=localhost --path=/wba-probe > "$BASE/valid.txt"
"$TOOL" --emit=headers --kid="$KID" --method=GET --authority=localhost --path=/wba-probe --tamper > "$BASE/tampered.txt"
V_SIGINPUT=$(sed -n 's/^SIGINPUT=//p' "$BASE/valid.txt")
V_SIG=$(sed -n 's/^SIG=//p' "$BASE/valid.txt")
T_SIGINPUT=$(sed -n 's/^SIGINPUT=//p' "$BASE/tampered.txt")
T_SIG=$(sed -n 's/^SIG=//p' "$BASE/tampered.txt")

# Scanner-probe shape: covered components ("@authority" "signature-agent"),
# created/expires/nonce/tag, and the Signature-Agent header the base covers
# (an sf-string, so the value INCLUDES the quotes).
NOW=$(date +%s)
SIG_AGENT='"https://keys.example.com/.well-known/http-message-signatures-directory"'
"$TOOL" --emit=headers --kid="$KID" --authority=localhost \
  --components=@authority,signature-agent \
  --field=signature-agent="$SIG_AGENT" \
  --created="$NOW" --expires=$((NOW + 300)) \
  --nonce=probe-nonce-smoke --tag=web-bot-auth > "$BASE/scanner.txt"
S_SIGINPUT=$(sed -n 's/^SIGINPUT=//p' "$BASE/scanner.txt")
S_SIG=$(sed -n 's/^SIG=//p' "$BASE/scanner.txt")

# Valid crypto, but tagged for some OTHER signing scheme: not our material.
"$TOOL" --emit=headers --kid="$KID" --method=GET --authority=localhost \
  --path=/wba-probe --tag=other-protocol > "$BASE/othertag.txt"
O_SIGINPUT=$(sed -n 's/^SIGINPUT=//p' "$BASE/othertag.txt")
O_SIG=$(sed -n 's/^SIG=//p' "$BASE/othertag.txt")

# CDN-style untagged member for the split-line intermediary case.
CDN_SIGINPUT='cdn=("@method");created=1;keyid="cdn-key";alg="rsa-v1_5-sha256"'
CDN_SIG='cdn=:QUJD:'

# Escaped wire path: %2D decodes to '-', so nginx serves /wba-probe while the
# raw request-target stays /wba%2Dprobe -- the signature is over the WIRE
# bytes and only an @path binding to the unparsed URI verifies it.
"$TOOL" --emit=headers --kid="$KID" --method=GET --authority=localhost \
  --path='/wba%2Dprobe' > "$BASE/escaped.txt"
E_SIGINPUT=$(sed -n 's/^SIGINPUT=//p' "$BASE/escaped.txt")
E_SIG=$(sed -n 's/^SIG=//p' "$BASE/escaped.txt")

# Warmed key, kid NOT in the verified-bot registry -> "signed-agent".
"$TOOL" --emit=headers --kid=unreg-kid --authority=localhost \
  --components=@authority,signature-agent \
  --field=signature-agent="$SIG_AGENT" \
  --created="$NOW" --expires=$((NOW + 300)) --tag=web-bot-auth > "$BASE/unreg.txt"
U_SIGINPUT=$(sed -n 's/^SIGINPUT=//p' "$BASE/unreg.txt")
U_SIG=$(sed -n 's/^SIG=//p' "$BASE/unreg.txt")

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
  pagespeed StatisticsPath /ngx_pagespeed_statistics;

  server {
    listen 127.0.0.1:$PORT;
    server_name localhost;
    root $BASE/www;
    add_header X-Verified-Bot \$x_verified_bot always;
    # Statistics page for the counter-delta assertions. Needs its own
    # location: the catch-all's try_files would 404 it in precontent before
    # the pagespeed content handler could route it.
    location /ngx_pagespeed_statistics { }
    # Index resolution triggers an INTERNAL REDIRECT (/dir/ -> the index
    # file), re-entering the precontent phase -- the once-per-transaction
    # guard must keep the verdict and tick each counter exactly once.
    location /dir/ { index index.html; }
    location / { try_files \$uri =404; }
  }
}
CONF

mkdir -p "$BASE/www/dir"
echo "indexed" > "$BASE/www/dir/index.html"

"$NGINX" -t -c "$BASE/nginx.conf" -p "$BASE"
"$NGINX" -c "$BASE/nginx.conf" -p "$BASE"
trap '"$NGINX" -c "$BASE/nginx.conf" -p "$BASE" -s stop 2>/dev/null || true' EXIT
sleep 1

verdict() {
  curl -fsS -D - -o /dev/null "$@" "http://127.0.0.1:$PORT/wba-probe" \
    | awk -F': ' 'tolower($1)=="x-verified-bot"{print $2}' | tr -d '\r'
}

# Verdict for an arbitrary URL (first arg), remaining args go to curl.
verdict_url() {
  local url="$1"; shift
  curl -fsS -D - -o /dev/null "$@" "http://127.0.0.1:$PORT$url" \
    | awk -F': ' 'tolower($1)=="x-verified-bot"{print $2}' | tr -d '\r'
}

# Read one counter off the pagespeed statistics page (a JSON document:
# {"variables": {"name": value, ...}}).
stat() {
  curl -fsS "http://127.0.0.1:$PORT/ngx_pagespeed_statistics" \
    | tr ',' '\n' | sed -n "s/.*\"$1\": *\([0-9][0-9]*\).*/\1/p" | head -1
}

fail=0
check() {  # check <label> <expected> <actual>
  if [ "$3" = "$2" ]; then echo "PASS  $1 -> '$3'"; else echo "FAIL  $1 -> got '$3', want '$2'"; fail=1; fi
}

c1=$(verdict -H "Host: localhost" -H "User-Agent: TestBot/1.0" -H "Signature-Input: $V_SIGINPUT" -H "Signature: $V_SIG")
c2=$(verdict -H "Host: localhost" -H "User-Agent: GPTBot")
c3=$(verdict -H "Host: localhost" -H "User-Agent: TestBot/1.0" -H "Signature-Input: $T_SIGINPUT" -H "Signature: $T_SIG")
c4=$(verdict -H "Host: localhost" -H "User-Agent: RenderPeek-SignedAgentProbe/1.0" \
     -H "Signature-Agent: $SIG_AGENT" -H "Signature-Input: $S_SIGINPUT" -H "Signature: $S_SIG")
c5=$(verdict -H "Host: localhost" -H "User-Agent: SomeOtherScheme/1.0" \
     -H "Signature-Input: $O_SIGINPUT" -H "Signature: $O_SIG")
# Repeated field lines: the CDN member on the FIRST Signature-Input/Signature
# lines, the tagged bot member on the SECOND (curl sends each -H verbatim as
# its own field line).
c6=$(verdict -H "Host: localhost" -H "User-Agent: RenderPeek-SignedAgentProbe/1.0" \
     -H "Signature-Agent: $SIG_AGENT" \
     -H "Signature-Input: $CDN_SIGINPUT" -H "Signature: $CDN_SIG" \
     -H "Signature-Input: $S_SIGINPUT" -H "Signature: $S_SIG")
c7=$(verdict -H "Host: localhost" -H "User-Agent: LegacySigner/1.0" -H "Signature: $V_SIG")
# Escaped wire path (signed over /wba%2Dprobe; nginx serves /wba-probe).
c8=$(verdict_url "/wba%2Dprobe" -H "Host: localhost" -H "User-Agent: TestBot/1.0" \
     -H "Signature-Input: $E_SIGINPUT" -H "Signature: $E_SIG")
# Same-line multi-signature dictionary: CDN member + tagged member in ONE line.
c9=$(verdict -H "Host: localhost" -H "User-Agent: RenderPeek-SignedAgentProbe/1.0" \
     -H "Signature-Agent: $SIG_AGENT" \
     -H "Signature-Input: $CDN_SIGINPUT, $S_SIGINPUT" \
     -H "Signature: $CDN_SIG, $S_SIG")
# Warmed key, unregistered kid.
c10=$(verdict -H "Host: localhost" -H "User-Agent: RenderPeek-SignedAgentProbe/1.0" \
     -H "Signature-Agent: $SIG_AGENT" -H "Signature-Input: $U_SIGINPUT" -H "Signature: $U_SIG")
# Index internal redirect: /dir/ -> /dir/index.html re-enters the phase; the
# scanner-shaped signature (path-independent) must classify once and keep its
# verdict on the final response.
c11=$(verdict_url "/dir/" -H "Host: localhost" -H "User-Agent: RenderPeek-SignedAgentProbe/1.0" \
     -H "Signature-Agent: $SIG_AGENT" -H "Signature-Input: $S_SIGINPUT" -H "Signature: $S_SIG")

check "valid-signature " "$BOT, ed25519-verified" "$c1"
check "no-signature    " "human"                  "$c2"
check "tampered-sig    " "unknown"                 "$c3"
check "scanner-probe   " "$BOT, ed25519-verified" "$c4"
check "other-tag       " "human"                  "$c5"
check "split-lines     " "$BOT, ed25519-verified" "$c6"
check "bare-signature  " "unknown"                 "$c7"
check "escaped-path    " "$BOT, ed25519-verified" "$c8"
check "same-line-multi " "$BOT, ed25519-verified" "$c9"
check "unregistered-kid" "signed-agent"           "$c10"
check "index-redirect  " "$BOT, ed25519-verified" "$c11"

# Counter deltas over the whole run (worker_processes 1, fresh statistics):
# verified/signed-agent verdicts: c1, c4, c6, c8, c9, c10, c11 = 7 -- the
# index-redirect transaction counts ONCE despite re-entering the phase.
# other-signature verdicts: c5 = 1.
V_COUNT=$(stat web_bot_auth_verified_signed_requests)
O_COUNT=$(stat web_bot_auth_other_signature_requests)
check "verified-counter" "7" "${V_COUNT:-unreadable}"
check "other-counter   " "1" "${O_COUNT:-unreadable}"

[ "$fail" -eq 0 ] && echo "WEBBOTAUTH SMOKE: PASS" || { echo "WEBBOTAUTH SMOKE: FAIL"; exit 1; }
