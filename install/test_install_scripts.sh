#!/bin/bash
#
# Self-tests for install/shell_utils.sh and install/Makefile.
# Usage: bash install/test_install_scripts.sh

this_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
failures=0

check() {
  local name="$1"
  shift
  if "$@"; then
    echo "PASS: $name"
  else
    echo "FAIL: $name"
    failures=$((failures + 1))
  fi
}

# --- version_compare ---
source "$this_dir/shell_utils.sh"

test_version_compare_lt() {
  version_compare 1.9 -lt 2.0 &&
    ! version_compare 2.0 -lt 1.9 &&
    ! version_compare 2.0 -lt 2.0 &&
    ! version_compare 10.0 -lt 9.9 &&
    version_compare 9.9 -lt 10.0
}
test_version_compare_gt() {
  version_compare 2.0 -gt 1.9 &&
    ! version_compare 1.9 -gt 2.0 &&
    ! version_compare 2.0 -gt 2.0 &&
    version_compare 10.0 -gt 9.9 &&
    version_compare 1.2.4 -gt 1.2.3
}
test_version_compare_ge_le_eq() {
  version_compare 2.0 -ge 1.9 &&
    ! version_compare 1.9 -ge 2.0 &&
    version_compare 2.0 -ge 2.0 &&
    version_compare 1.9 -le 2.0 &&
    ! version_compare 2.0 -le 1.9 &&
    version_compare 2.0 -eq 2.0 &&
    ! version_compare 2.0 -eq 1.9
}
check version_compare_lt test_version_compare_lt
check version_compare_gt test_version_compare_gt
check version_compare_ge_le_eq test_version_compare_ge_le_eq

# --- Makefile staging paths ---
make_var() {
  make -s -C "$this_dir" echo_vars "${@:2}" 2>/dev/null |
    sed -n "s/^ *$1=//p"
}

test_staging_dir_unpredictable() {
  local a b
  a="$(make_var STAGING_DIR)"
  b="$(make_var STAGING_DIR)"
  [ -n "$a" ] && [ -n "$b" ] &&
    [ "$a" != "/tmp/mod_pagespeed.install" ] && [ "$a" != "$b" ]
}
test_staging_dir_overridable() {
  [ "$(make_var STAGING_DIR STAGING_DIR=/var/tmp/x)" = "/var/tmp/x" ]
}
test_no_fixed_tmp_sentinel() {
  ! grep -q '/tmp/semanage.set' "$this_dir/Makefile"
}
test_staging_dir_shared_with_submake() {
  local tmp out
  tmp="$(mktemp -d)"
  {
    echo "include $this_dir/Makefile"
    echo ".DEFAULT_GOAL := probe"
    echo "probe:"
    printf '\t@echo $(STAGING_DIR)\n'
    printf '\t@$(MAKE) -s -f %s inner\n' "$tmp/m.mk"
    echo "inner:"
    printf '\t@echo $(STAGING_DIR)\n'
  } > "$tmp/m.mk"
  out="$(make -s -C "$this_dir" -f "$tmp/m.mk" 2>/dev/null | sort -u | wc -l)"
  rm -rf "$tmp"
  [ "$out" -eq 1 ]
}
check staging_dir_unpredictable test_staging_dir_unpredictable
check staging_dir_shared_with_submake test_staging_dir_shared_with_submake
check staging_dir_overridable test_staging_dir_overridable
check no_fixed_tmp_sentinel test_no_fixed_tmp_sentinel

# --- Package signing keys must not be fetched over plain HTTP ---
test_rpm_key_fetched_over_https() {
  ! grep -n 'curl.*http://\|^ *http://.*RPM-GPG-KEY' \
      "$this_dir/centos/install_required_packages.sh" | grep -q 'GPG-KEY'
}
check rpm_key_fetched_over_https test_rpm_key_fetched_over_https

[ "$failures" -eq 0 ] || { echo "$failures test(s) failed"; exit 1; }
echo "All tests passed"
