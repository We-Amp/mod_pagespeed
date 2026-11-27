#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Per-runner host ports for the docker compose stacks the pipelines bring up.
# Several runner slots can share one host, and every stack publishes fixed
# host ports, so two jobs on the same host would collide. Source this file
# (do not execute it) and every published host port moves by a per-runner
# offset:
#
#   offset = (last two trailing digits of RUNNER_NAME, modulo 20) * 1000
#
# "fleet-heavy-07" gives 7000; a name without trailing digits, an unset
# RUNNER_NAME, or RUNNER_ENVIRONMENT=github-hosted gives 0, which leaves every
# port at its default. Only the last two digits are read, so a long digit run
# cannot overflow the arithmetic. The slot number is taken modulo 20, so the
# highest port is 19 * 1000 + 9882 = 28882, below the Linux ephemeral range
# (32768 and up). A step of 1000 keeps every stack clear of every other
# stack's defaults on any slot (the defaults span less than 1000 ports within
# each group).
#
# Exported: one variable per published host port (default + offset), plus
# RUNNER_PORT_VARS, the space-separated list of those names. A workflow step
# persists them for later steps with:
#
#   . tools/ci/runner-ports.sh
#   for v in $RUNNER_PORT_VARS; do eval "p=\${$v}"; echo "$v=$p"; done >> "$GITHUB_ENV"
#
# POSIX sh; ASCII only.

_rp_digits=$(printf '%s' "${RUNNER_NAME:-}" | sed 's/.*[^0-9]//; s/.*\(..\)$/\1/; s/^0*//')
if [ -z "$_rp_digits" ] || [ "${RUNNER_ENVIRONMENT:-}" = "github-hosted" ]; then
  _rp_n=0
else
  _rp_n=$((_rp_digits % 20))
fi
RUNNER_PORT_OFFSET=$((_rp_n * 1000))
export RUNNER_PORT_OFFSET

RUNNER_PORT_VARS=""
_rp_set() {
  # _rp_set NAME DEFAULT
  _rp_val=$(($2 + RUNNER_PORT_OFFSET))
  eval "$1=$_rp_val"
  export "$1"
  RUNNER_PORT_VARS="${RUNNER_PORT_VARS:+$RUNNER_PORT_VARS }$1"
}

# tools/e2e
_rp_set E2E_ORIGIN_PORT 8082
_rp_set E2E_WORKER_PORT 9881
_rp_set E2E_NGINX_PORT 8083
# tools/realworld-tests
_rp_set REALWORLD_WORKER_PORT 9882
_rp_set REALWORLD_NGINX_PORT 8083
# tools/stress
_rp_set STRESS_ORIGIN_PORT 8191
_rp_set STRESS_NGINX_PORT 8190
# tools/production-tests
_rp_set PRODTEST_ORIGIN_PORT 8201
_rp_set PRODTEST_NGINX_PORT 8200
# tools/http-compliance
_rp_set COMPLIANCE_ORIGIN_PORT 8181
_rp_set COMPLIANCE_NGINX_PORT 8180
_rp_set COMPLIANCE_AGGRESSIVE_PORT 8183
# tools/async-css-probe
_rp_set PROBE_ORIGIN_PORT 8281
_rp_set PROBE_NGINX_PORT 8280
# tools/workbench-demo
_rp_set WORKBENCH_ORIGIN_PORT 8081
_rp_set WORKBENCH_API_PORT 9880
_rp_set WORKBENCH_NGINX_PORT 8084
export RUNNER_PORT_VARS

echo "runner-ports: RUNNER_NAME='${RUNNER_NAME:-}' offset=${RUNNER_PORT_OFFSET}"
for _rp_v in $RUNNER_PORT_VARS; do
  eval "_rp_x=\${$_rp_v}"
  echo "  $_rp_v=$_rp_x"
done
unset _rp_digits _rp_n _rp_val _rp_v _rp_x
