#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Install a pinned anchore tool (syft or grype) into a bin dir, ABORTING on
# any sha256 mismatch before the archive is extracted or anything is
# installed. Replaces `curl .../install.sh | sh`, whose checksum failure
# ("hash_sha256_verify ... did not verify") was logged and then ignored,
# leaving a truncated binary on PATH (segfault, exit 139; issue #859).
#
# Usage: install-pinned-tool.sh <syft|grype> <vX.Y.Z> <bin-dir>
# Env:   INSTALL_RETRIES   download+verify attempts (default 3)
#        PINNED_TOOL_BASE  override of https://github.com/anchore (tests)
#        PINNED_TOOL_OS / PINNED_TOOL_ARCH  override platform (tests)
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <syft|grype> <vX.Y.Z> <bin-dir>" >&2
  exit 2
fi
tool="$1" version="$2" bindir="$3"
case "$tool" in syft|grype) ;; *) echo "install-pinned-tool: unsupported tool '$tool'" >&2; exit 2 ;; esac
case "$version" in v[0-9]*) ;; *) echo "install-pinned-tool: version must look like v1.2.3, got '$version'" >&2; exit 2 ;; esac

os="${PINNED_TOOL_OS:-$(uname -s | tr '[:upper:]' '[:lower:]')}"
arch="${PINNED_TOOL_ARCH:-$(uname -m)}"
case "$arch" in x86_64) arch=amd64 ;; aarch64) arch=arm64 ;; esac
base="${PINNED_TOOL_BASE:-https://github.com/anchore}"
retries="${INSTALL_RETRIES:-3}"
ver="${version#v}"
tarball="${tool}_${ver}_${os}_${arch}.tar.gz"
sums="${tool}_${ver}_checksums.txt"
url="${base}/${tool}/releases/download/${version}"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

attempt=1
while :; do
  rm -f "$tmp/$tarball" "$tmp/$sums"
  err=""
  if ! curl -sSfL --retry 2 -o "$tmp/$tarball" "$url/$tarball" \
     || ! curl -sSfL --retry 2 -o "$tmp/$sums" "$url/$sums"; then
    err="download failed"
  else
    want="$(awk -v f="$tarball" '$2==f || $2=="*"f {print $1; exit}' "$tmp/$sums")"
    got="$(sha256_of "$tmp/$tarball")"
    if [ -z "$want" ]; then
      err="no checksum entry for $tarball in $sums"
    elif [ "$want" != "$got" ]; then
      err="sha256 mismatch for $tarball (expected $want, got $got)"
    fi
  fi
  [ -z "$err" ] && break
  echo "install-pinned-tool: $tool $version attempt $attempt/$retries: $err" >&2
  if [ "$attempt" -ge "$retries" ]; then
    echo "install-pinned-tool: ABORTING: $tool $version not installed (nothing was extracted)" >&2
    exit 1
  fi
  attempt=$((attempt + 1))
  sleep "${INSTALL_RETRY_SLEEP:-2}"
done

mkdir -p "$bindir"
tar -xzf "$tmp/$tarball" -C "$tmp" "$tool"
install -m 0755 "$tmp/$tool" "$bindir/$tool"
echo "install-pinned-tool: installed $tool $version (sha256 verified) to $bindir"
