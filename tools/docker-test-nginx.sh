#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run test-nginx integration tests inside the pagespeed2-dev Docker container.
# Installs nginx + Test::Nginx, builds the module, and runs prove.
#
# Usage (from host):
#   docker run --rm \
#     -v "$PWD:/workspace" -v "pagespeed2-bazel-cache:/root/.cache/bazel" \
#     -w /workspace pagespeed2-dev bash tools/docker-test-nginx.sh
# To run a single test file, pass it as an argument:
#   ... pagespeed2-dev bash tools/docker-test-nginx.sh t/200-miss-path.t
#
# Prebuilt mode (CI): set PREBUILT_DIR to a Linux Build bundle directory
# (ngx_pagespeed_module.so, seed_test_cache, webbotauth_sign_tool,
# nginx-version). The script then skips Steps 2-4, uses those binaries and
# keeps the image's own nginx, which must be the version the bundle's module
# was built against.
set -euxo pipefail

PREBUILT_DIR="${PREBUILT_DIR:-}"

echo "=== Step 1: Installing nginx + Test::Nginx ==="
# Test::Nginx's dependencies come from the distribution, so cpanm fetches
# only Test::Nginx itself (a CPAN libwww-perl pulls XS modules that need a
# C compiler).
TEST_NGINX_DEPS="perl cpanminus libipc-run-perl libwww-perl libtest-base-perl libtext-diff-perl libtest-longstring-perl"
apt-get update -qq
if [ -n "$PREBUILT_DIR" ]; then
  # Keep the image's nginx: installing the package again could move it to a
  # newer release than the one the bundle's module was built against.
  apt-get install -y -qq $TEST_NGINX_DEPS 2>&1 | tail -3
else
  apt-get install -y -qq nginx $TEST_NGINX_DEPS libpcre3-dev zlib1g-dev 2>&1 | tail -3
fi
# Pinned, so a new CPAN release cannot change the harness under a run.
cpanm --notest Test::Nginx@0.32 2>&1 | tail -3
perl -MTest::Nginx::Socket -e 'print "Test::Nginx: OK\n"'

if [ -n "$PREBUILT_DIR" ]; then
  echo "=== Steps 2-4: using the prebuilt bundle in $PREBUILT_DIR ==="
  NGINX_VERSION=$(nginx -v 2>&1 | grep -oP 'nginx/\K[0-9]+\.[0-9]+\.[0-9]+')
  BUNDLE_NGINX_VERSION=$(tr -d '[:space:]' < "$PREBUILT_DIR/nginx-version")
  if [ "$NGINX_VERSION" != "$BUNDLE_NGINX_VERSION" ]; then
    echo "Error: nginx is $NGINX_VERSION but the bundle's module was built against $BUNDLE_NGINX_VERSION" >&2
    exit 1
  fi
  MODULE_SO="$PREBUILT_DIR/ngx_pagespeed_module.so"
  SEED_TOOL="$PREBUILT_DIR/seed_test_cache"
  WEBBOTAUTH_SIGN_TOOL="$PREBUILT_DIR/webbotauth_sign_tool"
  # t/103 and t/104 self-skip without the sign tool; in prebuilt mode a
  # missing tool is an error, not a silent skip.
  for f in "$MODULE_SO" "$SEED_TOOL" "$WEBBOTAUTH_SIGN_TOOL"; do
    [ -f "$f" ] || { echo "Error: $f is missing from the bundle" >&2; exit 1; }
  done
  chmod +x "$SEED_TOOL" "$WEBBOTAUTH_SIGN_TOOL" 2>/dev/null || true
  cd /workspace
else
  echo "=== Step 2: Downloading matching nginx source ==="
  NGINX_VERSION=$(nginx -v 2>&1 | grep -oP 'nginx/\K[0-9]+\.[0-9]+\.[0-9]+')
  echo "nginx version: $NGINX_VERSION"
  curl -L "https://nginx.org/download/nginx-$NGINX_VERSION.tar.gz" -o /tmp/nginx.tar.gz
  cd /tmp && tar xzf nginx.tar.gz
  cd "/tmp/nginx-$NGINX_VERSION"
  ./configure --with-compat 2>&1 | tail -3
  echo "nginx source configured"

  echo "=== Step 3: Building module ==="
  cd /workspace
  NGINX_PATH="/tmp/nginx-$NGINX_VERSION" bazel build --config=docker --symlink_prefix=/dev/null/ //src/nginx:ngx_pagespeed_module.so 2>&1 | tail -5

  BAZEL_EXECROOT="$(bazel info --config=docker execution_root 2>/dev/null)"
  MODULE_SO="$BAZEL_EXECROOT/$(bazel cquery --config=docker --output=files //src/nginx:ngx_pagespeed_module.so 2>/dev/null)"
  echo "Module at: $MODULE_SO"

  echo "=== Step 4: Building seed_test_cache + webbotauth_sign_tool ==="
  bazel build --config=docker --symlink_prefix=/dev/null/ //tools:seed_test_cache //src/crypto/webbotauth:webbotauth_sign_tool 2>&1 | tail -3
  SEED_TOOL="$BAZEL_EXECROOT/$(bazel cquery --config=docker --output=files //tools:seed_test_cache 2>/dev/null)"
  echo "Seed tool at: $SEED_TOOL"
  # t/103-webbotauth-valid.t mints fresh RFC 9421 signatures with this tool
  # (the suite self-skips when the variable is unset).
  WEBBOTAUTH_SIGN_TOOL="$BAZEL_EXECROOT/$(bazel cquery --config=docker --output=files //src/crypto/webbotauth:webbotauth_sign_tool 2>/dev/null)"
  echo "Sign tool at: $WEBBOTAUTH_SIGN_TOOL"
fi

echo "=== Step 5: ABI check ==="
echo "load_module $MODULE_SO;
events { worker_connections 64; }
http { server { listen 19840; } }" > /tmp/abi-test.conf
nginx -t -c /tmp/abi-test.conf 2>&1
echo "ABI check passed"

echo "=== Step 6: Seeding cache for HIT-path tests ==="
rm -f /tmp/test-ps-*.vol /tmp/test-ps-*.vol.gen /tmp/test-ps-*.sock
export SEED_TEST_CACHE="$SEED_TOOL"
for seed_script in /workspace/t/seed-*.sh; do
  [ -f "$seed_script" ] && bash "$seed_script"
done

echo "=== Step 7: Running test-nginx tests ==="
export TEST_NGINX_LOAD_MODULES="$MODULE_SO"
export TEST_NGINX_LOG_LEVEL=warn
export TEST_NGINX_SERVER_PORT=19840
export WEBBOTAUTH_SIGN_TOOL
# PROVE_OPTS is prove options only; test files are positional arguments.
# PROVE_ARGS (older name) holds the whole prove invocation.
if [ -z "${PROVE_OPTS:-}" ] && [ -n "${PROVE_ARGS:-}" ]; then
  # shellcheck disable=SC2086
  prove $PROVE_ARGS
else
  if [ $# -eq 0 ]; then
    # No test files given: run the full suite.
    PROVE_OPTS="${PROVE_OPTS:--v -r}"
    set -- t/
  fi
  prove ${PROVE_OPTS:--v} "$@"
fi
