#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# E2E test worker entrypoint — delegates to canonical entrypoint.
#
# API posture contract: docker/entrypoint-worker.sh. A non-loopback bind
# needs PAGESPEED_API_ALLOW_REMOTE=true AND a token of at least 16 characters;
# tools/ci/check-entrypoint-api-posture.sh enforces that across every copy.

export DATA_DIR=/shared
export PAGESPEED_API_PORT=9881
# The management API requires a token, and this harness
# reaches it from OUTSIDE the worker container -- a deliberate non-loopback
# bind, so it names the flag and carries a credential exactly as an operator
# would have to. Fixed value ON PURPOSE: throwaway test network, and the
# harness has to be able to call the API. Read from the environment so it
# never lands in /proc/<pid>/cmdline. At least 16 characters, or the daemon
# treats it as unset and then refuses the non-loopback bind.
export PAGESPEED_API_BIND=0.0.0.0
export PAGESPEED_API_ALLOW_REMOTE=true
export PAGESPEED_API_TOKEN="${PAGESPEED_API_TOKEN:-e2e-harness-test-token}"
export PAGESPEED_PROACTIVE_IMAGE_VARIANTS=true

exec /docker/entrypoint-worker.sh
