#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Start the ASan nginx rig and drive tools/stress/stress_shutdown.py against it
# with periodic reload / restart / cache-flush chaos.
#
# The reload (-s reload → new workers, old drain) and hard restart (quit → start)
# cycles exercise ngx worker teardown (ps_exit_child_process) under live load —
# the worker-vs-static-destruction window on the nginx port.
set -uo pipefail

RIG_DIR="${RIG_DIR:-$HOME/nginx-asan-rig}"
PORT="${PORT:-18081}"
DURATION="${DURATION:-600}"
CONCURRENCY="${CONCURRENCY:-16}"
REPO_DIR="${REPO_DIR:-$(pwd)}"

echo "=== starting ASan nginx rig ==="
"$RIG_DIR/run-nginx.sh" -s quit 2>/dev/null || true
sleep 1
"$RIG_DIR/run-nginx.sh"
sleep 2

code="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/stress/page1.html" || true)"
echo "health /stress/page1.html: $code"
if [[ "$code" != "200" ]]; then
  echo "ERROR: rig not healthy before stress (got $code)"; echo "--- error.log tail ---"
  tail -40 "$RIG_DIR/logs/error.log" 2>/dev/null || true
  exit 1
fi

# No-Host HTTP/1.0 probes: ps_determine_host's server-IP fallback only
# runs when the request carries no Host header, and plain HTTP/1.0 is the one
# client shape nginx accepts without one — the stress corpus never sends such a
# request, which is how a dead-buffer read in that fallback stayed invisible
# to this rig. -H 'Host:' makes
# curl omit the header entirely. A crashed worker shows up here as a non-200
# (empty reply) AND as an asan.* report for the sweep.
no_host_probe() {
  local label="$1" fails=0 page code
  for page in page1 page2 page3; do
    code="$(curl -s -o /dev/null -w '%{http_code}' --http1.0 -H 'Host:' \
      "http://127.0.0.1:$PORT/stress/$page.html" || true)"
    if [[ "$code" != "200" ]]; then
      echo "ERROR: no-Host HTTP/1.0 probe ($label) got '$code' for /stress/$page.html"
      fails=$((fails+1))
    fi
  done
  echo "no-Host HTTP/1.0 probes ($label): $((3-fails))/3 OK"
  return "$fails"
}

if ! no_host_probe pre-stress; then
  echo "--- error.log tail ---"; tail -40 "$RIG_DIR/logs/error.log" 2>/dev/null || true
  "$RIG_DIR/run-nginx.sh" -s quit 2>/dev/null || true
  exit 1
fi

echo "=== stress: duration=${DURATION}s concurrency=${CONCURRENCY} ==="
python3 "$REPO_DIR/tools/stress/stress_shutdown.py" run --server nginx \
  --base-url "http://127.0.0.1:$PORT" --url-prefix /stress \
  --cache-dir "$RIG_DIR/cache" --duration "$DURATION" --concurrency "$CONCURRENCY" \
  --reload-cmd "$RIG_DIR/nginx-reload.sh" --restart-cmd "$RIG_DIR/nginx-restart.sh" \
  --error-log "$RIG_DIR/logs/error.log" --coredump-dir "$RIG_DIR/cores"
rc=$?
echo "harness rc=$rc"

# Probe again post-stress: the reload/restart churn replaced the workers, so
# this exercises the no-Host path against workers born mid-chaos too.
if ! no_host_probe post-stress && [[ $rc -eq 0 ]]; then
  rc=1
fi

# Stop the rig; leave logs/cores for the sweep.
"$RIG_DIR/run-nginx.sh" -s quit 2>/dev/null || true
exit $rc
