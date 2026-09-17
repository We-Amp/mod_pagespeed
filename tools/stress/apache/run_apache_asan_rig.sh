#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Drive tools/stress/stress_shutdown.py against the ASan Apache rig with periodic
# graceful reload / hard restart / cache-flush chaos.
#
# The `apache2 -k graceful` reload and the stop/start restart exercise child
# teardown under load — the worker-vs-static-destruction window and the
# Cyclone graceful-restart thread-leak path. Runs the harness under sudo so it can
# read the root-owned error.log and drive the restart helper.
set -uo pipefail

RIG_DIR="${RIG_DIR:-${TMPDIR:-/tmp}/apache-asan-rig}"
PORT="${PORT:-18085}"
DURATION="${DURATION:-600}"
CONCURRENCY="${CONCURRENCY:-16}"
REPO_DIR="${REPO_DIR:-$(pwd)}"
ERRLOG="${ERRLOG:-/var/log/apache2/error.log}"
CACHE_DIR="${CACHE_DIR:-/var/cache/mod_pagespeed}"

mkdir -p "$RIG_DIR/cores"
code="$(curl -s -o /dev/null -w '%{http_code}' "http://localhost:$PORT/stress/page1.html" || true)"
echo "health /stress/page1.html: $code"
if [[ "$code" != "200" ]]; then
  echo "ERROR: rig not healthy before stress (got $code)"; sudo tail -40 "$ERRLOG" 2>/dev/null || true; exit 1
fi

echo "=== stress: duration=${DURATION}s concurrency=${CONCURRENCY} ==="
sudo python3 -u "$REPO_DIR/tools/stress/stress_shutdown.py" run --server apache \
  --base-url "http://localhost:$PORT" --url-prefix /stress \
  --cache-dir "$CACHE_DIR" --duration "$DURATION" --concurrency "$CONCURRENCY" \
  --reload-cmd "$RIG_DIR/apache-reload.sh" --restart-cmd "$RIG_DIR/apache-restart.sh" \
  --error-log "$ERRLOG" --coredump-dir "$RIG_DIR/cores"
rc=$?
echo "harness rc=$rc"
exit $rc
