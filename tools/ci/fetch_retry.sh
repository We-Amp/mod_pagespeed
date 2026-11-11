#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Download one file with retries, so a short outage of a download host does
# not fail a CI job. A checksum mismatch is NOT retried: it is a hard failure.
#
# Usage: fetch_retry.sh <url> <dest> [<sha256>]
#   <sha256>  when given, the download is verified before it is moved to
#             <dest>; on a mismatch nothing is left at <dest> and the exit
#             status is 3.
# Env:   FETCH_RETRY_COUNT         curl --retry count (default 8)
#        FETCH_RETRY_MAX_TIME      total retry window in seconds (default 900)
#        FETCH_CONNECT_TIMEOUT     connect timeout in seconds (default 20)
# Exit:  0 ok, 2 usage, 3 checksum mismatch, otherwise curl's status.
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "usage: $0 <url> <dest> [<sha256>]" >&2
  exit 2
fi
url="$1" dest="$2" want="${3:-}"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

mkdir -p "$(dirname "$dest")"
tmp="$(mktemp "${dest}.XXXXXX")"
trap 'rm -f "$tmp"' EXIT

curl -fsSL \
  --retry "${FETCH_RETRY_COUNT:-8}" --retry-all-errors \
  --retry-max-time "${FETCH_RETRY_MAX_TIME:-900}" \
  --connect-timeout "${FETCH_CONNECT_TIMEOUT:-20}" \
  -o "$tmp" "$url"

if [ -n "$want" ]; then
  got="$(sha256_of "$tmp")"
  if [ "$got" != "$want" ]; then
    echo "fetch_retry: sha256 mismatch for $url (expected $want, got $got)" >&2
    exit 3
  fi
fi
mv -f "$tmp" "$dest"
trap - EXIT
