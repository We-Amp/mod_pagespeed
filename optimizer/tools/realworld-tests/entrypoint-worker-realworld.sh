#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Entrypoint for the factory worker service (realworld tests).
# Same pattern as tools/workbench-demo/entrypoint-worker-demo.sh
# but with API on port 9882 and no console directory.
#
# API posture contract: docker/entrypoint-worker.sh. This copy builds its own
# argv rather than delegating, so the contract is restated by hand: the
# non-loopback --api-bind below is paired with --api-allow-remote AND a token
# of at least 16 characters, which is the pairing the daemon refuses to start
# without. tools/ci/check-entrypoint-api-posture.sh enforces it across every copy.

set -euo pipefail

# Ensure /shared exists and is writable by both nginx and worker
chmod 777 /shared

# Set umask so files and sockets created by the worker are
# world-readable/writable.  The nginx worker process runs as
# "nobody" and needs write access to:
#   - cache.vol (Cyclone mmap'd cache, opened O_RDWR)
#   - pagespeed.sock (Unix socket, connect requires write permission)
umask 0000

# Pre-create cache file so chmod takes effect before Cyclone opens it.
touch /shared/cache.vol
chmod 666 /shared/cache.vol

# The API is published to the sibling test containers on this
# compose network, which is a deliberate non-loopback bind -- so it names the
# flag and carries a token, exactly as an operator would have to.  The token is
# fixed here ON PURPOSE: this is a throwaway test network, and the harness has
# to be able to call the API.  The daemon reads it from the environment so it
# stays out of /proc/<pid>/cmdline.
export PAGESPEED_API_TOKEN="${PAGESPEED_API_TOKEN:-realworld-test-token}"

exec factory_worker \
  --socket /shared/pagespeed.sock \
  --cache-path /shared/cache.vol \
  --cache-size 2147483648 \
  --api-port 9882 \
  --api-bind 0.0.0.0 \
  --api-allow-remote \
  --allow-private-urls \
  --svg-mode auto \
  --console-dir /opt/console
