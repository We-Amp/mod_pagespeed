#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Switch between the lean WORKSPACE (default) and the Envoy WORKSPACE.
#
# Usage:
#   ./scripts/use-envoy-workspace.sh          # activate Envoy workspace
#   ./scripts/use-envoy-workspace.sh --lean   # restore lean workspace
#
# The lean WORKSPACE (default) does not load Envoy dependencies.
# Use this for Apache, nginx, and IIS builds.
#
# The Envoy WORKSPACE loads the full Envoy dependency graph (~100+ repos).
# Use this for building the Envoy HTTP filter.
#
# Also swaps .bazelignore: the lean build ignores pagespeed/envoy (its BUILD
# files load @envoy rules that don't exist in the lean workspace), which
# would otherwise make the envoy packages unbuildable in the Envoy workspace
# too ("Package is considered deleted").

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

if [[ "${1:-}" == "--lean" ]]; then
    if [[ -f WORKSPACE.lean.bak ]]; then
        cp WORKSPACE.lean.bak WORKSPACE
        rm -f WORKSPACE.lean.bak
        echo "Restored lean WORKSPACE (default)."
    else
        echo "Already using lean WORKSPACE (no backup found)."
    fi
    if [[ -f .bazelignore.lean.bak ]]; then
        cp .bazelignore.lean.bak .bazelignore
        rm -f .bazelignore.lean.bak
        echo "Restored lean .bazelignore."
    fi
else
    if [[ ! -f WORKSPACE.envoy ]]; then
        echo "ERROR: WORKSPACE.envoy not found." >&2
        exit 1
    fi
    cp WORKSPACE WORKSPACE.lean.bak
    cp WORKSPACE.envoy WORKSPACE
    if [[ -f .bazelignore ]]; then
        cp .bazelignore .bazelignore.lean.bak
        : > .bazelignore
    fi
    echo "Activated Envoy WORKSPACE. Restore with: $0 --lean"
fi
