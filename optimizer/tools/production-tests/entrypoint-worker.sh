#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Production test worker entrypoint — delegates to canonical entrypoint.
#
# API posture contract: docker/entrypoint-worker.sh. A non-loopback bind needs
# PAGESPEED_API_ALLOW_REMOTE=true AND a token of at least 16 characters;
# tools/ci/check-entrypoint-api-posture.sh enforces that across every copy.

export DATA_DIR=/shared
export CACHE_SIZE="${PAGESPEED_CACHE_SIZE:-1073741824}"
export PAGESPEED_PROACTIVE_IMAGE_VARIANTS=true

exec /docker/entrypoint-worker.sh
