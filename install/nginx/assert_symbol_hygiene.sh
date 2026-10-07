#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# assert_symbol_hygiene.sh — deterministic symbol check on the built
# ngx_pagespeed_module.so. Run in BOTH build scripts and reused as the
# Phase C CI gate. Needs only binutils (readelf); no nginx required.
#
# The assertions live in tools/ci/assert_module_symbols.sh, shared with the
# Apache module; this wrapper selects the nginx profile. All FAIL (exit
# non-zero) on violation:
#   1. readelf -d: NO libssl / libcrypto in NEEDED. A leaked OpenSSL symbol
#      colliding with nginx's system OpenSSL segfaults the worker on load —
#      the highest-severity crash mode (R3).
#   2. undefined dynamic symbols: every one is either versioned by the C/C++
#      runtime or on the checker's short allow-list (the nginx API and a few
#      weak runtime hooks). The module carries its own crypto; any other
#      undefined reference would be bound to whatever nginx has loaded.
#   3. defined dynamic symbols: the ONLY exported ones are the nginx
#      module trio (ngx_modules / ngx_module_names / ngx_module_order). Anything
#      else (spdlog/protobuf/re2/...) means the version script (.lds) did
#      not take effect.
#
# Usage: assert_symbol_hygiene.sh <path-to-module.so>
set -euo pipefail

SO="${1:?usage: assert_symbol_hygiene.sh <module.so>}"
[ -f "${SO}" ] || { echo "FAIL: $SO not found" >&2; exit 1; }

SCRIPTDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHECKER="${SCRIPTDIR}/../../tools/ci/assert_module_symbols.sh"
[ -f "${CHECKER}" ] || { echo "FAIL: ${CHECKER} not found — the gate cannot run" >&2; exit 1; }

exec bash "${CHECKER}" --profile nginx "${SO}"
