# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Source this file; it defines docker_pull_retry.
#
# docker_pull_retry IMAGE [--platform P]: `docker pull` of a registry image,
# tried up to three times when the registry or the network fails for a moment
# (a timeout while reaching the registry, a TLS or connection reset, a 502,
# 503 or 504, rate limiting, a cancelled request). Between the tries it
# sleeps 10 s, then 20 s (DOCKER_PULL_RETRY_SLEEP_UNIT seconds times the try
# number; tests set it to 0). Any other failure (manifest unknown, not found,
# unauthorized, denied) is not retried and fails at once. The docker output
# stays visible. After the last try the function prints an ::error:: line and
# returns 1. A digest in IMAGE is passed through untouched.
#
# Every `docker run` of a registry image in the release lane and its test
# scripts is preceded by this pull, so that a registry outage fails (or heals)
# in one clear place instead of inside `docker run`; tools/ci/
# test_docker_pull_retry.sh checks that.

# Signatures of a registry or network failure that a retry fixes.
DOCKER_PULL_TRANSIENT_RE='context deadline exceeded|TLS handshake timeout|connection reset|i/o timeout|net/http: request canceled|toomanyrequests|too many requests|received unexpected HTTP status: 50[234]|(^|[^0-9])50[234] (Bad Gateway|Service Unavailable|Gateway Time-?out)|Client\.Timeout exceeded'

docker_pull_retry() {
  local image="${1:?docker_pull_retry: image}" attempt rc out
  shift
  for attempt in 1 2 3; do
    # The output is kept to tell a transient failure from a real one, and
    # shown either way.
    if out="$(docker pull -q "$@" "$image" 2>&1)"; then
      return 0
    else
      rc=$?
    fi
    echo "$out" >&2
    if ! printf '%s\n' "$out" | grep -qiE "$DOCKER_PULL_TRANSIENT_RE"; then
      echo "::error::docker pull of ${image%%@*} failed, not a transient registry error (not retried)" >&2
      return "$rc"
    fi
    if [ "$attempt" -ge 3 ]; then
      echo "::error::docker pull of ${image%%@*} failed after $attempt attempts (registry or network)" >&2
      return 1
    fi
    echo "::warning::docker pull of ${image%%@*} failed (attempt $attempt of 3), retrying" >&2
    sleep $((attempt * ${DOCKER_PULL_RETRY_SLEEP_UNIT:-10}))
  done
}
