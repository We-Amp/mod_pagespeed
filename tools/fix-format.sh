#!/bin/bash
# Reformats C/C++ sources in the same scope as CI's clang-format check
# (see the CI workflow's "clang-format check" step). Mirrors CI's find
# filter AND its `grep -L "DO NOT EDIT BY HAND"` skip, so it never reformats the
# vendored license_v2 crypto files (vendored from 2.0 per the design record; they follow
# 2.0 style and are guarded by the crypto-drift-check workflow, not 1.1
# clang-format). The only difference from CI is `-i` (fix) vs `--dry-run`.
#
# Requires clang-format-20 on PATH (or run from inside the dev container).

set -eu

cd "$(git rev-parse --show-toplevel)"

find pagespeed net -type f \
  \( -name "*.cc" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) \
  -not -path "pagespeed/iis/*" \
  -not -path "net/instaweb/rewriter/generated/*" \
  -print0 \
  | xargs -0 -r grep -LZ "DO NOT EDIT BY HAND" \
  | xargs -0 -r clang-format-20 -i
