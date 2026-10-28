#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test for assert_release_stamp.sh. Fake modules are small files that
# hold the string under test, packed the way the release assets are packed.
#   bash tools/ci/test_assert_release_stamp.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/assert_release_stamp.sh"
SHA="0123456789abcdef0123456789abcdef01234567"
OTHER="fedcba9876543210fedcba9876543210fedcba98"
VER="2.6.0-pre.1"
# What a stamped module holds: the fetch user agent suffix the code builds from
# MOD_PAGESPEED_FULL_VERSION_STRING and LASTCHANGE_STRING.
stamp() { printf 'prefix (mod_pagespeed/%s-%s) suffix' "$1" "${2:0:7}"; }
STAMP="$(stamp "$VER" "$SHA")"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail=0
n=0

# make_tarball <dir> <name> <content>
make_tarball() {
  local d="$1" name="$2" content="$3" t
  t="$(mktemp -d "${WORK}/pack.XXXXXX")"
  mkdir -p "${t}/ngx_pagespeed/modules"
  printf 'x%sx\n' "$content" > "${t}/ngx_pagespeed/modules/ngx_pagespeed.so"
  mkdir -p "$d"
  tar -czf "${d}/${name}" -C "$t" ngx_pagespeed
}

# make_deb <dir> <name> <content>
make_deb() {
  local d="$1" name="$2" content="$3" t
  t="$(mktemp -d "${WORK}/pack.XXXXXX")"
  mkdir -p "${t}/usr/lib/apache2/modules" "${t}/DEBIAN"
  printf 'x%sx\n' "$content" > "${t}/usr/lib/apache2/modules/mod_pagespeed_ap24.so"
  printf 'Package: mod-pagespeed\nVersion: 1.0\nArchitecture: amd64\nMaintainer: t <t@example.com>\nDescription: t\n' > "${t}/DEBIAN/control"
  mkdir -p "$d"
  dpkg-deb --build "$t" "${d}/${name}" >/dev/null
}

# make_rpm_like: rpm needs rpmbuild, so ea4/rpm cases use rpm2cpio only when
# rpmbuild exists.
make_rpm() {
  local d="$1" name="$2" content="$3" t
  t="$(mktemp -d "${WORK}/pack.XXXXXX")"
  mkdir -p "${t}/BUILD" "${t}/RPMS" "${t}/SPECS" "${t}/ROOT/opt"
  printf 'x%sx\n' "$content" > "${t}/ROOT/opt/mod_pagespeed.so"
  printf 'Name: ea-apache24-mod_pagespeed\nVersion: 1\nRelease: 1.el9\nSummary: t\nLicense: Apache-2.0\nBuildArch: noarch\n%%description\nt\n%%install\nmkdir -p %%{buildroot}/opt\ncp %s/ROOT/opt/mod_pagespeed.so %%{buildroot}/opt/\n%%files\n/opt/mod_pagespeed.so\n' "$t" > "${t}/SPECS/t.spec"
  rpmbuild --define "_topdir ${t}" -bb "${t}/SPECS/t.spec" >/dev/null 2>&1
  mkdir -p "$d"
  cp "${t}"/RPMS/*/*.rpm "${d}/${name}"
}

# expect <case> <want-rc> <want-substring-or-empty> <dir> <sha>
expect() {
  local name="$1" want="$2" sub="$3" dir="$4" sha="$5" out rc=0
  n=$((n + 1))
  out="$(bash "$SCRIPT" "$dir" "$sha" 2>&1)" || rc=$?
  if [ "$rc" -ne "$want" ]; then
    echo "FAIL $name: exit $rc, want $want"; printf '    %s\n' "${out//$'\n'/$'\n'    }"; fail=1; return
  fi
  if [ -n "$sub" ] && ! printf '%s\n' "$out" | grep -qF -- "$sub"; then
    echo "FAIL $name: output lacks '$sub'"; printf '    %s\n' "${out//$'\n'/$'\n'    }"; fail=1; return
  fi
  echo "ok $name"
}

d="${WORK}/c1"; make_tarball "$d" ngx_pagespeed-1.0-linux-x64.tar.gz "$STAMP"
expect stamped-tarball-passes 0 "stamp ok:" "$d" "$SHA"

if command -v dpkg-deb >/dev/null 2>&1; then
  d="${WORK}/c2"; make_deb "$d" mod-pagespeed_1.0-1_amd64.deb "$STAMP"
  expect stamped-deb-passes 0 "stamp ok:" "$d" "$SHA"
else
  echo "skip: no dpkg-deb"
fi
if ! command -v rpm2cpio >/dev/null 2>&1; then
  echo "skip: no rpm2cpio"
fi

d="${WORK}/c3"; make_tarball "$d" ngx_pagespeed-1.0-linux-x64.tar.gz "ci"
expect literal-ci-refused 1 "not stamped" "$d" "$SHA"

d="${WORK}/c4"; make_tarball "$d" ngx_pagespeed-1.0-linux-x64.tar.gz "$(stamp "$VER" "$OTHER")"
expect wrong-sha-refused 1 "not stamped" "$d" "$SHA"

d="${WORK}/c5"; make_tarball "$d" ngx_pagespeed-1.0-linux-x64.tar.gz "${SHA:0:7}"
expect short-sha-only-refused 1 "not stamped" "$d" "$SHA"

d="${WORK}/c6"; mkdir -p "$d"; echo "deadbeef  file" > "${d}/SHA256SUMS"
expect no-module-asset-refused 1 "no Linux module" "$d" "$SHA"

d="${WORK}/c7"
make_tarball "$d" ngx_pagespeed-1.0-linux-x64.tar.gz "$STAMP"
make_tarball "$d" ngx_pagespeed-1.0-linux-arm64.tar.gz "ci"
expect one-of-two-unstamped-refused 1 "not stamped: ngx_pagespeed-1.0-linux-arm64.tar.gz" "$d" "$SHA"

# Optimizer packages are not module assets: an unstamped optimizer deb beside
# a stamped module neither fails the run nor counts as a module asset.
d="${WORK}/c8"; make_tarball "$d" ngx_pagespeed-1.0-linux-x64.tar.gz "$STAMP"
printf 'not a real deb, never opened\n' > "${d}/pagespeed-optimizer_1.0-1_amd64.deb"
printf 'not a real rpm\n' > "${d}/pagespeed-optimizer-1.0-1.x86_64.rpm"
expect optimizer-deb-ignored 0 "stamp ok:" "$d" "$SHA"
n=$((n + 1))
if bash "$SCRIPT" "$d" "$SHA" 2>&1 | grep -q 'pagespeed-optimizer'; then
  echo "FAIL optimizer-deb-ignored: optimizer asset was counted"; fail=1
else
  echo "ok optimizer-deb-ignored (not counted)"
fi

# A bare 7-character match that is not in the user agent form is refused, as is
# the full hash alone, a stamp with another commit's short sha, and a stamp
# whose version is not the release version when --version names it.
d="${WORK}/c13"; make_tarball "$d" ngx_pagespeed-1.0-linux-x64.tar.gz "junk ${SHA:0:7} junk -${SHA:0:7} ${SHA:0:7})"
expect bare-sha7-not-ua-format-refused 1 "not stamped" "$d" "$SHA"
d="${WORK}/c14"; make_tarball "$d" ngx_pagespeed-1.0-linux-x64.tar.gz "$SHA"
expect full-hash-alone-refused 1 "not stamped" "$d" "$SHA"
d="${WORK}/c15"; make_tarball "$d" ngx_pagespeed-1.0-linux-x64.tar.gz "$(stamp "$VER" "$OTHER") (mod_pagespeed/${VER}-0000000)"
expect other-commit-short-sha-refused 1 "not stamped" "$d" "$SHA"
d="${WORK}/c16"; make_tarball "$d" ngx_pagespeed-1.0-linux-x64.tar.gz "$STAMP"
n=$((n + 1))
out="$(bash "$SCRIPT" --version "$VER" "$d" "$SHA" 2>&1)" && rc=0 || rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -qF 'stamp ok:'; then echo "ok version-pinned-passes"; else echo "FAIL version-pinned-passes: rc=$rc"; fail=1; fi
n=$((n + 1))
out="$(bash "$SCRIPT" --version 2.6.0 "$d" "$SHA" 2>&1)" && rc=0 || rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF 'not stamped'; then echo "ok wrong-version-refused"; else echo "FAIL wrong-version-refused: rc=$rc"; fail=1; fi
n=$((n + 1))
out="$(bash "$SCRIPT" --version 2x6x0-pre.1 "$d" "$SHA" 2>&1)" && rc=0 || rc=$?
if [ "$rc" -eq 1 ]; then echo "ok malformed-version-refused"; else echo "FAIL malformed-version-refused: rc=$rc"; fail=1; fi

# nginx module deb, unstamped
if command -v dpkg-deb >/dev/null 2>&1; then
  d="${WORK}/c9"; make_deb "$d" nginx-module-pagespeed_1.0-1.noble_amd64.deb "ci"
  expect nginx-module-deb-unstamped-refused 1 "not stamped: nginx-module-pagespeed_" "$d" "$SHA"
else
  echo "skip: no dpkg-deb"
fi

# ea4 rpm, stamped
if command -v rpm2cpio >/dev/null 2>&1 && command -v rpmbuild >/dev/null 2>&1; then
  d="${WORK}/c10"; make_rpm "$d" ea-apache24-mod_pagespeed-1-1.el9.noarch.rpm "$STAMP"
  expect ea4-rpm-stamped-passes 0 "stamp ok: ea-apache24" "$d" "$SHA"
else
  if command -v rpm2cpio >/dev/null 2>&1; then echo "skip: no rpmbuild"; else echo "skip: no rpm2cpio"; fi
fi

# Family check: --require-families names a missing family.
n=$((n + 1))
out="$(bash "$SCRIPT" --require-families "${WORK}/c1" "$SHA" 2>&1)" && rc=0 || rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF 'no apache deb asset'; then
  echo "ok missing-family-refused"
else
  echo "FAIL missing-family-refused: rc=$rc"; fail=1
fi
# A pre-release does not require the cPanel family, but every other family
# is still required.
n=$((n + 1))
out="$(bash "$SCRIPT" --require-families --prerelease "${WORK}/c1" "$SHA" 2>&1)" || true
ok=1
printf '%s' "$out" | grep -qF 'no ea4 rpm' && ok=0
for fam in "apache deb" "apache rpm" "apache rpm el10" "apache module so" "nginx deb" "nginx rpm"; do
  printf '%s' "$out" | grep -qF "no ${fam} asset" || ok=0
done
if [ "$ok" -eq 1 ]; then echo "ok prerelease-ea4-not-required"; else echo "FAIL prerelease-ea4-not-required"; fail=1; fi

# Standalone module .so assets are read as they are.
d="${WORK}/c11"; mkdir -p "$d"; printf 'x%sx\n' "$STAMP" > "${d}/libmod_pagespeed-linux-x86_64.so"
expect standalone-so-stamped-passes 0 "stamp ok: libmod_pagespeed-linux-x86_64.so" "$d" "$SHA"
d="${WORK}/c12"; mkdir -p "$d"; printf 'xcix\n' > "${d}/libmod_pagespeed-linux-aarch64.so"
expect standalone-so-unstamped-refused 1 "not stamped: libmod_pagespeed-linux-aarch64.so" "$d" "$SHA"

# The family globs agree with the names in the manifest: every pattern
# fragment is in the manifest and every family glob is in the script.
n=$((n + 1))
agree=1
while IFS='|' read -r frag glob; do
  grep -qF -- "$frag" "${HERE}/release_manifest.py" || { echo "FAIL manifest-agrees: '$frag' not in release_manifest.py"; agree=0; fail=1; }
  grep -qF -- "|${glob}|" "$SCRIPT" || { echo "FAIL manifest-agrees: '$glob' not in $SCRIPT"; agree=0; fail=1; }
done <<'LIST'
mod-pagespeed_%s|mod-pagespeed_*.deb
mod-pagespeed-%s|mod-pagespeed-*.rpm
"el10-"|el10-mod-pagespeed-*.rpm
libmod_pagespeed-linux-%s|libmod_pagespeed-linux-*.so
ngx_pagespeed-|ngx_pagespeed-*.tar.gz
nginx-module-pagespeed_%s|nginx-module-pagespeed_*.deb
nginx-module-pagespeed-%s|nginx-module-pagespeed-*.rpm
ea-apache24-mod_pagespeed-|ea-apache24-mod_pagespeed-*.rpm
LIST
[ "$agree" -eq 1 ] && echo "ok manifest-agrees"

# A malformed SHA argument is refused too (fail closed).
expect malformed-sha-refused 1 "40" "${WORK}/c1" "abc"

if [ "$fail" -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "all $n cases passed"
