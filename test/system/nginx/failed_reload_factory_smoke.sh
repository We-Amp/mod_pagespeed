#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# Smoke for the per-cycle driver factory across a FAILED nginx reload
# (issue #377).
#
# A reload builds the new configuration cycle (and its driver factory) while
# the old cycle keeps serving.  When the new cycle fails to start, nginx
# discards it and the old cycle stays live.  The discarded cycle must not
# disturb the live cycle's factory, and must not leak its own.  Two failing
# reloads are driven, both failing at the listen socket after the
# configuration parsed:
#
#   A. the new configuration still has a pagespeed server;
#   B. the new configuration has no pagespeed server at all.
#
# Assertions after each failed reload: nginx reports the failure, the old
# workers keep serving the optimized page, and a following good reload
# succeeds.  Finally the master quits cleanly and the error log carries no
# crash markers.  A leak or a mis-tracked factory is only visible on a
# sanitizer build (run the script with a module built with LSan/ASan); on
# a regular build it guards the crash and hang shapes.
#
# Usage:
#   ./test/system/nginx/failed_reload_factory_smoke.sh
#
# Requires: a built ngx_pagespeed_module.so in bazel-bin (or MODULE=... env),
# nginx, python3, curl.  Runs its own nginx and stops only that nginx, by the
# pid file it owns; other nginx processes on the host are left alone.
set -u

WORKSPACE="${WORKSPACE:-/workspace}"
MODULE="${MODULE:-$WORKSPACE/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so}"
NGINX_BIN="${NGINX_BINARY:-/usr/local/src/nginx/objs/nginx}"
D=/tmp/failed-reload-factory-smoke
PORT="${BASE_PORT:-8531}"
BLOCKED_PORT="${BLOCKED_PORT:-$((PORT + 1))}"

FAILURES=0
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }

stop_nginx() {  # stop the nginx this script started (its pid file), no other
  [ -f "$D/nginx.pid" ] || return 0
  NGINX_PID=$(cat "$D/nginx.pid" 2>/dev/null)
  case "$NGINX_PID" in
    ''|*[!0-9]*) return 0 ;;  # empty or garbage: nothing we own to signal
  esac
  kill -QUIT "$NGINX_PID" 2>/dev/null || return 0
  for _ in $(seq 1 50); do
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

# Hold BLOCKED_PORT so that a server listening on it cannot start.
python3 -c '
import socket, sys, time
s = socket.socket()
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(1)
time.sleep(600)
' "$BLOCKED_PORT" > "$D/logs/blocker.log" 2>&1 &
BLOCKER_PID=$!
trap 'kill "$BLOCKER_PID" 2>/dev/null; stop_nginx' EXIT

cat > "$D/www/commented.html" <<'HTML'
<html>
<head><title>failed reload smoke</title></head>
<body>
<!-- remove me: remove_comments is not in CoreFilters -->
Hello.
</body>
</html>
HTML

# $1 = extra http-level text, $2 = extra server blocks
write_conf() {
  cat > "$D/nginx.conf" <<CONF
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
    $1
    server {
        listen $PORT;
        server_name localhost;
        root $D/www;
        $3
    }
    $2
}
CONF
}

PS_SERVER='pagespeed on;
        pagespeed FileCachePath '"$D"'/cache;
        pagespeed RewriteLevel CoreFilters;
        pagespeed EnableFilters remove_comments;'
BAD_PS_SERVER="server { listen 127.0.0.1:$BLOCKED_PORT; root $D/www; $PS_SERVER }"
BAD_PLAIN_SERVER="server { listen 127.0.0.1:$BLOCKED_PORT; root $D/www; }"

URL="http://127.0.0.1:$PORT/commented.html"

serves_optimized() {  # 0 when the page comes back with its comment removed
  local body
  body=$(curl -s --max-time 10 "$URL")
  case "$body" in
    *'<!--'*) return 1 ;;
    *Hello*) return 0 ;;
    *) return 1 ;;
  esac
}

reload() {  # reload and give the master a moment to finish the attempt
  kill -HUP "$(cat "$D/nginx.pid")"
  sleep 2
}

write_conf "" "" "$PS_SERVER"
"$NGINX_BIN" -p "$D" -c "$D/nginx.conf" \
  || { echo "FAIL: nginx would not start"; exit 1; }
sleep 1
# The first request also warms the cache; the optimized form needs a second.
curl -s --max-time 10 "$URL" > /dev/null
sleep 1
serves_optimized || fail "baseline: the page is not served optimized"

for CASE in A B; do
  FAILED_BEFORE=$(grep -c "bind() to 127.0.0.1:$BLOCKED_PORT failed" \
    "$D/logs/error.log")
  if [ "$CASE" = A ]; then
    write_conf "" "$BAD_PS_SERVER" "$PS_SERVER"
  else
    # No pagespeed directive anywhere in the new configuration.
    write_conf "" "$BAD_PLAIN_SERVER" ""
  fi
  reload
  FAILED_AFTER=$(grep -c "bind() to 127.0.0.1:$BLOCKED_PORT failed" \
    "$D/logs/error.log")
  [ "$FAILED_AFTER" -gt "$FAILED_BEFORE" ] \
    || fail "case $CASE: the reload did not fail at the listen socket"
  kill -0 "$(cat "$D/nginx.pid")" 2>/dev/null \
    || fail "case $CASE: the master is gone after a failed reload"
  serves_optimized \
    || fail "case $CASE: the old configuration stopped serving after the failed reload"

  # A good reload must still work afterwards.
  write_conf "" "" "$PS_SERVER"
  reload
  curl -s --max-time 10 "$URL" > /dev/null
  sleep 1
  serves_optimized || fail "case $CASE: not served optimized after a good reload"
done

stop_nginx
for _ in $(seq 1 50); do
  [ -f "$D/nginx.pid" ] || break
  sleep 0.1
done

CRASH=$(grep -cE \
  "exited on signal|signal [0-9]+ .SIG(SEGV|ABRT|BUS|ILL|FPE)|Check failed|CRITICAL" \
  "$D/logs/error.log" || true)
[ "$CRASH" -eq 0 ] || fail "$CRASH crash markers in error.log"

if [ "$FAILURES" -eq 0 ]; then
  echo "PASS: failed reload factory smoke"
  exit 0
fi
echo "$FAILURES failure(s); error log preserved at $D/logs/error.log"
exit 1
