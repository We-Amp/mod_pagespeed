#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Generate test fixtures that are too large to check into git.
# Run this before E2E or HTTP compliance tests.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

generate_large_html() {
  local dest="$1"
  if [ -f "$dest" ]; then
    return
  fi
  echo "Generating $dest ..."
  python3 -c "
header = '<!DOCTYPE html>\n<html>\n<head><title>Large HTML Test</title></head>\n<body>\n<h1>Large Test</h1>\n'
footer = '</body>\n</html>\n'
block = '<p>Test paragraph for graceful degradation testing of the PageSpeed worker max-html-size limit.</p>\n'
target = 5 * 1024 * 1024 + 1024
repeat = (target - len(header) - len(footer)) // len(block) + 1
html = header + block * repeat + footer
with open('$dest', 'w') as f:
    f.write(html)
print(f'  {len(html)} bytes')
"
}

generate_large_html "$SCRIPT_DIR/e2e/testdata/large.html"
generate_large_html "$SCRIPT_DIR/http-compliance/testdata/large.html"
echo "Done."
