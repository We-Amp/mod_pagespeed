#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Production entrypoint for the mod_pagespeed nginx image.
#
# Performs environment variable substitution on the nginx config
# template, then starts nginx.

set -euo pipefail

# If a template exists and the config file is writable, render it with envsubst.
# When nginx.conf is volume-mounted as :ro, skip template rendering.
if [ -f /etc/nginx/nginx.conf.template ] && [ -w /etc/nginx/nginx.conf ]; then
    # Only substitute the backend address (preserve $request_uri etc.).
    # Everything else nginx needs (worker socket, HTML toggle) is auto-discovered
    # from the worker's shared config (rendering contract); the template
    # carries no other variables.
    envsubst '${BACKEND_HOST} ${BACKEND_PORT}' \
        < /etc/nginx/nginx.conf.template \
        > /etc/nginx/nginx.conf
fi

# Self-heal nginx cache ownership before dropping privileges. The proxy cache
# (e.g. the PSI cache backing /analyze) lives on a persisted volume mounted at
# /var/cache/nginx. Two situations leave it unwritable by the worker, which
# runs as the `nginx` user via `user nginx;`:
#   * the volume is created fresh and owned by root, or
#   * a rebuild changes the image's `nginx` UID, orphaning subdirs created by
#     the old UID (mode 0700) — the exact cause of the 2026-06-12 /analyze
#     outage, where UID drifted 101 -> 999 and cache writes failed with
#     `open() ... (13: Permission denied)` -> 500 -> 503.
# This entrypoint runs as root, so chown to the worker user by NAME (resolves
# to whatever UID this image assigns) on every start. The cache is small and
# bounded, so the recursive chown is cheap.
if [ -d /var/cache/nginx ]; then
    chown -R nginx:nginx /var/cache/nginx || true
fi

exec "$@"
