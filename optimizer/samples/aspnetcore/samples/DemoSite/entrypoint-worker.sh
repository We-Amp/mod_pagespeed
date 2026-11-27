#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Entrypoint for the factory worker service (ASP.NET Core demo).
# Sets up shared directory permissions and starts the worker.
#
# API posture contract: docker/entrypoint-worker.sh. This copy builds its own
# argv rather than delegating, so the contract is restated by hand: the
# non-loopback --api-bind below is paired with --api-allow-remote AND a token
# of at least 16 characters, which is the pairing the daemon refuses to start
# without. tools/ci/check-entrypoint-api-posture.sh enforces it across every copy.

set -euo pipefail

# Ensure /shared exists and is writable by both ASP.NET and worker.
chmod 777 /shared

# Set umask so files and sockets created by the worker are
# world-readable/writable. The ASP.NET process needs access to:
#   - cache.vol (Cyclone mmap'd cache, opened O_RDWR)
#   - pagespeed.sock (Unix socket, connect requires write permission)
umask 0000

# Pre-create cache file so chmod takes effect before Cyclone opens it.
touch /shared/cache.vol
chmod 666 /shared/cache.vol

# The demo publishes its management API to the sibling
# ASP.NET container, which is a deliberate non-loopback bind -- so it names
# the flag and carries a token, exactly as an operator would have to. Fixed
# value ON PURPOSE (throwaway demo network), read from the environment so it
# never reaches /proc/<pid>/cmdline.
export PAGESPEED_API_TOKEN="${PAGESPEED_API_TOKEN:-aspnetcore-demo-token}"

# This container runs as root, where Chrome refuses to
# sandbox itself, so the `require` default would make browser analysis refuse
# to start. Take the documented opt-out explicitly.
export PAGESPEED_BROWSER_SANDBOX=off

exec factory_worker \
  --socket /shared/pagespeed.sock \
  --cache-path /shared/cache.vol \
  --cache-size 536870912 \
  --api-port 9880 \
  --api-bind 0.0.0.0 \
  --api-allow-remote \
  --api-read-open \
  --console-dir /opt/console \
  --allow-private-urls \
  --enable-browser-analysis
