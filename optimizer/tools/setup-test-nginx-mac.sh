#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

set -euo pipefail

# Install dependencies
brew install perl cpanminus nginx 2>/dev/null || true
cpanm --notest Test::Nginx IPC::Run 2>/dev/null || true

# Get matching nginx source
NGINX_VERSION=$(nginx -v 2>&1 | sed 's/.*nginx\///')
if ! [[ "$NGINX_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Error: unexpected nginx version format: $NGINX_VERSION"
  exit 1
fi
echo "System nginx: $NGINX_VERSION"

NGINX_SRC="/tmp/nginx-$NGINX_VERSION"
if [ ! -d "$NGINX_SRC" ]; then
  curl -sL "https://nginx.org/download/nginx-$NGINX_VERSION.tar.gz" | tar xz -C /tmp
  (cd "$NGINX_SRC" && ./configure --with-compat)
fi

# Build module
echo "Building module against $NGINX_SRC..."
NGINX_PATH="$NGINX_SRC" bazel build //src/nginx:ngx_pagespeed_module.so

echo ""
echo "Setup complete. Run tests with:"
echo "  ./tools/run-test-nginx.sh"
