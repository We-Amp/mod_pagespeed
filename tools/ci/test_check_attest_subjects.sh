#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test for tools/ci/check_attest_subjects.sh.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=check_attest_subjects.sh
. "$HERE/check_attest_subjects.sh"

fail=0
expect() { # expect ok|no <desc> <cmd...>
  local want="$1" desc="$2" got; shift 2
  if "$@" 2>/dev/null; then got=ok; else got=no; fi
  if [ "$got" = "$want" ]; then echo "ok   $desc"; else echo "FAIL $desc (wanted $want, got $got)"; fail=1; fi
}

expect ok "plain package"        attest_name_ok "pagespeed-1.1.deb"
expect ok "plus and underscore"  attest_name_ok "a_b+c-1.0.tar.gz"
expect ok "sbom name"            attest_name_ok "pagespeed-1.1.spdx.json"
expect no "empty"                attest_name_ok ""
expect no "leading dot"          attest_name_ok ".hidden"
expect no "leading dash"         attest_name_ok "-rf"
expect no "directory"            attest_name_ok "sub/file.deb"
expect no "traversal"            attest_name_ok "../file.deb"
expect no "dotdot inside"        attest_name_ok "a..b.deb"
expect no "space"                attest_name_ok "a b.deb"
expect no "trailing newline then junk" attest_name_ok $'good.deb\n../evil'
expect no "clean line first"     attest_name_ok $'good.deb\nevil'
expect no "clean line last"      attest_name_ok $'evil/x\ngood.deb'
expect no "carriage return"      attest_name_ok $'good.deb\r'

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
printf '%s\n' pkg.deb pagespeed.spdx.json other.spdx.json.bak SHA256SUMS > "$tmp/all.txt"
sbom() { printf '%s' "$1" > "$tmp/sbom.txt"; attest_sbom_ok "$tmp/sbom.txt" "$tmp/all.txt"; }
expect ok "sbom listed"              sbom $'pagespeed.spdx.json\n'
expect no "sbom suffix extra"        sbom $'other.spdx.json.bak\n'
expect no "sbom not spdx"            sbom $'pkg.deb\n'
expect no "sbom unlisted"            sbom $'unknown.spdx.json\n'
expect no "sbom prefix of listed"    sbom $'pagespeed.spdx\n'
expect no "sbom two lines"           sbom $'pagespeed.spdx.json\npkg.deb\n'
expect no "sbom empty"               sbom ''
expect no "sbom traversal"           sbom $'../pagespeed.spdx.json\n'
expect no "sbom missing file"        attest_sbom_ok "$tmp/none.txt" "$tmp/all.txt"
expect ok "cli name"                 "$HERE/check_attest_subjects.sh" name pkg.deb
expect no "cli bad name"             "$HERE/check_attest_subjects.sh" name ../pkg.deb

[ "$fail" = 0 ] && echo "all passed"
exit "$fail"
