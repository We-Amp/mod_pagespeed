#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# License audit: run Apache RAT (Release Audit Tool) over the repository and
# fail unless every file it classifies either carries an approved license
# header or is listed in .rat-excludes. Backs the "License audit (Apache RAT)"
# job in the maintainer CI workflow and runs the same way locally
# (needs java on PATH; the first run downloads the pinned jar).
#
#   tools/ci/rat.sh                 audit the repository root
#   RAT_JAR=/path/to/jar ...        reuse an already-downloaded jar
#   RAT_REPORT=/path/to/report ...  write the full report elsewhere
#
# The jar is pinned by version and SHA-1; the download fails closed on any
# mismatch. RAT 0.16.1 honours either -e options or the -E excludes file,
# never both, so every exclusion (including .git and the Bazel output trees)
# lives in .rat-excludes and RAT is invoked with -E only. A pattern RAT cannot
# compile is silently dropped from the exclusion set, so its "Will skip given
# exclusion" diagnostic is treated as a failure too.
set -euo pipefail

RAT_VERSION="0.16.1"
RAT_SHA1="7a35d6881c9430c51ecb346bae662ee9832fe59a"
RAT_URL="https://repo1.maven.org/maven2/org/apache/rat/apache-rat/${RAT_VERSION}/apache-rat-${RAT_VERSION}.jar"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EXCLUDES="$ROOT/.rat-excludes"
if [ ! -f "$EXCLUDES" ]; then
  echo "::error::$EXCLUDES is missing" >&2
  exit 1
fi
if ! command -v java >/dev/null 2>&1; then
  echo "::error::java not found on PATH" >&2
  exit 1
fi

sha1_of() {
  if command -v sha1sum >/dev/null 2>&1; then
    sha1sum "$1" | cut -d' ' -f1
  else
    shasum -a 1 "$1" | cut -d' ' -f1
  fi
}

fetch() {
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --retry 3 -o "$2" "$1"
  else
    wget -q -O "$2" "$1"
  fi
}

SCRATCH="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
JAR="${RAT_JAR:-$SCRATCH/apache-rat-${RAT_VERSION}.jar}"
if [ ! -f "$JAR" ] || [ "$(sha1_of "$JAR")" != "$RAT_SHA1" ]; then
  echo "Downloading apache-rat ${RAT_VERSION}..."
  rm -f "$JAR" "$JAR.tmp"
  fetch "$RAT_URL" "$JAR.tmp"
  got="$(sha1_of "$JAR.tmp")"
  if [ "$got" != "$RAT_SHA1" ]; then
    rm -f "$JAR.tmp"
    echo "::error::apache-rat jar SHA-1 mismatch: expected $RAT_SHA1, got $got" >&2
    exit 1
  fi
  mv "$JAR.tmp" "$JAR"
fi
echo "apache-rat ${RAT_VERSION} sha1 ${RAT_SHA1} OK"

# The report is written outside the tree so RAT never audits its own output;
# a copy lands in the repository root for the CI artifact upload.
REPORT="${RAT_REPORT:-$SCRATCH/rat-report.txt}"
RAT_LOG="$(mktemp)"
trap 'rm -f "$RAT_LOG"' EXIT

set +e
# Both streams are kept: RAT's diagnostics (including the dropped-pattern
# notice checked below) go through its own logger, not necessarily stderr.
java -jar "$JAR" --dir . --scan-hidden-directories -E "$EXCLUDES" -o "$REPORT" >"$RAT_LOG" 2>&1
rc=$?
set -e
if [ "$REPORT" != "$ROOT/rat-report.txt" ] && [ -f "$REPORT" ]; then
  cp "$REPORT" "$ROOT/rat-report.txt"
fi
if [ $rc -ne 0 ] || [ ! -s "$REPORT" ]; then
  echo "::error::apache-rat exited with status $rc" >&2
  cat "$RAT_LOG" >&2
  exit 1
fi
if grep -q 'Will skip given exclusion' "$RAT_LOG"; then
  echo "::error::.rat-excludes contains a pattern RAT rejected:" >&2
  grep 'Will skip given exclusion' "$RAT_LOG" >&2
  exit 1
fi

# Summary block (the first lines of the report, up to the Unknown Licenses count).
sed -n '1,20p' "$REPORT"

unknown="$(grep -E '^[0-9]+ Unknown Licenses' "$REPORT" | awk '{print $1}' | head -1 || true)"
if [ -z "$unknown" ]; then
  echo "::error::could not find the Unknown Licenses count in the RAT report" >&2
  exit 1
fi
if [ "$unknown" != "0" ]; then
  echo "::error::$unknown file(s) without an approved license header (add the SPDX header, or list a legitimately header-less file in .rat-excludes):" >&2
  awk '/^Files with unapproved licenses:/{p=1;next} /^\*\*\*\*/{if(p)exit} p' "$REPORT" | sed '/^[[:space:]]*$/d' >&2
  exit 1
fi
echo "OK: 0 Unknown Licenses."
