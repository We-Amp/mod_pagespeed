#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Build the cPanel (EasyApache 4) package of the module release from the stock
# Apache RPM of the same build (docs/ci/release.md). The package is a
# repackager spec (packaging/cpanel): it wraps the stock RPM, it does not
# compile anything. The result is unsigned; signing happens in a later,
# protected part of the lane.
#
#   release_ea4_build.sh --os el8|el9|el10 --upstream DIR --version-file FILE
#                        --out DIR [--spec-dir DIR] [--work DIR]
#   release_ea4_build.sh --self-test
#
#   --upstream      directory with exactly one mod-pagespeed-*.x86_64.rpm, the
#                   stock RPM of that operating system's floor
#   --version-file  the stamped net/instaweb/public/VERSION
#   --out           receives the cPanel RPM
#
# Exit status 0 with the line `built=true` or `built=false` on stdout (and in
# $GITHUB_OUTPUT when set). `built=false` is a skip with a reason on stderr,
# for el10 only (this operating system is additive): any mismatch is a skip.
# For el8 and el9 a stock RPM whose name does not match what the spec expects
# is an error, on a release and on a pre-release alike: a rehearsal builds
# the package a release would, so it must fail where a release fails. A
# pre-release stock RPM is named `X.Y.Z~PRERELEASE`; the package built from it
# carries that version.
# The spec's `%global mps_version` must be the version being built, except in
# the weekly rehearsal (PRERELEASE `rehearsal.N`): its version is made up from
# the optimizer pin, which differs from the spec after a module-only release,
# so the spec is built at the rehearsal version, with a notice.
set -euo pipefail
# shellcheck source=tools/ci/docker_pull_retry.sh
source "$(dirname "${BASH_SOURCE[0]}")/docker_pull_retry.sh"

OS="" UPSTREAM="" VERSION_FILE="" OUT="" SPEC_DIR="packaging/cpanel" WORK=""

# expected_name VERSION_FILE: the stock RPM name the spec expects.
expected_name() {
  local f="$1" major minor build last
  major="$(sed -n 's/^MAJOR=//p' "$f")"
  minor="$(sed -n 's/^MINOR=//p' "$f")"
  build="$(sed -n 's/^BUILD=//p' "$f")"
  last="$(sed -n 's/^LASTCHANGE=//p' "$f")"
  local pre; pre="$(sed -n 's/^PRERELEASE=//p' "$f")"
  echo "mod-pagespeed-${major}.${minor}.${build}${pre:+~$pre}-${last:-1}.x86_64.rpm"
}

# mirror_repo OS: the cPanel repository name for the version cross-check.
mirror_repo() {
  case "$1" in
    el8) echo ea4-c8 ;;
    el9) echo ea4-c9 ;;
    el10) echo ea4-a10 ;;
    *) return 1 ;;
  esac
}

emit() {
  echo "built=$1"
  if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "built=$1" >> "$GITHUB_OUTPUT"; fi
}

# patch_spec SPEC RELEASE [PRERELEASE [VERSION]]: set `%global mps_release`;
# with a VERSION set `%global mps_version` to it; with a PRERELEASE append
# `~PRERELEASE` to `%global mps_version` (the rpm spelling, and the name of
# the stock RPM the spec wraps).
patch_spec() {
  local spec="$1" rel="$2" pre="${3:-}" ver="${4:-}" tmp
  tmp="${spec}.tmp.$$"
  awk -v rel="$rel" -v pre="$pre" -v ver="$ver" '
    /^%global mps_release / { print "%global mps_release " rel; next }
    /^%global mps_version / {
      line = (ver != "") ? "%global mps_version " ver : $0
      print line ((pre != "") ? "~" pre : ""); next
    }
    { print }' "$spec" > "$tmp"
  mv "$tmp" "$spec"
}

inner_script() {
  cat <<'INNER'
#!/bin/bash
set -euo pipefail

# dnf with retries, for the throwaway EL containers: a short mirror or
# metadata outage (a Rocky/Alma mirror serving an empty repomd.xml, a curl
# error, a 5xx) is waited out, anything else (a missing package, a conflict,
# a dependency error) fails on the first attempt. Up to 3 tries, with
# `dnf clean metadata` and a growing pause between them; a lasting outage
# still fails loud with dnf's last output. Usage: dnf_retry install -y PKG...
# or dnf_retry repoquery ... (stderr is shown after each attempt, stdout after
# the last one). DNF_RETRY_SLEEP_UNIT (default 10) scales the pause. The function
# is spliced into container commands with declare -f, so it runs in the
# container's shell.
dnf_retry() {
  local attempt rc out err
  out="$(mktemp)" && err="$(mktemp)" || return 1
  for attempt in 1 2 3; do
    rc=0
    dnf "$@" --setopt=skip_if_unavailable=False >"$out" 2>"$err" || rc=$?
    cat "$err" >&2
    if [ "$rc" -eq 0 ]; then
      cat "$out"
      rm -f "$out" "$err"
      return 0
    fi
    if ! grep -Eqi 'Failed to download metadata|repomd\.xml parser error|Errors during downloading metadata|Curl error|Cannot download|Status code: 5[0-9][0-9]|Operation timed out|Timeout was reached|timed out|Could not resolve host|Cannot prepare internal mirrorlist|Failed to synchronize cache|Connection (reset|refused)' "$out" "$err"; then
      cat "$out"
      rm -f "$out" "$err"
      return "$rc"
    fi
    if [ "$attempt" -lt 3 ]; then
      echo "::warning::dnf $1 failed on a repository or network error (attempt $attempt of 3), retrying" >&2
      dnf clean metadata >/dev/null 2>&1 || true
      sleep $((attempt * ${DNF_RETRY_SLEEP_UNIT:-10}))
    fi
  done
  echo "::error::dnf $1 failed after 3 attempts" >&2
  cat "$out"
  rm -f "$out" "$err"
  return "$rc"
}

dnf_retry install -q -y rpm-build cpio findutils >/dev/null

# The Apache module interface version the cPanel Apache provides. The spec
# pins the package to it, so it is checked against the real repository below.
INJECTED_MMN="20120211x8664"
{
  printf "%%_httpd_mmn        %s\n" "$INJECTED_MMN"
  printf "%%_httpd_apxs       /usr/bin/apxs\n"
  printf "%%_httpd_dir        /etc/apache2\n"
  printf "%%_httpd_bindir     %%{_httpd_dir}/bin\n"
  printf "%%_httpd_modconfdir %%{_httpd_dir}/conf.modules.d\n"
  printf "%%_httpd_confdir    %%{_httpd_dir}/conf.d\n"
  printf "%%_httpd_contentdir /usr/share/apache2\n"
  printf "%%_httpd_moddir     /usr/lib64/apache2/modules\n"
} > /etc/rpm/macros.apache2

{
  printf "[cpanel-ea4-mmncheck]\n"
  printf "name=cPanel EA4 (module interface cross-check)\n"
  printf "mirrorlist=http://httpupdate.cpanel.net/%s-x86_64-mirrorlist\n" "$EA4_REPO"
  printf "enabled=0\n"
  printf "gpgcheck=0\n"
} > /etc/yum.repos.d/cpanel-ea4-mmncheck.repo
PROVIDED=""
for attempt in 1 2 3; do
  PROVIDED="$(dnf -q --enablerepo=cpanel-ea4-mmncheck repoquery \
    --whatprovides ea-apache24-mmn --provides 2>/dev/null \
    | sed -n "s/^ea-apache24-mmn = //p" | sed "s/[[:space:]]//g")"
  [ -n "$PROVIDED" ] && break
  echo "::notice::the ${EA4_REPO} query attempt ${attempt} came back empty; retrying"
  sleep 5
done
if [ -z "$PROVIDED" ]; then
  echo "::warning::could not read the module interface version from ${EA4_REPO} (unreachable or transient); using ${INJECTED_MMN}"
elif printf "%s\n" "$PROVIDED" | grep -qxF "$INJECTED_MMN"; then
  echo "module interface verified for ${EA4_OS}: ${INJECTED_MMN} is provided by ${EA4_REPO}"
else
  echo "::error::module interface mismatch for ${EA4_OS}: ${INJECTED_MMN} is not among what ${EA4_REPO} provides ($(printf "%s" "$PROVIDED" | tr "\n" " ")); the package would not install on a real host"
  exit 1
fi
rm -f /etc/yum.repos.d/cpanel-ea4-mmncheck.repo

mkdir -p /root/rpmbuild/{SOURCES,SPECS,RPMS,SRPMS,BUILD,BUILDROOT}
cp /work/spec/ea-apache24-mod_pagespeed.spec /root/rpmbuild/SPECS/
cp /work/spec/pagespeed-cpanel.conf          /root/rpmbuild/SOURCES/
cp /work/upstream/mod-pagespeed-*.x86_64.rpm /root/rpmbuild/SOURCES/
rpmbuild -bb --nodeps /root/rpmbuild/SPECS/ea-apache24-mod_pagespeed.spec
mkdir -p /work/build-out
cp /root/rpmbuild/RPMS/x86_64/*.rpm /work/build-out/
INNER
}

run() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --os) OS="$2"; shift 2 ;;
      --upstream) UPSTREAM="$2"; shift 2 ;;
      --version-file) VERSION_FILE="$2"; shift 2 ;;
      --out) OUT="$2"; shift 2 ;;
      --spec-dir) SPEC_DIR="$2"; shift 2 ;;
      --work) WORK="$2"; shift 2 ;;
      *) echo "release_ea4_build: unknown argument $1" >&2; return 2 ;;
    esac
  done
  [ -n "$OS" ] && [ -d "$UPSTREAM" ] && [ -f "$VERSION_FILE" ] && [ -n "$OUT" ] \
    || { echo "release_ea4_build: --os, --upstream, --version-file and --out are required" >&2; return 2; }
  local repo; repo="$(mirror_repo "$OS")" || { echo "release_ea4_build: unknown --os $OS" >&2; return 2; }

  local rpms=("$UPSTREAM"/mod-pagespeed-*.x86_64.rpm)
  if [ ! -e "${rpms[0]}" ]; then
    echo "::error::no stock mod-pagespeed-*.x86_64.rpm in the upstream directory"
    return 1
  fi
  if [ "${#rpms[@]}" -ne 1 ]; then
    echo "::error::${#rpms[@]} stock RPMs in the upstream directory, expected exactly one"
    return 1
  fi
  local have want; have="$(basename "${rpms[0]}")"; want="$(expected_name "$VERSION_FILE")"
  if [ "$have" != "$want" ]; then
    if [ "$OS" = el10 ]; then
      echo "::warning::stock RPM '$have' is not the '$want' the spec expects; skipping the el10 cPanel package (additive)" >&2
      emit false; return 0
    fi
    echo "::error::stock RPM '$have' is not the '$want' the spec expects. The cPanel package would not ship. Check MAJOR.MINOR.BUILD in net/instaweb/public/VERSION against %global mps_version in the spec." >&2
    return 1
  fi

  # The spec names the stock RPM it wraps through %global mps_version, so a
  # spec still at the previous version cannot find its Source1 and rpmbuild
  # fails late with a missing-file error. Say what is wrong before that.
  local spec_version file_version
  spec_version="$(sed -n 's/^%global mps_version //p' "$SPEC_DIR/ea-apache24-mod_pagespeed.spec" | head -n 1)"
  file_version="$(sed -n 's/^MAJOR=//p' "$VERSION_FILE").$(sed -n 's/^MINOR=//p' "$VERSION_FILE").$(sed -n 's/^BUILD=//p' "$VERSION_FILE")"
  local pre spec_override=""
  pre="$(sed -n 's/^PRERELEASE=//p' "$VERSION_FILE" | head -n 1)"
  if [ "$spec_version" != "$file_version" ] && [ "${pre#rehearsal.}" != "$pre" ]; then
    echo "::notice::rehearsal: %global mps_version $spec_version in the spec is not the rehearsal version $file_version (made up from the optimizer pin); building the spec at $file_version" >&2
    spec_override="$file_version"
  elif [ "$spec_version" != "$file_version" ]; then
    if [ "$OS" = el10 ]; then
      echo "::warning::%global mps_version $spec_version in the spec is not the release version $file_version; skipping the el10 cPanel package (additive)" >&2
      emit false; return 0
    fi
    echo "::error::%global mps_version $spec_version in $SPEC_DIR/ea-apache24-mod_pagespeed.spec is not the release version $file_version. Bump it (and add a %changelog entry) with the module version." >&2
    return 1
  fi

  [ -n "$WORK" ] || WORK="$(mktemp -d)"
  rm -rf "$WORK"; mkdir -p "$WORK/spec" "$WORK/upstream" "$WORK/build-out"
  cp -a "$SPEC_DIR/." "$WORK/spec/"
  cp "${rpms[0]}" "$WORK/upstream/"
  patch_spec "$WORK/spec/ea-apache24-mod_pagespeed.spec" "$(sed -n 's/^LASTCHANGE=//p' "$VERSION_FILE" | head -n 1 | sed 's/^$/1/')" "$pre" "$spec_override"
  inner_script > "$WORK/inner.sh"
  docker_pull_retry "almalinux:${OS#el}" || return 1
  docker run --rm --init \
    -v "$WORK":/work -w /work \
    -e EA4_OS="$OS" -e EA4_REPO="$repo" \
    "almalinux:${OS#el}" bash /work/inner.sh
  mkdir -p "$OUT"
  cp "$WORK"/build-out/*.rpm "$OUT"/
  ls -lh "$OUT"
  emit true
}

self_test() {
  local fail=0 t
  t="$(mktemp -d)"
  trap 'rm -rf "$t"' RETURN
  chk() { [ "$2" = "$3" ] || { echo "FAIL: $1: got '$2', want '$3'"; fail=1; }; }

  printf 'MAJOR=1\nMINOR=19\nBUILD=0\nPATCH=0\nPRERELEASE=\nLASTCHANGE=1\n' > "$t/V"
  chk "expected name" "$(expected_name "$t/V")" "mod-pagespeed-1.19.0-1.x86_64.rpm"
  printf 'MAJOR=1\nMINOR=19\nBUILD=0\nPRERELEASE=rc.1\nLASTCHANGE=1\n' > "$t/V2"
  chk "repo el8" "$(mirror_repo el8)" ea4-c8
  chk "repo el9" "$(mirror_repo el9)" ea4-c9
  chk "repo el10" "$(mirror_repo el10)" ea4-a10
  mirror_repo el7 >/dev/null 2>&1 && { echo "FAIL: el7 accepted"; fail=1; }

  printf '%%global mps_version 1.19.0\n%%global mps_release 1\nName: x\n' > "$t/spec"
  patch_spec "$t/spec" 4
  chk "patched spec" "$(sed -n 's/^%global mps_release //p' "$t/spec")" 4
  chk "spec untouched" "$(sed -n 's/^%global mps_version //p' "$t/spec")" 1.19.0

  # skip and error decisions, without docker (the checks come first)
  mkdir -p "$t/up" "$t/spec-dir"
  : > "$t/up/mod-pagespeed-1.19.0-1.x86_64.rpm"
  # a pre-release is named with its `~` suffix; a stock RPM without it is a
  # mismatch and an error, as on a release
  chk "expected name rc" "$(expected_name "$t/V2")" "mod-pagespeed-1.19.0~rc.1-1.x86_64.rpm"
  if run --os el9 --upstream "$t/up" --version-file "$t/V2" --out "$t/out" >/dev/null 2>&1; then
    echo "FAIL: pre-release mismatch accepted"; fail=1
  fi
  # positive: the `~` name is accepted (the run gets past the name check and
  # stops at the spec version check)
  mkdir -p "$t/uprc" "$t/spec-old"
  : > "$t/uprc/mod-pagespeed-1.19.0~rc.1-1.x86_64.rpm"
  printf '%%global mps_version 1.18.0\n%%global mps_release 1\n' > "$t/spec-old/ea-apache24-mod_pagespeed.spec"
  run --os el9 --upstream "$t/uprc" --version-file "$t/V2" --out "$t/out" --spec-dir "$t/spec-old" >"$t/err-rc" 2>&1 || true
  grep -qF "%global mps_version 1.18.0" "$t/err-rc" && ! grep -qF "stock RPM" "$t/err-rc" \
    || { echo "FAIL: ~ name not accepted: $(cat "$t/err-rc")"; fail=1; }
  printf '%%global mps_version 1.19.0\n%%global mps_release 1\n' > "$t/spec-rc"
  patch_spec "$t/spec-rc" 1 rc.1
  chk "patched spec rc" "$(sed -n 's/^%global mps_version //p' "$t/spec-rc")" "1.19.0~rc.1"
  printf '%%global mps_version 1.20.0\n%%global mps_release 1\n' > "$t/spec-rh"
  patch_spec "$t/spec-rh" 1 rehearsal.3 1.19.0
  chk "patched spec rehearsal" "$(sed -n 's/^%global mps_version //p' "$t/spec-rh")" "1.19.0~rehearsal.3"
  printf 'MAJOR=1\nMINOR=20\nBUILD=0\nPRERELEASE=\nLASTCHANGE=1\n' > "$t/V3"
  if run --os el9 --upstream "$t/up" --version-file "$t/V3" --out "$t/out" >/dev/null 2>&1; then
    echo "FAIL: release mismatch accepted"; fail=1
  fi
  run --os el10 --upstream "$t/up" --version-file "$t/V3" --out "$t/out" 2>/dev/null | grep -qx 'built=false' \
    || { echo "FAIL: el10 mismatch should skip"; fail=1; }
  # the spec's mps_version must be the release version (checked before docker)
  mkdir -p "$t/up25"
  : > "$t/up25/mod-pagespeed-2.5.0-1.x86_64.rpm"
  printf 'MAJOR=2\nMINOR=5\nBUILD=0\nPRERELEASE=\nLASTCHANGE=1\n' > "$t/V25"
  chk "expected name 2.5.0" "$(expected_name "$t/V25")" "mod-pagespeed-2.5.0-1.x86_64.rpm"
  printf '%%global mps_version 1.19.0\n%%global mps_release 1\n' > "$t/spec-dir/ea-apache24-mod_pagespeed.spec"
  if run --os el9 --upstream "$t/up25" --version-file "$t/V25" --out "$t/out" --spec-dir "$t/spec-dir" >"$t/err" 2>&1; then
    echo "FAIL: spec mps_version 1.19.0 accepted for a 2.5.0 release"; fail=1
  fi
  grep -qF "::error::%global mps_version 1.19.0 in $t/spec-dir/ea-apache24-mod_pagespeed.spec is not the release version 2.5.0" "$t/err" \
    || { echo "FAIL: spec mismatch error text: $(cat "$t/err")"; fail=1; }
  run --os el10 --upstream "$t/up25" --version-file "$t/V25" --out "$t/out" --spec-dir "$t/spec-dir" 2>/dev/null | grep -qx 'built=false' \
    || { echo "FAIL: el10 spec mismatch should skip"; fail=1; }
  # a pre-release tag needs the bumped spec as a release does; the weekly
  # rehearsal builds the spec at its made-up version (docker stubbed out:
  # reaching it means the checks passed)
  mkdir -p "$t/uppre" "$t/uprh"
  : > "$t/uppre/mod-pagespeed-2.5.0~pre.1-1.x86_64.rpm"
  : > "$t/uprh/mod-pagespeed-2.5.0~rehearsal.7-1.x86_64.rpm"
  printf 'MAJOR=2\nMINOR=5\nBUILD=0\nPRERELEASE=pre.1\nLASTCHANGE=1\n' > "$t/Vpre"
  printf 'MAJOR=2\nMINOR=5\nBUILD=0\nPRERELEASE=rehearsal.7\nLASTCHANGE=1\n' > "$t/Vrh"
  run --os el9 --upstream "$t/uppre" --version-file "$t/Vpre" --out "$t/out" --spec-dir "$t/spec-dir" >"$t/err" 2>&1 || true
  grep -qF "::error::%global mps_version 1.19.0" "$t/err" \
    || { echo "FAIL: pre-release spec mismatch should be an error: $(cat "$t/err")"; fail=1; }
  ( docker_pull_retry() { echo "docker reached" >&2; return 1; }
    run --os el9 --upstream "$t/uprh" --version-file "$t/Vrh" --out "$t/out" --spec-dir "$t/spec-dir" ) >"$t/err" 2>&1 || true
  if ! grep -qF "building the spec at 2.5.0" "$t/err" || ! grep -qx "docker reached" "$t/err"; then
    echo "FAIL: rehearsal spec mismatch should build at the rehearsal version: $(cat "$t/err")"; fail=1
  fi
  : > "$t/up/mod-pagespeed-1.19.0-2.x86_64.rpm"
  if run --os el9 --upstream "$t/up" --version-file "$t/V" --out "$t/out" >/dev/null 2>&1; then
    echo "FAIL: two stock RPMs accepted"; fail=1
  fi
  [ "$fail" = 0 ] && echo "release_ea4_build self-test: ok"
  return "$fail"
}

case "${1:-}" in
  --self-test) self_test ;;
  --*) run "$@" ;;
  *) echo "usage: $0 --os el8|el9|el10 --upstream DIR --version-file FILE --out DIR | --self-test" >&2; exit 2 ;;
esac
