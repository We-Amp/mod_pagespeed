#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Points git at the in-tree .githooks/ directory. Run once per clone.
# Safe to re-run; only toggles the repo's core.hooksPath config.

set -eu

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

git config core.hooksPath .githooks
echo "Installed hooks from .githooks/ (core.hooksPath set): pre-commit + pre-push clang-format checks."
echo "Bypass for one commit: git commit --no-verify"
