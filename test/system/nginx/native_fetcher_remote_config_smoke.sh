#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# Smoke for the interim #301 change: with the native fetcher in use, a
# remote configuration refresh must not pause the worker on the request
# path.
#
# One vhost runs UseNativeFetcher on with RemoteConfigurationUrl aimed at
# pathological_server.py's /slightly-slow: a valid remote configuration
# (EnableFilters remove_comments,collapse_whitespace) that the server
# delays by two seconds and caches for twenty.  RemoteConfigurationTimeoutMs
# is 3000, longer than the delay, so a worker that waits the timeout out on
# the request path is measurable.  Assertions:
#
#   1. the config-time warning is logged;
#   2. no HTML response takes anywhere near the timeout: before the fix the
#      first request blocked the event loop for the full 3000 ms, which is
#      also why the fetch could never complete during the wait;
#   3. the remote configuration is applied eventually: the refresh
#      completes once the event loop is free, fills the cache, and the next
#      requests apply it (a cached copy must still apply with the wait
#      disabled);
#   4. no crash markers in the error log.
#
# Usage:
#   ./test/system/nginx/native_fetcher_remote_config_smoke.sh
#
# Requires: a built ngx_pagespeed_module.so in bazel-bin (or MODULE=... env),
# nginx, python3, curl. Runs its own nginx and stops only that nginx, by the
# pid file it owns; other nginx processes on the host are left alone.
set -u

WORKSPACE="${WORKSPACE:-/workspace}"
MODULE="${MODULE:-$WORKSPACE/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so}"
NGINX_BIN="${NGINX_BINARY:-/usr/local/src/nginx/objs/nginx}"
D=/tmp/native-remote-config-smoke
PORT="${BASE_PORT:-8521}"
RCPORT="${RCPORT:-$((PORT + 1))}"
TIMEOUT_MS=3000      # RemoteConfigurationTimeoutMs; longer than the delay
STALL_LIMIT_S=1.5    # every response must stay well under this
APPLY_DEADLINE_S=25  # fetch_until budget for the config to land

FAILURES=0
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }

stop_nginx() {  # stop the nginx this script started (its pid file), no other
  [ -f "$D/nginx.pid" ] || return 0
  NGINX_PID=$(cat "$D/nginx.pid" 2>/dev/null)
  case "$NGINX_PID" in
    ''|*[!0-9]*) return 0 ;;  # empty or garbage: nothing we own to signal
  esac
  kill -QUIT "$NGINX_PID" 2>/dev/null || return 0
  for _ in $(seq 1 20); do
    kill -0 "$NGINX_PID" 2>/dev/null || return 0
    sleep 0.1
  done
  kill -KILL "$NGINX_PID" 2>/dev/null
}

[ -f "$MODULE" ] || { echo "FAIL: no module at $MODULE"; exit 1; }
[ -x "$NGINX_BIN" ] || { echo "FAIL: no nginx binary at $NGINX_BIN"; exit 1; }
command -v python3 > /dev/null 2>&1 \
  || { echo "FAIL: python3 not found"; exit 1; }

stop_nginx  # a previous run killed outright may have left its master alive
rm -rf "$D"; mkdir -p "$D/logs" "$D/cache" "$D/www"

# The remote configuration origin: slow but valid.
python3 "$WORKSPACE/pagespeed/system/pathological_server.py" "$RCPORT" \
  > "$D/logs/pathological.log" 2>&1 &
RC_PID=$!
trap 'kill "$RC_PID" 2>/dev/null; stop_nginx' EXIT

for _ in $(seq 1 20); do
  curl -sf "http://127.0.0.1:$RCPORT/standard" > /dev/null && break
  sleep 1
done
curl -sf "http://127.0.0.1:$RCPORT/standard" > /dev/null \
  || { echo "FAIL: pathological server did not come up"; exit 1; }

# The page's comment only disappears once the remote configuration is
# applied: remove_comments is not part of CoreFilters.
cat > "$D/www/commented.html" <<'EOF'
<html>
<head><title>remote config smoke</title></head>
<body>
<!-- remove me: only the remote configuration removes this -->
Hello, remote configuration.
</body>
</html>
EOF

RESOLVER=$(awk '/^nameserver/{print $2; exit}' /etc/resolv.conf)

cat > "$D/nginx.conf" <<EOF
worker_processes 1;
pid $D/nginx.pid;
error_log $D/logs/error.log info;
load_module $MODULE;
events { worker_connections 512; }
http {
    types { text/html html; }
    default_type application/octet-stream;
    access_log off;
    client_body_temp_path $D/client_body_temp;
    proxy_temp_path $D/proxy_temp;
    resolver ${RESOLVER:-127.0.0.1} ipv6=off;
    pagespeed UseNativeFetcher on;

    server {
        listen $PORT;
        server_name localhost;
        root $D/www;
        pagespeed on;
        pagespeed FileCachePath $D/cache;
        pagespeed RewriteLevel CoreFilters;
        pagespeed RemoteConfigurationUrl http://127.0.0.1:$RCPORT/slightly-slow;
        pagespeed RemoteConfigurationTimeoutMs $TIMEOUT_MS;
    }
}
EOF

"$NGINX_BIN" -p "$D" -c "$D/nginx.conf" \
  || { echo "FAIL: nginx would not start"; exit 1; }
sleep 1

URL="http://127.0.0.1:$PORT/commented.html"

fetch() {  # sets TIME_S and BODY for $URL
  TIME_S=$(curl -s -o "$D/body.html" -w '%{time_total}' "$URL")
  BODY=$(cat "$D/body.html")
}

under_limit() {  # under_limit <seconds>: exit 0 when below STALL_LIMIT_S
  awk -v t="$1" -v l="$STALL_LIMIT_S" 'BEGIN { exit !(t < l) }'
}

# 2. The first request is the cold-cache one; before the fix it stalled the
# worker for the full timeout, so no warm-up request may precede it.
fetch
echo "first response: ${TIME_S}s"
under_limit "$TIME_S" \
  || fail "first response took ${TIME_S}s (limit ${STALL_LIMIT_S}s): the \
worker stalled on the remote configuration refresh"

# 3. Applied eventually, with no response stalling along the way.
APPLIED=0
for _ in $(seq 1 "$APPLY_DEADLINE_S"); do
  under_limit "$TIME_S" \
    || fail "response took ${TIME_S}s (limit ${STALL_LIMIT_S}s) while the \
remote configuration was landing"
  case "$BODY" in
    *'<!--'*) ;;
    *) APPLIED=1; break ;;
  esac
  fetch
  sleep 1
done
[ "$APPLIED" = 1 ] \
  || fail "remote configuration not applied within ${APPLY_DEADLINE_S}s"
echo "last response: ${TIME_S}s, comments removed"

# 1. The config-time warning (file cache here, so the "will not wait" form).
grep -q "will not wait for a remote configuration refresh" \
  "$D/logs/error.log" \
  || fail "no config-time warning for RemoteConfigurationUrl with \
UseNativeFetcher"

# 4. No crash markers.
CRASH=$(grep -cE \
  "exited on signal|signal [0-9]+ .SIG(SEGV|ABRT|BUS|ILL|FPE)|Check failed|CRITICAL" \
  "$D/logs/error.log" || true)
[ "$CRASH" -eq 0 ] || fail "$CRASH crash markers in error.log"

stop_nginx
if [ "$FAILURES" -eq 0 ]; then
  echo "PASS: native fetcher remote config smoke"
  exit 0
fi
echo "$FAILURES failure(s); error log preserved at $D/logs/error.log"
exit 1
