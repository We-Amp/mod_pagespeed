#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Verify the Docker daemon is reachable before a job continues. Used to
# fail fast on WSL2 runners when Docker Desktop on the Windows host is
# down.
#
# Usage: tools/ci/docker-preflight.sh

set -euo pipefail

case "${1:-}" in
  -h|--help)
    sed -n '3,7p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  '')
    ;;
  *)
    echo "Usage: $0" >&2
    echo "  (takes no arguments; use -h/--help for details)" >&2
    exit 2
    ;;
esac

# Poll the daemon a few times before declaring it down. On WSL2 the Docker
# Desktop daemon can be briefly unreachable (host resuming, a service restart,
# a transient named-pipe hiccup); a single probe turns that blip into a hard
# job failure — the recurring "Docker preflight" flake that the same branch
# then re-runs green. Retry with short backoff so a momentary hiccup self-heals.
for attempt in 1 2 3 4 5; do
  if docker info >/dev/null 2>&1; then
    [ "$attempt" -gt 1 ] && echo "Docker daemon reachable after ${attempt} attempt(s)."
    exit 0
  fi
  [ "$attempt" -lt 5 ] && sleep 3
done

# The runner's host name is deliberately not printed: job logs are public.
echo "::error::Docker daemon not reachable on this runner." >&2
echo "::error::On WSL2 runners this usually means Docker Desktop on the Windows host is not running, or the docker.sock is not exposed into the distro." >&2

# Surface diagnostic detail to the job log. `docker version` (unlike
# `docker info`) returns the client side even when the daemon is down,
# which makes the gap between client and daemon obvious. Bound with
# `timeout` so a hung daemon can't wedge the runner.
{
  echo "--- docker version (diagnostic) ---"
  timeout 10 docker version 2>&1 || true
} >&2

exit 1
