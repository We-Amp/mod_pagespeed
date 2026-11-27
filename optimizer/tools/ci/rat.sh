#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# License audit with Apache RAT (Release Audit Tool), BLOCKING.
#
# Every file in the tree must either carry a recognised license header or be
# listed, with a reason, in .rat-excludes; the audit fails unless the report
# shows 0 Unknown Licenses. It complements tools/check-license-headers.sh,
# which enforces the exact SPDX header per source-file type: RAT is the
# whole-tree sweep that also catches file types that gate does not know about.
#
# Usage: tools/ci/rat.sh [REPORT_PATH]
#   Needs java (8 or newer) and curl on PATH. The pinned RAT jar is fetched
#   from Maven Central (repo1, then the repo.maven.apache.org mirror, then the
#   Apache archive binary tarball) into a cache directory outside the tree, and
#   its SHA-1 -- the only checksum Maven publishes for this artifact -- is
#   verified before use (fail closed). The download goes to a per-process temp
#   file that is renamed into place only after it verifies, so a truncated
#   download never becomes the cached jar and concurrent jobs sharing the
#   cache cannot race on a common path. The report is written to a per-run
#   scratch dir (under RUNNER_TEMP when set) while scanning and then moved to
#   REPORT_PATH (default: rat-report.txt in the repo root -- git-ignored and
#   listed in .rat-excludes so a rerun does not audit it).
#
# RAT 0.16 gotchas this script is built around:
#   * It honours EITHER -e command-line exclusions OR the -E file, never both,
#     so everything (incl. .git, bazel-*, node_modules) lives in .rat-excludes
#     and only -E is passed.
#   * Exclusion lines match the bare file/directory NAME as a Java regex; an
#     invalid regex is silently dropped with a stderr warning. That warning is
#     treated as a failure here, so a broken pattern cannot widen the audit's
#     blind spot unnoticed.
set -euo pipefail

RAT_VERSION="0.16.1"
RAT_SHA1="7a35d6881c9430c51ecb346bae662ee9832fe59a"
# Download sources, tried in order: Maven Central's repo1 CDN (which has
# flaked with transient 404s), its repo.maven.apache.org mirror, and finally
# the Apache archive's binary tarball (the jar inside it is byte-identical to
# the Maven artifact). Whatever the source, the jar must match the pinned
# SHA-1 above. RAT_URL_PRIMARY is overridable to exercise the fallback chain.
RAT_URL_PRIMARY="${RAT_URL_PRIMARY:-https://repo1.maven.org/maven2/org/apache/rat/apache-rat/${RAT_VERSION}/apache-rat-${RAT_VERSION}.jar}"
RAT_URL_MIRROR="https://repo.maven.apache.org/maven2/org/apache/rat/apache-rat/${RAT_VERSION}/apache-rat-${RAT_VERSION}.jar"
RAT_URL_ARCHIVE="https://archive.apache.org/dist/creadur/apache-rat-${RAT_VERSION}/apache-rat-${RAT_VERSION}-bin.tar.gz"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPORT="${1:-${REPO_ROOT}/rat-report.txt}"
CACHE_DIR="${RAT_CACHE_DIR:-${XDG_CACHE_HOME:-${HOME}/.cache}/apache-rat}"
JAR="${CACHE_DIR}/apache-rat-${RAT_VERSION}.jar"

for tool in java curl; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "::error::${tool} not found on PATH -- required by tools/ci/rat.sh" >&2
    exit 1
  fi
done

sha1_of() {
  if command -v sha1sum >/dev/null 2>&1; then
    sha1sum "$1" | awk '{print $1}'
  else
    shasum -a 1 "$1" | awk '{print $1}'
  fi
}

# --- Fetch + verify the pinned jar (fail closed on any mismatch) -------------
mkdir -p "$CACHE_DIR"
scratch="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/rat.XXXXXX")"
part=""
trap 'rm -rf "$scratch" "$part"' EXIT

fetch() {
  # fetch <url> <dest>: retried curl; returns non-zero so the caller can try
  # the next source. --retry-all-errors also retries HTTP 404 responses,
  # which the CDN has served transiently next to the real artifact.
  curl --fail --silent --show-error --location \
    --retry 4 --retry-all-errors --retry-delay 5 -o "$2" "$1"
}

if [[ ! -f "$JAR" ]] || [[ "$(sha1_of "$JAR")" != "$RAT_SHA1" ]]; then
  echo "Downloading Apache RAT ${RAT_VERSION}..."
  # Per-process temp name INSIDE the cache dir: the final step is then an
  # atomic rename on the same filesystem, so a concurrent job on the same
  # host either sees no jar or a complete, verified one -- never a partial.
  part="$(mktemp "${JAR}.XXXXXX")"
  if ! fetch "$RAT_URL_PRIMARY" "$part"; then
    echo "::warning::Maven Central primary (${RAT_URL_PRIMARY}) failed; trying the repo.maven.apache.org mirror" >&2
    if ! fetch "$RAT_URL_MIRROR" "$part"; then
      echo "::warning::Maven Central mirror failed too; trying the Apache archive" >&2
      tgz="${scratch}/apache-rat-${RAT_VERSION}-bin.tar.gz"
      if ! fetch "$RAT_URL_ARCHIVE" "$tgz"; then
        echo "::error::all Apache RAT download sources failed" >&2
        exit 1
      fi
      # The archive only ships the jar inside its binary tarball.
      if ! tar -xzf "$tgz" -O "apache-rat-${RAT_VERSION}/apache-rat-${RAT_VERSION}.jar" >"$part"; then
        echo "::error::could not extract the jar from ${RAT_URL_ARCHIVE}" >&2
        exit 1
      fi
    fi
  fi
  got="$(sha1_of "$part")"
  if [[ "$got" != "$RAT_SHA1" ]]; then
    echo "::error::apache-rat-${RAT_VERSION}.jar SHA-1 mismatch: expected ${RAT_SHA1}, got ${got} -- refusing to run an unverified jar" >&2
    exit 1
  fi
  mv -f "$part" "$JAR"
  part=""
fi
echo "Using $(basename "$JAR") (sha1 ${RAT_SHA1})"

# --- Run the audit -------------------------------------------------------------
cd "$REPO_ROOT"
if [[ ! -f .rat-excludes ]]; then
  echo "::error::.rat-excludes not found in ${REPO_ROOT}" >&2
  exit 1
fi

# Write outside the tree while scanning so the report cannot audit itself,
# then move it into place.
set +e
java -jar "$JAR" --dir . --scan-hidden-directories -E .rat-excludes \
  -o "${scratch}/report.txt" 2>"${scratch}/stderr.txt"
rc=$?
set -e
# RAT reports its comment-line count on stderr prefixed "ERROR:" although it
# is informational; keep everything else it says.
grep -v 'lines in your exclusion files as comments or empty lines' "${scratch}/stderr.txt" >&2 || true
if [[ $rc -ne 0 ]]; then
  echo "::error::Apache RAT exited with status ${rc}" >&2
  exit "$rc"
fi
mkdir -p "$(dirname "$REPORT")"
mv -f "${scratch}/report.txt" "$REPORT"

# A dropped exclusion pattern silently widens the blind spot -- fail on it.
if grep -q 'Will skip given exclusion' "${scratch}/stderr.txt"; then
  echo "::error::RAT dropped one or more .rat-excludes patterns (invalid regex, see above). Fix the pattern; extension rules are spelled '.*\\.ext'." >&2
  exit 1
fi

# --- Evaluate ------------------------------------------------------------------
echo
sed -n '/^Summary$/,/Unknown Licenses$/p' "$REPORT"
echo
unknown="$(grep -E '^[0-9]+ Unknown Licenses$' "$REPORT" | awk '{print $1}' | head -n1)"
if [[ -z "$unknown" ]]; then
  echo "::error::could not find the 'Unknown Licenses' summary line in ${REPORT}" >&2
  exit 1
fi
if [[ "$unknown" != "0" ]]; then
  echo "::error::Apache RAT found ${unknown} file(s) with an unknown license. Add the SPDX header (first-party source) or, for non-source / third-party / generated / content-addressed files only, a reasoned pattern in .rat-excludes:" >&2
  echo >&2
  awk '/^Files with unapproved licenses:/{p=1;next} /^\*\*\*\*/{if(p)exit} p' "$REPORT" | grep -v '^$' >&2
  echo >&2
  echo "Full report: ${REPORT}" >&2
  exit 1
fi
echo "License audit OK: 0 Unknown Licenses (report: ${REPORT})"
