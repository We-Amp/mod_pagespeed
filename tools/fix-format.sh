#!/bin/bash
# Reformats C/C++ sources in the same scope as CI's clang-format check
# (see the CI workflow's "clang-format check" step).
#
# Requires clang-format-20 on PATH (or run from inside the dev container).

set -eu

cd "$(git rev-parse --show-toplevel)"

find pagespeed net -type f \
  \( -name "*.cc" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) \
  -not -path "pagespeed/iis/*" \
  -print0 \
  | xargs -0 clang-format-20 -i
