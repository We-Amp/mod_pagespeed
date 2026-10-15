#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Version handling of the module release lane (docs/ci/release.md).
#
#   release_version.sh parse TAG
#       Validate TAG (module-v<X.Y.Z>[-<prerelease>]) and print the derived
#       values as key=value lines (suitable for $GITHUB_OUTPUT):
#         tag, version, base, major, minor, build, prerelease, is_prerelease,
#         release_tag (v<version>, the spelling the packaged admin console and
#         the smoke tests use), optimizer_tag (optimizer-v<version>), pkg_version
#         (the dpkg/rpm spelling: the first '-' becomes '~').
#   release_version.sh stamp TAG VERSION_FILE
#       Rewrite MAJOR, MINOR, BUILD, PRERELEASE and LASTCHANGE in the tree's
#       net/instaweb/public/VERSION so that the packages carry the module
#       version of the tag. The file in the repository carries the product line
#       version; the module version comes from the tag only. LASTCHANGE is the
#       package revision and is always 1: this lane has no reship spelling, a
#       new build of the same sources gets a new tag.
#   release_version.sh --self-test
#
# Tags: `module-v1.19.0` (a release) and `module-v1.19.0-rc.1` (a release
# candidate, flagged as a pre-release). Build metadata (`+...`) is refused.
set -euo pipefail

SEMVER_RE='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-([0-9A-Za-z]+(\.[0-9A-Za-z]+)*))?$'

parse() {
  local tag="${1:-}" ver pre base major minor build
  case "$tag" in
    module-v[0-9]*) ;;
    *) echo "release_version: '$tag' does not match module-v<major>.<minor>.<patch>[-<prerelease>]" >&2; return 1 ;;
  esac
  ver="${tag#module-v}"
  if ! printf '%s' "$ver" | grep -Eq "$SEMVER_RE"; then
    echo "release_version: '$ver' is not X.Y.Z[-prerelease] (no build metadata)" >&2
    return 1
  fi
  base="${ver%%-*}"
  pre=""
  case "$ver" in *-*) pre="${ver#*-}" ;; esac
  major="${base%%.*}"
  minor="${base#*.}"; minor="${minor%%.*}"
  build="${base##*.}"
  # A literal '~' in the replacement of ${var/pat/repl} is tilde-expanded by
  # bash 4.4+ (even inside double quotes); expand it from a variable instead.
  local tilde='~' pkg
  pkg="${ver/-/$tilde}"
  printf 'tag=%s\n' "$tag"
  printf 'version=%s\n' "$ver"
  printf 'base=%s\n' "$base"
  printf 'major=%s\n' "$major"
  printf 'minor=%s\n' "$minor"
  printf 'build=%s\n' "$build"
  printf 'prerelease=%s\n' "$pre"
  if [ -n "$pre" ]; then printf 'is_prerelease=true\n'; else printf 'is_prerelease=false\n'; fi
  printf 'release_tag=v%s\n' "$ver"
  printf 'optimizer_tag=optimizer-v%s\n' "$ver"
  printf 'pkg_version=%s\n' "$pkg"
}

# get KEY < parse output
field() { sed -n "s/^$1=//p" | head -n 1; }

stamp() {
  local tag="${1:-}" file="${2:-}" out tmp major minor build pre
  [ -f "$file" ] || { echo "release_version: no such file: $file" >&2; return 1; }
  out="$(parse "$tag")" || return 1
  major="$(printf '%s\n' "$out" | field major)"
  minor="$(printf '%s\n' "$out" | field minor)"
  build="$(printf '%s\n' "$out" | field build)"
  pre="$(printf '%s\n' "$out" | field prerelease)"
  tmp="${file}.stamp.$$"
  awk -v major="$major" -v minor="$minor" -v build="$build" -v pre="$pre" '
    /^MAJOR=/      { print "MAJOR=" major; seen_major = 1; next }
    /^MINOR=/      { print "MINOR=" minor; seen_minor = 1; next }
    /^BUILD=/      { print "BUILD=" build; seen_build = 1; next }
    /^PRERELEASE=/ { print "PRERELEASE=" pre; seen_pre = 1; next }
    /^LASTCHANGE=/ { print "LASTCHANGE=1"; seen_last = 1; next }
    { print }
    END {
      if (!seen_major) print "MAJOR=" major
      if (!seen_minor) print "MINOR=" minor
      if (!seen_build) print "BUILD=" build
      if (!seen_pre)   print "PRERELEASE=" pre
      if (!seen_last)  print "LASTCHANGE=1"
    }
  ' "$file" > "$tmp"
  mv "$tmp" "$file"
  if grep -q -- '-dirty' "$file"; then
    echo "release_version: $file contains a -dirty stamp after patching" >&2
    return 1
  fi
}

self_test() {
  local fail=0 t out
  t="$(mktemp -d)"
  trap 'rm -rf "$t"' RETURN
  ok() { if "$@" >/dev/null 2>&1; then :; else echo "FAIL: expected success: $*"; fail=1; fi; }
  no() { if "$@" >/dev/null 2>&1; then echo "FAIL: expected refusal: $*"; fail=1; fi; }

  ok parse module-v1.19.0
  ok parse module-v1.19.0-rc.1
  ok parse module-v0.0.1
  no parse module-v1.19
  no parse module-v1.19.0.1
  no parse module-v01.19.0
  no parse module-v1.19.0+r1
  no parse module-v1.19.0-
  no parse module-v1.19.0-rc..1
  no parse v1.19.0
  no parse optimizer-v1.19.0
  no parse module-1.19.0
  no parse ""

  out="$(parse module-v1.19.0-rc.1)"
  [ "$(printf '%s\n' "$out" | field version)" = "1.19.0-rc.1" ] || { echo "FAIL: version"; fail=1; }
  [ "$(printf '%s\n' "$out" | field base)" = "1.19.0" ] || { echo "FAIL: base"; fail=1; }
  [ "$(printf '%s\n' "$out" | field prerelease)" = "rc.1" ] || { echo "FAIL: prerelease"; fail=1; }
  [ "$(printf '%s\n' "$out" | field is_prerelease)" = "true" ] || { echo "FAIL: is_prerelease"; fail=1; }
  [ "$(printf '%s\n' "$out" | field pkg_version)" = "1.19.0~rc.1" ] || { echo "FAIL: pkg_version"; fail=1; }
  [ "$(printf '%s\n' "$out" | field release_tag)" = "v1.19.0-rc.1" ] || { echo "FAIL: release_tag"; fail=1; }
  [ "$(printf '%s\n' "$out" | field optimizer_tag)" = "optimizer-v1.19.0-rc.1" ] || { echo "FAIL: optimizer_tag"; fail=1; }
  out="$(parse module-v12.3.45)"
  [ "$(printf '%s\n' "$out" | field minor)" = "3" ] || { echo "FAIL: minor"; fail=1; }
  [ "$(printf '%s\n' "$out" | field build)" = "45" ] || { echo "FAIL: build"; fail=1; }
  [ "$(printf '%s\n' "$out" | field is_prerelease)" = "false" ] || { echo "FAIL: is_prerelease false"; fail=1; }
  [ "$(printf '%s\n' "$out" | field pkg_version)" = "12.3.45" ] || { echo "FAIL: pkg_version ga"; fail=1; }

  # stamp: replaces, appends the missing keys, keeps unrelated lines.
  printf 'MAJOR=2\nMINOR=4\nBUILD=0\nPATCH=0\n' > "$t/VERSION"
  stamp module-v1.19.0 "$t/VERSION" || { echo "FAIL: stamp ga"; fail=1; }
  want="$(printf 'MAJOR=1\nMINOR=19\nBUILD=0\nPATCH=0\nPRERELEASE=\nLASTCHANGE=1\n')"
  [ "$(cat "$t/VERSION")" = "$want" ] || { echo "FAIL: stamp content"; cat "$t/VERSION"; fail=1; }
  stamp module-v1.19.1-rc.2 "$t/VERSION" || { echo "FAIL: stamp rc"; fail=1; }
  want="$(printf 'MAJOR=1\nMINOR=19\nBUILD=1\nPATCH=0\nPRERELEASE=rc.2\nLASTCHANGE=1\n')"
  [ "$(cat "$t/VERSION")" = "$want" ] || { echo "FAIL: stamp rc content"; cat "$t/VERSION"; fail=1; }
  # idempotent
  cp "$t/VERSION" "$t/first"; stamp module-v1.19.1-rc.2 "$t/VERSION"
  cmp -s "$t/first" "$t/VERSION" || { echo "FAIL: stamp not idempotent"; fail=1; }
  # a stale LASTCHANGE is reset
  printf 'MAJOR=2\nMINOR=4\nBUILD=0\nLASTCHANGE=9\n' > "$t/V2"
  stamp module-v3.0.0 "$t/V2"
  grep -qx 'LASTCHANGE=1' "$t/V2" || { echo "FAIL: LASTCHANGE reset"; fail=1; }
  no stamp module-v1.2 "$t/V2"
  no stamp module-v1.2.3 "$t/does-not-exist"
  [ "$fail" = 0 ] && echo "release_version self-test: ok"
  return "$fail"
}

case "${1:-}" in
  --self-test) self_test ;;
  parse) shift; parse "${1:-}" ;;
  stamp) shift; stamp "${1:-}" "${2:-}" ;;
  *) echo "usage: $0 parse TAG | stamp TAG VERSION_FILE | --self-test" >&2; exit 2 ;;
esac
