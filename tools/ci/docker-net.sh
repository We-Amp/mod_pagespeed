#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Safe docker network handling for CI jobs on hosts shared by many jobs.
#
#   docker-net.sh create <name>   reap stale ci-net-* networks, then create
#                                 <name>; fails loudly if the create fails
#   docker-net.sh reap-stale      remove leaked ci-net-* networks only
#   docker-net.sh selftest        prove the reap filters on scratch networks
#
# A network is reaped only when ALL hold: its name starts with the prefix
# (default ci-net-), it has no connected containers, and it was created more
# than CI_NET_MAX_AGE_SECS ago (default 7200), so a concurrent job's fresh,
# not-yet-attached network is never touched. Nothing else is removed and
# nothing is pruned. Set CI_NET_REUSE=1 to let create accept an existing
# network of that name.
set -u

PREFIX="${CI_NET_PREFIX:-ci-net-}"
MAX_AGE="${CI_NET_MAX_AGE_SECS:-7200}"

reap_stale() {
  local now id name n created ts age
  now=$(date +%s)
  # --filter name= is a substring match; the prefix is re-checked below.
  for id in $(docker network ls -q --filter "name=${PREFIX}" 2>/dev/null); do
    name=$(docker network inspect -f '{{.Name}}' "$id" 2>/dev/null) || continue
    case "$name" in "${PREFIX}"*) ;; *) continue ;; esac
    n=$(docker network inspect -f '{{len .Containers}}' "$id" 2>/dev/null) || continue
    [ "$n" = 0 ] || continue
    created=$(docker network inspect -f '{{.Created}}' "$id" 2>/dev/null) || continue
    # Unparsable timestamp: leave the network alone.
    ts=$(date -d "$created" +%s 2>/dev/null) || continue
    age=$((now - ts))
    [ "$age" -gt "$MAX_AGE" ] || continue
    if docker network rm "$id" >/dev/null 2>&1; then
      echo "reaped stale network ${name} (0 endpoints, age ${age}s)"
    else
      echo "::warning::could not remove stale network ${name}"
    fi
  done
  return 0
}

create() {
  local name=${1:?usage: docker-net.sh create <name>} out
  reap_stale
  if out=$(docker network create "$name" 2>&1); then
    return 0
  fi
  if [ "${CI_NET_REUSE:-0}" = 1 ] && docker network inspect "$name" >/dev/null 2>&1; then
    echo "network ${name} already exists; reusing it"
    return 0
  fi
  echo "::error::docker network create ${name} failed: ${out}. Likely cause: the docker address pools are exhausted by leaked ci-net-* networks on this host (see 'docker network ls'); stale ones are reaped automatically after ${MAX_AGE}s."
  return 1
}

selftest() {
  local p="ci-net-selftest-$$-" fail=0 img=${SELFTEST_IMAGE:-} cid=""
  local old="${p}old" busy="${p}busy" fresh="${p}fresh" other="keep-selftest-$$-old"
  PREFIX="$p"
  MAX_AGE=5
  cleanup() {
    [ -n "$cid" ] && docker rm -f "$cid" >/dev/null 2>&1
    docker network rm "$old" "$busy" "$fresh" "$other" >/dev/null 2>&1
    return 0
  }
  trap cleanup EXIT
  for nw in "$old" "$busy" "$other"; do
    docker network create --label ci-net-selftest=1 "$nw" >/dev/null || return 1
  done
  if [ -n "$img" ]; then
    cid=$(docker run -d --network "$busy" "$img" sleep 120) || return 1
  fi
  sleep 7
  docker network create --label ci-net-selftest=1 "$fresh" >/dev/null || return 1
  reap_stale
  has() { docker network inspect "$1" >/dev/null 2>&1; }
  has "$old" && { echo "selftest FAIL: old empty network survived"; fail=1; }
  has "$fresh" || { echo "selftest FAIL: fresh network was reaped"; fail=1; }
  has "$other" || { echo "selftest FAIL: non-matching name was reaped"; fail=1; }
  if [ -n "$img" ]; then
    has "$busy" || { echo "selftest FAIL: network with an endpoint was reaped"; fail=1; }
  fi
  [ "$fail" = 0 ] && echo "selftest ok"
  return "$fail"
}

case "${1:-}" in
  create) shift; create "$@" ;;
  reap-stale) reap_stale ;;
  selftest) selftest ;;
  *) echo "usage: docker-net.sh create <name> | reap-stale | selftest" >&2; exit 2 ;;
esac
