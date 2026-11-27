#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# CI check: Validate the Helm chart, and keep the Compose recipe's default
# image tag in lockstep with the chart's appVersion.
# Requires: helm CLI installed (the Compose lockstep check itself needs none).
# Library: source this file and call check_helm_compose_lockstep; the
# self-test tools/ci/test_check_helm.sh does, over scratch copies.

# check_helm_chart_app_version CHART_YAML -> prints appVersion; 1 if unreadable.
# The value may be bare, single-quoted or double-quoted (all valid YAML), with
# or without a trailing comment.
check_helm_chart_app_version() {
  local v
  v=$(grep -E '^appVersion:' "$1" 2>/dev/null | head -1 \
    | sed -E "s/^appVersion:[[:space:]]*//; s/[[:space:]]+#.*$//; s/[[:space:]]+$//; s/^\"(.*)\"$/\1/; s/^'(.*)'$/\1/")
  [ -n "$v" ] || return 1
  printf '%s\n' "$v"
}

# check_helm_compose_lockstep CHART_YAML COMPOSE_YML ENV_EXAMPLE
#   -> 0 in lockstep, 1 otherwise; every finding is an ERROR: line on stdout.
#
# The Compose recipe pins the same images as the chart, and its default tag
# must be the chart's appVersion: the worker and nginx images publish version
# tags only, so a default of `latest` (or a stale version) fails to pull.
# Each image needs exactly one uncommented `image:` line, in the
# `${PAGESPEED_VERSION:-<appVersion>}` form; a commented-out image line, or
# the header comment's `docker tag` examples, name the images too and are
# read past, so they can neither satisfy nor fail the check. Every
# PAGESPEED_VERSION line of the env example must agree, not just the first,
# and its value may be quoted either way.
check_helm_compose_lockstep() {
  local chart="$1" compose="$2" env_example="$3"
  local app_version image lines count tag env_values v bad=0
  app_version=$(check_helm_chart_app_version "$chart") || {
    echo "ERROR: cannot read appVersion from $chart"
    return 1
  }
  for image in worker nginx; do
    lines=$(grep -E "^[[:space:]]*image:[[:space:]]*ghcr\.io/we-amp/pagespeed-${image}:" "$compose" 2>/dev/null || true)
    count=$(printf '%s\n' "$lines" | awk 'NF { n++ } END { print n + 0 }')
    tag=$(printf '%s\n' "$lines" \
      | sed -n -E 's/^[[:space:]]*image:[[:space:]]*ghcr\.io\/we-amp\/pagespeed-[a-z]+:\$\{PAGESPEED_VERSION:-([^}]*)\}[[:space:]]*$/\1/p')
    if [ "$count" != 1 ] || [ -z "$tag" ]; then
      echo "ERROR: $compose must carry exactly one uncommented 'image: ghcr.io/we-amp/pagespeed-${image}:\${PAGESPEED_VERSION:-$app_version}' line (found $count uncommented image line(s) for pagespeed-${image})"
      bad=1
    elif [ "$tag" != "$app_version" ]; then
      echo "ERROR: $compose defaults pagespeed-${image} to $tag; the chart appVersion is $app_version"
      bad=1
    fi
  done
  env_values=$(grep -E '^#?[[:space:]]*PAGESPEED_VERSION=' "$env_example" 2>/dev/null \
    | sed -E "s/^[^=]*=//; s/[[:space:]]+#.*$//; s/[[:space:]]+$//; s/^\"(.*)\"$/\1/; s/^'(.*)'$/\1/" || true)
  if [ -z "$env_values" ]; then
    echo "ERROR: $env_example has no PAGESPEED_VERSION= line (expected PAGESPEED_VERSION=$app_version, commented out or not)"
    bad=1
  else
    while IFS= read -r v; do
      if [ "$v" != "$app_version" ]; then
        echo "ERROR: $env_example sets PAGESPEED_VERSION=${v:-<empty>}; the chart appVersion is $app_version"
        bad=1
      fi
    done <<EOF
$env_values
EOF
  fi
  if [ "$bad" = 0 ]; then
    echo "OK: Compose defaults to $app_version (= appVersion) in $compose and $env_example"
  fi
  return "$bad"
}

# Sourced as a library: stop here, leaving the caller's shell options alone.
if [ "${BASH_SOURCE[0]}" != "$0" ]; then
  return 0
fi

set -euo pipefail

CHART_DIR="deploy/helm/pagespeed"

if [ ! -d "$CHART_DIR" ]; then
  echo "ERROR: $CHART_DIR not found"
  exit 1
fi

echo "=== Compose default image tag tracks appVersion ==="
check_helm_compose_lockstep "$CHART_DIR/Chart.yaml" deploy/docker-compose.yml deploy/.env.example
app_version=$(check_helm_chart_app_version "$CHART_DIR/Chart.yaml")
echo ""

if ! command -v helm &>/dev/null; then
  # Locally a missing helm is a skip; in CI (CHECK_HELM_REQUIRED=1) it is a
  # failure, so the gate cannot pass without having run.
  if [ "${CHECK_HELM_REQUIRED:-0}" = "1" ]; then
    echo "ERROR: helm not installed and CHECK_HELM_REQUIRED=1"
    exit 1
  fi
  echo "SKIP: helm not installed"
  exit 0
fi

echo "=== Helm lint ==="
helm lint "$CHART_DIR"

echo ""
echo "=== Helm template (default values) ==="
helm template pagespeed-test "$CHART_DIR" > /dev/null
echo "OK: default values template renders"

echo ""
echo "=== Helm template (custom values) ==="
helm template pagespeed-test "$CHART_DIR" \
  --set worker.cacheSize=1073741824 \
  --set ingress.enabled=true \
  --set ingress.hosts[0].host=example.com \
  --set ingress.hosts[0].paths[0].path=/ \
  --set ingress.hosts[0].paths[0].pathType=Prefix \
  > /dev/null
echo "OK: custom values template renders"

echo ""
echo "=== Helm lockstep: worker and nginx share ONE image tag ==="
rendered=$(helm template pagespeed-test "$CHART_DIR")
worker_tag=$(echo "$rendered" | grep 'pagespeed-worker:' | head -1 | sed -E 's/.*pagespeed-worker:([^"[:space:]]+).*/\1/')
nginx_tag=$(echo "$rendered" | grep 'pagespeed-nginx:' | head -1 | sed -E 's/.*pagespeed-nginx:([^"[:space:]]+).*/\1/')
echo "  worker=$worker_tag nginx=$nginx_tag appVersion=$app_version"
if [ -z "$worker_tag" ] || [ "$worker_tag" != "$nginx_tag" ]; then
  echo "ERROR: worker ($worker_tag) and nginx ($nginx_tag) image tags differ — lockstep broken"
  exit 1
fi
if [ "$worker_tag" != "$app_version" ]; then
  echo "ERROR: default image tag ($worker_tag) does not track appVersion ($app_version)"
  exit 1
fi
echo "OK: both images pinned to $worker_tag (= appVersion)"

echo ""
echo "=== Helm guard: legacy per-image tag is rejected (no silent split) ==="
if helm template pagespeed-test "$CHART_DIR" --set worker.image.tag=9.9.9 >/dev/null 2>&1; then
  echo "ERROR: --set worker.image.tag should FAIL (it would allow a worker/nginx split) but rendered"
  exit 1
fi
echo "OK: worker.image.tag override is rejected loud"

echo ""
echo "=== Helm guard: the pod shares one PID namespace ==="
# The pod keeps every cache writer in one PID namespace (as the compose files
# do) with the pause container as PID 1; see the comment in deployment.yaml.
if ! helm template pagespeed-test "$CHART_DIR" | grep -qE '^ +shareProcessNamespace: true$'; then
  echo "ERROR: the rendered pod does not set shareProcessNamespace: true"
  exit 1
fi
echo "OK: shareProcessNamespace: true"

echo ""
echo "All Helm checks passed."
