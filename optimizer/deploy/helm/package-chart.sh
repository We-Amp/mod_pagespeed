#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# Packages the `pagespeed` Helm chart and appends it to the classic Helm-repo
# index that is published as a static asset under the public website and served
# at https://modpagespeed.com/charts/.
#
# Run this whenever the chart changes (template/values edits or a Chart.yaml
# version bump), then commit the regenerated files under
# website/public/charts/. Customers consume the result with:
#
#   helm repo add weamp https://modpagespeed.com/charts
#   helm install pagespeed weamp/pagespeed ...
#
# The index is append-only. `helm repo index` regenerates every entry from
# the .tgz files it can see, which re-stamps their `created` dates, drifts
# digests (packaging is not reproducible across checkouts; the tarball
# carries file mtimes) and re-derives entry metadata from packages that
# should no longer speak for themselves. Even `--merge` overwrites matching
# versions and re-serializes the file. So the chart is packaged and indexed
# in a scratch directory and only its single fresh entry is spliced into the
# published index, by text, so every published line is carried over
# byte-identical. Re-running for a version the index already has changes
# nothing (the published .tgz is left in place for the same reason).
#
# Usage: deploy/helm/package-chart.sh   (set HELM=/path/to/helm to override)
set -euo pipefail

REPO_URL="https://modpagespeed.com/charts"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHART_DIR="$HERE/pagespeed"
OUT_DIR="$HERE/../../website/public/charts"
HELM="${HELM:-helm}"

# Chart name and version out of Chart.yaml (the same bare parse style
# tools/ci/check_helm.sh uses for appVersion).
CHART_NAME=$(sed -nE 's/^name:[[:space:]]+//p' "$CHART_DIR/Chart.yaml" | head -1)
CHART_VERSION=$(sed -nE 's/^version:[[:space:]]+//p' "$CHART_DIR/Chart.yaml" | head -1)
if [ -z "$CHART_NAME" ] || [ -z "$CHART_VERSION" ]; then
  echo "ERROR: cannot read name/version from $CHART_DIR/Chart.yaml" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

"$HELM" package "$CHART_DIR" --destination "$SCRATCH"

INDEX="$OUT_DIR/index.yaml"
if [ -f "$INDEX" ] \
  && grep -qE "^[[:space:]]+version: ${CHART_VERSION//./\\.}\$" "$INDEX"; then
  echo "$CHART_NAME $CHART_VERSION is already in $INDEX; nothing to publish."
  exit 0
fi

# Index ONLY the scratch package -- never $OUT_DIR, whose older packages must
# not be re-derived -- giving a fresh index with exactly one entry for the
# new version.
"$HELM" repo index "$SCRATCH" --url "$REPO_URL"
if [ ! -f "$INDEX" ]; then
  # First publish: the scratch index is the whole repo index.
  cp "$SCRATCH/index.yaml" "$INDEX"
else
  # The new entry, verbatim out of the scratch index: every line under the
  # chart's entries key up to the trailing generated stamp.
  ENTRY="$SCRATCH/entry.yaml"
  sed -n "/^  ${CHART_NAME}:\$/,/^generated:/p" "$SCRATCH/index.yaml" \
    | sed '1d;$d' > "$ENTRY"
  grep -qE "version: ${CHART_VERSION//./\\.}\$" "$ENTRY" || {
    echo "ERROR: no $CHART_VERSION entry in the freshly generated index" >&2
    exit 1
  }
  GENERATED=$(sed -n 's/^generated:[[:space:]]*//p' "$SCRATCH/index.yaml")

  # Splice it in directly under the chart's entries key (the newest version
  # leads the list) and take the fresh generated stamp; every other line of
  # the published index is printed untouched.
  awk -v key="  ${CHART_NAME}:" -v entry="$ENTRY" -v generated="$GENERATED" '
    !done && $0 == key {
      print
      while ((getline line < entry) > 0) print line
      close(entry)
      done = 1
      next
    }
    /^generated:/ { print "generated: " generated; next }
    { print }
  ' "$INDEX" > "$INDEX.tmp" || { rm -f "$INDEX.tmp"; exit 1; }
  grep -qE "^[[:space:]]+version: ${CHART_VERSION//./\\.}\$" "$INDEX.tmp" || {
    echo "ERROR: splice failed: no \"${CHART_NAME}:\" entries key in $INDEX" >&2
    rm -f "$INDEX.tmp"
    exit 1
  }
  mv "$INDEX.tmp" "$INDEX"
fi

mv "$SCRATCH/${CHART_NAME}-${CHART_VERSION}.tgz" "$OUT_DIR/"

echo "Packaged chart + index.yaml -> $OUT_DIR"
ls -1 "$OUT_DIR"
