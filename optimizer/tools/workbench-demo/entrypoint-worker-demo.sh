#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Workbench demo worker entrypoint - delegates to canonical entrypoint.
#
# API posture contract: docker/entrypoint-worker.sh. A non-loopback bind needs
# PAGESPEED_API_ALLOW_REMOTE=true AND a token of at least 16 characters;
# tools/ci/check-entrypoint-api-posture.sh enforces that across every copy.

export DATA_DIR=/shared
export CACHE_SIZE=2147483648
export PAGESPEED_API_PORT=9880
# The management API requires a token, and this harness
# reaches it from OUTSIDE the worker container -- a deliberate non-loopback
# bind, so it names the flag and carries a credential exactly as an operator
# would have to. Fixed value ON PURPOSE: throwaway test network, and the
# harness has to be able to call the API. Read from the environment so it
# never lands in /proc/<pid>/cmdline.
export PAGESPEED_API_BIND=0.0.0.0
export PAGESPEED_API_ALLOW_REMOTE=true
export PAGESPEED_API_TOKEN="${PAGESPEED_API_TOKEN:-workbench-demo-token}"
# Reads stay open. The console's live streams are WebSockets, and the WS layer
# asks for the credential IN BAND rather than honouring the one the proxy put
# on the upgrade request -- so a browser that holds no token can never open
# one while reads are closed, and the console never reaches "connected". Open
# reads is the daemon's own documented answer for a console that must be
# usable without a login (the ASP.NET demo makes the same call). Mutating
# requests still require the token, which the proxy supplies.
export PAGESPEED_API_READ_OPEN=true
# This image still runs as root (a known limitation), where
# Chrome refuses to sandbox itself, so browser analysis would REFUSE to start
# under the `require` default. Take the documented opt-out explicitly -- the
# WARNING banner stays in the log on every Chrome spawn.
export PAGESPEED_BROWSER_SANDBOX=off
export PAGESPEED_ENABLE_BROWSER_ANALYSIS=true
export PAGESPEED_SVG_MODE=auto
export PAGESPEED_ALLOW_PRIVATE_URLS=true
export PAGESPEED_CONSOLE_DIR=/opt/console

exec /docker/entrypoint-worker.sh
