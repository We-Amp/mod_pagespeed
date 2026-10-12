#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Build the SHA256SUMS manifest for the files of a GitHub release.
#
#   release_sha256sums.sh DIR      rewrite DIR/SHA256SUMS from the files in DIR
#   release_sha256sums.sh --self-test
#
# Properties the release page relies on:
#   - one row per file in DIR (the manifest itself excluded), never a duplicate;
#   - rows sorted by file name in the C locale, so the output is deterministic;
#   - names are the names the files carry on the release page. GitHub stores an
#     uploaded asset with every '~' replaced by '.', so the files are renamed
#     that way here, before upload, and the manifest lists those names. That
#     makes `sha256sum -c` work against the downloaded assets as they are.
#   - a name outside [A-Za-z0-9._+-] (after that mapping) is an error, because
#     GitHub rewrites other characters in ways we do not control;
#   - two files that would end up with the same name are an error.
set -euo pipefail

sum_cmd() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

build() {
  local dir="$1" f base new
  [ -d "$dir" ] || { echo "release_sha256sums: not a directory: $dir" >&2; return 1; }
  # Rename '~' to '.' first, refusing to overwrite an existing file.
  for f in "$dir"/*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    case "$base" in *"~"*) ;; *) continue ;; esac
    new=${base//\~/.}
    if [ -e "$dir/$new" ]; then
      echo "release_sha256sums: '$base' and '$new' collide after renaming" >&2
      return 1
    fi
    mv "$f" "$dir/$new"
  done
  # GitHub renames characters outside this set in an undocumented way; refuse them.
  for f in "$dir"/*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    case "$base" in
      SHA256SUMS) ;;
      *[!A-Za-z0-9._+-]*)
        echo "release_sha256sums: file name outside [A-Za-z0-9._+-]: $base" >&2
        return 1 ;;
    esac
  done
  local names
  names=$(cd "$dir" && for f in *; do
            if [ "$f" = SHA256SUMS ]; then continue; fi
            if [ -f "$f" ]; then
              printf '%s\n' "$f"
            else
              echo "release_sha256sums: skipping non-file: $f" >&2
            fi
          done | LC_ALL=C sort -u)
  if [ -z "$names" ]; then
    echo "release_sha256sums: no files in $dir" >&2
    return 1
  fi
  rm -f "$dir/SHA256SUMS"
  (cd "$dir" && printf '%s\n' "$names" | while IFS= read -r f; do sum_cmd -- "$f"; done) \
    > "$dir/SHA256SUMS.tmp"
  mv "$dir/SHA256SUMS.tmp" "$dir/SHA256SUMS"
}

self_test() {
  local t fail=0
  t=$(mktemp -d)
  trap 'rm -rf "$t"' RETURN
  mkdir "$t/a"
  printf 'one' > "$t/a/pagespeed-optimizer_1.16.0~rc.8-r1~bookworm_amd64.deb"
  printf 'two' > "$t/a/mod-pagespeed_1.16.0.rc.8_amd64.deb"
  printf 'three' > "$t/a/Zeta.tar.gz"
  printf 'old' > "$t/a/SHA256SUMS"   # stale manifest with duplicates must be replaced
  printf 'x  dup\nx  dup\n' >> "$t/a/SHA256SUMS"
  build "$t/a"
  local want
  want=$(cd "$t/a" && for f in Zeta.tar.gz mod-pagespeed_1.16.0.rc.8_amd64.deb pagespeed-optimizer_1.16.0.rc.8-r1.bookworm_amd64.deb; do sum_cmd -- "$f"; done)
  [ "$(cat "$t/a/SHA256SUMS")" = "$want" ] || { echo "FAIL: content/order"; fail=1; }
  [ "$(wc -l < "$t/a/SHA256SUMS" | tr -d ' ')" = 3 ] || { echo "FAIL: row count"; fail=1; }
  grep -q '~' "$t/a/SHA256SUMS" && { echo "FAIL: tilde name"; fail=1; }
  (cd "$t/a" && sum_cmd -c SHA256SUMS >/dev/null) || { echo "FAIL: sha256sum -c"; fail=1; }
  cp "$t/a/SHA256SUMS" "$t/first"; build "$t/a"
  cmp -s "$t/first" "$t/a/SHA256SUMS" || { echo "FAIL: not idempotent"; fail=1; }
  # collision
  mkdir "$t/b"; printf 1 > "$t/b/x~1"; printf 2 > "$t/b/x.1"
  if build "$t/b" 2>/dev/null; then echo "FAIL: collision accepted"; fail=1; fi
  # subdirectory sorting last, plus a stale manifest: must still succeed
  mkdir "$t/d" "$t/d/zdir"; printf 1 > "$t/d/a.deb"; printf old > "$t/d/SHA256SUMS"
  build "$t/d" 2>/dev/null || { echo "FAIL: subdir/stale manifest"; fail=1; }
  [ "$(wc -l < "$t/d/SHA256SUMS" | tr -d ' ')" = 1 ] || { echo "FAIL: subdir row count"; fail=1; }
  # special characters
  mkdir "$t/e"; printf 1 > "$t/e/bad name.deb"
  if build "$t/e" 2>/dev/null; then echo "FAIL: odd name accepted"; fail=1; fi
  # empty
  mkdir "$t/c"
  if build "$t/c" 2>/dev/null; then echo "FAIL: empty dir accepted"; fail=1; fi
  [ "$fail" = 0 ] && echo "release_sha256sums self-test: ok"
  return "$fail"
}

case "${1:-}" in
  --self-test) self_test ;;
  "") echo "usage: $0 DIR | --self-test" >&2; exit 2 ;;
  *) build "$1" ;;
esac
