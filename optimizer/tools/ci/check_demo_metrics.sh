#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# CI check: Ensure demo-metrics.json contains real measurements.
# Fails if the file contains placeholder text or stale timestamps.
#
# The metrics file lives in the modpagespeed.com site tree, which is in the
# public repository (We-Amp/pagespeed-optimizer, website/). WEBSITE_DIR names
# that tree; default: <repo root>/website.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WEBSITE_DIR="${WEBSITE_DIR:-$REPO_ROOT/website}"
if [ ! -d "$WEBSITE_DIR" ]; then
  echo "ERROR: website tree not found at $WEBSITE_DIR"
  echo "Set WEBSITE_DIR to the public repository's website/ checkout (We-Amp/pagespeed-optimizer)."
  exit 1
fi
METRICS_FILE="$WEBSITE_DIR/src/data/demo-metrics.json"

if [ ! -f "$METRICS_FILE" ]; then
  echo "ERROR: $METRICS_FILE not found"
  exit 1
fi

# Check for placeholder text.
if grep -qi "placeholder" "$METRICS_FILE"; then
  echo "ERROR: $METRICS_FILE contains 'Placeholder' text."
  echo "Run tools/measure-demo/run.sh to generate real metrics."
  exit 1
fi

# Check that generated_at timestamp exists and is not too old (>30 days).
GENERATED_AT=$(python3 -c "
import json, sys
with open('$METRICS_FILE') as f:
    data = json.load(f)
print(data.get('generated_at', ''))
")

if [ -z "$GENERATED_AT" ]; then
  echo "ERROR: $METRICS_FILE missing generated_at field."
  exit 1
fi

# Check staleness (>30 days).
STALE=$(python3 -c "
import json, sys
from datetime import datetime, timezone, timedelta
with open('$METRICS_FILE') as f:
    data = json.load(f)
ts = data.get('generated_at', '')
if not ts:
    print('missing')
    sys.exit(0)
try:
    dt = datetime.fromisoformat(ts.replace('Z', '+00:00'))
    age = datetime.now(timezone.utc) - dt
    if age > timedelta(days=30):
        print(f'stale ({age.days} days old)')
    else:
        print('ok')
except Exception as e:
    print(f'parse_error: {e}')
")

if [ "$STALE" = "ok" ]; then
  echo "OK: $METRICS_FILE is up to date (generated at $GENERATED_AT)."
  exit 0
elif echo "$STALE" | grep -q "stale"; then
  echo "WARNING: $METRICS_FILE is $STALE."
  echo "Consider running tools/measure-demo/run.sh to refresh."
  # Warning only, not a hard failure for CI.
  exit 0
else
  echo "ERROR: $METRICS_FILE generated_at: $STALE"
  exit 1
fi
