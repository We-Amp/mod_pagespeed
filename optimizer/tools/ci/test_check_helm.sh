#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test for the Compose/appVersion lockstep in tools/ci/check_helm.sh.
# Runs check_helm_compose_lockstep over scratch copies of Chart.yaml,
# docker-compose.yml and .env.example written per case, so no repository file
# is read or touched, except for the first case, which runs it over the real
# deploy/ files. bash 3.2 and POSIX grep/sed/awk only; exits non-zero when any
# case does not behave as pinned.
# shellcheck disable=SC2016  # the ${PAGESPEED_VERSION:-X} strings are compose text, meant literally
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# shellcheck source=check_helm.sh
. "$HERE/check_helm.sh"

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0
n=0

report() { # report WANT GOT NAME OUTPUT
  if [ "$2" = "$1" ]; then
    echo "ok   $3"
  else
    echo "FAIL $3 (wanted $1, got $2)"
    printf '%s\n' "$4" | sed 's/^/     | /'
    fail=1
  fi
}

# write_case DIR APPVERSION_LINE WORKER_LINE NGINX_LINE WORKER_EXTRA ENV_BODY
# WORKER_EXTRA goes on the line above the worker's image: line (a commented-out
# image line, say); ENV_BODY is the whole env example. The compose header
# repeats the real file's `docker tag` examples, which name both images.
write_case() {
  local dir="$1"
  mkdir -p "$dir"
  printf 'apiVersion: v2\nname: pagespeed\nversion: 0.3.8\n%s\n' "$2" > "$dir/Chart.yaml"
  {
    printf '# Pulls the published images. To run images you built yourself:\n'
    printf '#   docker tag modpagespeed/worker:VERSION ghcr.io/we-amp/pagespeed-worker:VERSION\n'
    printf '#   docker tag modpagespeed/nginx:VERSION  ghcr.io/we-amp/pagespeed-nginx:VERSION\n'
    printf 'services:\n  worker:\n'
    if [ -n "$5" ]; then printf '%s\n' "$5"; fi
    printf '%s\n' "$3"
    printf '  nginx:\n'
    printf '%s\n' "$4"
  } > "$dir/docker-compose.yml"
  printf '%s\n' "$6" > "$dir/.env.example"
}

run_case() { # run_case ok|no NAME APPVERSION_LINE WORKER_LINE NGINX_LINE WORKER_EXTRA ENV_BODY
  local want="$1" name="$2" dir out got
  shift 2
  n=$((n + 1))
  dir="$tmp/case$n"
  write_case "$dir" "$@"
  if out=$(check_helm_compose_lockstep "$dir/Chart.yaml" "$dir/docker-compose.yml" "$dir/.env.example" 2>&1); then
    got=ok
  else
    got=no
  fi
  report "$want" "$got" "$name" "$out"
}

# --- the repository's own files -------------------------------------------
if out=$(check_helm_compose_lockstep "$ROOT/deploy/helm/pagespeed/Chart.yaml" \
  "$ROOT/deploy/docker-compose.yml" "$ROOT/deploy/.env.example" 2>&1); then
  got=ok
else
  got=no
fi
report ok "$got" "the repository's deploy/ files (header comment names the images)" "$out"

W='    image: ghcr.io/we-amp/pagespeed-worker:${PAGESPEED_VERSION:-2.4.0}'
N='    image: ghcr.io/we-amp/pagespeed-nginx:${PAGESPEED_VERSION:-2.4.0}'
W5='    image: ghcr.io/we-amp/pagespeed-worker:${PAGESPEED_VERSION:-2.5.0}'
N5='    image: ghcr.io/we-amp/pagespeed-nginx:${PAGESPEED_VERSION:-2.5.0}'
E='# PAGESPEED_VERSION=2.4.0'
E5='# PAGESPEED_VERSION=2.5.0'
A='appVersion: "2.4.0"'

# --- appVersion spellings ---------------------------------------------------
run_case ok "appVersion double-quoted"                     "$A"                              "$W" "$N" '' "$E"
run_case ok "appVersion bare"                              'appVersion: 2.4.0'               "$W" "$N" '' "$E"
run_case ok "appVersion single-quoted"                     "appVersion: '2.4.0'"             "$W" "$N" '' "$E"
run_case ok "appVersion double-quoted, trailing comment"   'appVersion: "2.4.0"   # product' "$W" "$N" '' "$E"
run_case ok "appVersion bare, trailing comment"            'appVersion: 2.4.0 # product'     "$W" "$N" '' "$E"
run_case ok "appVersion single-quoted, trailing comment"   "appVersion: '2.4.0' # product"   "$W" "$N" '' "$E"
run_case no "appVersion empty, quoted"                     'appVersion: ""'                  "$W" "$N" '' "$E"
run_case no "appVersion empty, bare"                       'appVersion:'                     "$W" "$N" '' "$E"
run_case no "appVersion missing"                           '# appVersion: "2.4.0"'           "$W" "$N" '' "$E"

# --- the bump ---------------------------------------------------------------
run_case no "appVersion bumped, compose and env not"       'appVersion: "2.5.0"' "$W"  "$N"  '' "$E"
run_case ok "appVersion, compose and env all bumped"       'appVersion: "2.5.0"' "$W5" "$N5" '' "$E5"
run_case no "compose and env bumped, appVersion not"       "$A"                  "$W5" "$N5" '' "$E5"

# --- compose image lines ----------------------------------------------------
run_case no "compose defaults to latest" "$A" \
  '    image: ghcr.io/we-amp/pagespeed-worker:${PAGESPEED_VERSION:-latest}' \
  '    image: ghcr.io/we-amp/pagespeed-nginx:${PAGESPEED_VERSION:-latest}' '' "$E"
run_case no "one image a stale literal, the other right" "$A" "$W" \
  '    image: ghcr.io/we-amp/pagespeed-nginx:2.3.0' '' "$E"
run_case no "one image a literal at the current version (no PAGESPEED_VERSION default)" "$A" "$W" \
  '    image: ghcr.io/we-amp/pagespeed-nginx:2.4.0' '' "$E"
run_case no "false green: commented-out line at the new tag above a stale worker literal" 'appVersion: "2.5.0"' \
  '    image: ghcr.io/we-amp/pagespeed-worker:2.4.0' "$N5" \
  '    # image: ghcr.io/we-amp/pagespeed-worker:${PAGESPEED_VERSION:-2.5.0}' "$E5"
run_case no "false green: commented-out worker line at the new tag, no uncommented one" 'appVersion: "2.5.0"' \
  '    build: .' "$N5" \
  '    # image: ghcr.io/we-amp/pagespeed-worker:${PAGESPEED_VERSION:-2.5.0}' "$E5"
run_case ok "commented-out image line at a stale tag is read past" "$A" "$W" "$N" \
  '    # image: ghcr.io/we-amp/pagespeed-worker:${PAGESPEED_VERSION:-9.9.9}' "$E"
run_case ok "commented-out image line at latest is read past" "$A" "$W" "$N" \
  '    # image: ghcr.io/we-amp/pagespeed-worker:latest' "$E"
run_case no "two uncommented worker image lines" "$A" "$W"$'\n'"$W" "$N" '' "$E"
run_case no "worker image line missing" "$A" '    build: .' "$N" '' "$E"
run_case ok "extra spaces after image:" "$A" \
  '    image:   ghcr.io/we-amp/pagespeed-worker:${PAGESPEED_VERSION:-2.4.0}' "$N" '' "$E"
run_case no "trailing junk after the default" "$A" \
  '    image: ghcr.io/we-amp/pagespeed-worker:${PAGESPEED_VERSION:-2.4.0}-rc1' "$N" '' "$E"

# --- the env example --------------------------------------------------------
run_case ok "env line uncommented"                       "$A" "$W" "$N" '' 'PAGESPEED_VERSION=2.4.0'
run_case ok "env value double-quoted"                    "$A" "$W" "$N" '' '# PAGESPEED_VERSION="2.4.0"'
run_case ok "env value single-quoted"                    "$A" "$W" "$N" '' "# PAGESPEED_VERSION='2.4.0'"
run_case ok "env value with trailing comment"            "$A" "$W" "$N" '' '# PAGESPEED_VERSION=2.4.0 # the current release'
run_case ok "env prose naming the variable is not a value line" "$A" "$W" "$N" '' \
  '# set PAGESPEED_VERSION=2.3.0 to pin an older release'$'\n'"$E"
run_case no "env stale"                                  "$A" "$W" "$N" '' '# PAGESPEED_VERSION=2.3.0'
run_case no "env latest"                                 "$A" "$W" "$N" '' '# PAGESPEED_VERSION=latest'
run_case no "env empty value"                            "$A" "$W" "$N" '' '# PAGESPEED_VERSION='
run_case no "env line missing"                           "$A" "$W" "$N" '' '# BACKEND_PORT=8081'
run_case no "second env line stale (every line must agree)" "$A" "$W" "$N" '' "$E"$'\n''PAGESPEED_VERSION=2.3.0'
run_case no "first env line stale, second right"         "$A" "$W" "$N" '' '# PAGESPEED_VERSION=2.3.0'$'\n'"$E"

echo ""
if [ "$fail" = 0 ]; then
  echo "check_helm self-test: $n scratch cases and the repository files behaved as pinned."
else
  echo "check_helm self-test FAILED"
fi
exit "$fail"
