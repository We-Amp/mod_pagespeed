#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Entrypoint for the stress test worker service.
# Translates environment variables to CLI flags for low-limit testing.
#
# No management API is enabled here (no --api-port/--api-socket), so the
# docker/entrypoint-worker.sh bind/token contract does not apply. Enabling one
# means adopting that contract -- see tools/ci/check-entrypoint-api-posture.sh.

set -euo pipefail

# Ensure /shared exists and is writable by both nginx and worker
chmod 777 /shared
umask 0000

# Pre-create cache file
touch /shared/cache.vol
chmod 666 /shared/cache.vol

exec factory_worker \
  --socket /shared/pagespeed.sock \
  --cache-path /shared/cache.vol \
  --cache-size "${WORKER_CACHE_SIZE:-52428800}" \
  --read-lease-duration "${WORKER_READ_LEASE_DURATION:-5000}" \
  --max-connections "${WORKER_MAX_CONNECTIONS:-16}" \
  --max-buffer-size "${WORKER_MAX_BUFFER_SIZE:-1048576}" \
  --connection-timeout "${WORKER_CONNECTION_TIMEOUT:-5000}" \
  --shutdown-timeout "${WORKER_SHUTDOWN_TIMEOUT:-3000}" \
  --log-level "${WORKER_LOG_LEVEL:-warning}" \
  --proactive-image-variants
