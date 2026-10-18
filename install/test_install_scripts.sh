#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
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
    version_compare 9.9 -lt 10.0 &&
    version_compare 2.0 -lt 2.0.1
}
test_version_compare_gt() {
  version_compare 2.0 -gt 1.9 &&
    ! version_compare 1.9 -gt 2.0 &&
    ! version_compare 2.0 -gt 2.0 &&
    version_compare 10.0 -gt 9.9 &&
    version_compare 1.2.4 -gt 1.2.3 &&
    version_compare 2.0.1 -gt 2.0
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

test_staging_dir_stable_per_user() {
  local a b
  a="$(make_var STAGING_DIR)"
  b="$(make_var STAGING_DIR)"
  [ -n "$a" ] && [ "$a" = "$b" ] && [ "$a" != "/tmp/mod_pagespeed.install" ] &&
    [[ "$a" == *mod_pagespeed.install.* ]]
}
test_staging_dir_follows_sudo_user() {
  [ "$(SUDO_USER=alice USER=root make_var STAGING_DIR | sed 's|.*/||')" = \
    "mod_pagespeed.install.alice" ]
}
test_parse_creates_no_directories() {
  local tmp n
  tmp="$(mktemp -d)"
  TMPDIR="$tmp" make -s -C "$this_dir" echo_vars >/dev/null 2>&1
  n="$(find "$tmp" -mindepth 1 | wc -l)"
  rm -rf "$tmp"
  [ "$n" -eq 0 ]
}
test_staging_dir_mode() {
  local tmp d mode
  tmp="$(mktemp -d)"
  d="$(TMPDIR="$tmp" make_var STAGING_DIR)"
  # Staging recreates the directory with mode 711.
  TMPDIR="$tmp" make -s -C "$this_dir" setup_staging_dir >/dev/null 2>&1
  mode="$(stat -c %a "$d")"
  rm -rf "$tmp"
  [ "$mode" = "711" ]
}
test_install_requires_owned_staging_dir() {
  local tmp
  tmp="$(mktemp -d)"
  # Missing directory: refuse.
  ! make -s -C "$this_dir" check_staging_dir STAGING_DIR="$tmp/missing" >/dev/null 2>&1 &&
    # Directory owned by the invoking user: accept.
    make -s -C "$this_dir" check_staging_dir STAGING_DIR="$tmp" >/dev/null 2>&1 &&
    # Directory owned by a different user: refuse.
    ! SUDO_USER=nobody-such-user make -s -C "$this_dir" check_staging_dir \
        STAGING_DIR="$tmp" >/dev/null 2>&1
  local rc=$?
  rm -rf "$tmp"
  return $rc
}
check staging_dir_stable_per_user test_staging_dir_stable_per_user
check staging_dir_follows_sudo_user test_staging_dir_follows_sudo_user
check parse_creates_no_directories test_parse_creates_no_directories
check staging_dir_mode test_staging_dir_mode
check install_requires_owned_staging_dir test_install_requires_owned_staging_dir
test_staging_dir_overridable() {
  [ "$(make_var STAGING_DIR STAGING_DIR=/var/tmp/x)" = "/var/tmp/x" ]
}
test_no_fixed_tmp_sentinel() {
  ! grep -q '/tmp/semanage.set' "$this_dir/Makefile"
}
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
