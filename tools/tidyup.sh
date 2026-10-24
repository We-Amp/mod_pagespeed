#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run clang-tidy on mod_pagespeed source.
# Usage: tools/tidyup.sh [--fix]
#
# Generates compile_commands.json via gen_compilation_database.py, then runs
# clang-tidy-20. Must be run inside the development Docker container where
# clang-tidy-20 is available.
#
# pagespeed/envoy is excluded via --deleted_packages: generating its
# compilation database would build Envoy's generated sources. The envoy source
# files are still linted via the header-filter, but only when included by other
# translation units.
#
# Options:
#   --fix    Apply auto-fixes in-place

set -euo pipefail

FIX_FLAG=""
for arg in "$@"; do
  case "$arg" in
    --fix) FIX_FLAG="-fix" ;;
    *) echo "Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

cd "$(bazel info workspace)"

echo "Generating compile_commands.json..."
BAZEL_BUILD_OPTIONS="--deleted_packages=pagespeed/envoy" \
  tools/gen_compilation_database.py -- //pagespeed/... //net/...

echo "Running clang-tidy-20 (fix=$([[ -n "$FIX_FLAG" ]] && echo yes || echo no))..."
# The same anchored roots as the CI lint jobs: run-clang-tidy matches the
# positional regexes with an unanchored search against the absolute path
# <output base>/execroot/<workspace>/<file> it rebuilds from each compdb
# entry, so unanchored roots would also match test/pagespeed/ and test/net/
# entries that the pull request lint never covers.
run-clang-tidy-20 $FIX_FLAG \
  -clang-tidy-binary=clang-tidy-20 \
  -p . \
  '(^|/execroot/[^/]+/)base/' \
  '(^|/execroot/[^/]+/)pagespeed/' \
  '(^|/execroot/[^/]+/)net/'

echo "done"
