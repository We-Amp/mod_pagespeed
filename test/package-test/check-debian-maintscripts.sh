#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# check-debian-maintscripts.sh -- the module package's postinst/prerm respect
# an administrator's a2dismod / a2disconf and exit 0 on every path (#916).
#
# Runs install/debian/{postinst,prerm} against a real Apache's a2en*/a2dis*
# tooling in a throwaway ubuntu container (default ubuntu:24.04); nothing on
# the host is touched. No package build is needed: the scripts are driven
# directly with the arguments dpkg passes.
#
# Usage: test/package-test/check-debian-maintscripts.sh [image]
set -euo pipefail
IMAGE="${1:-ubuntu:24.04}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

docker run --rm -v "$ROOT:/src:ro" "$IMAGE" bash -c '
set -u
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq apache2 >/dev/null 2>&1
A=/etc/apache2
# Stand in for the package payload.
echo "LoadModule pagespeed_module /bin/true" > $A/mods-available/pagespeed.load
: > $A/mods-available/pagespeed.conf
: > $A/conf-available/pagespeed_libraries.conf
cp /src/install/common/pagespeed_daemon.conf $A/conf-available/
fail=0
check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fail=1; fi; }
enabled() { [ -e $A/conf-enabled/$1.conf ]; }
run() { out=$(sh /src/install/debian/$1 "${@:2}" 2>&1); rc=$?; echo "$out" | grep -v "^$" | sed "s/^/    /"; return $rc; }

echo "== first install"
run postinst configure; check "postinst exits 0" "[ $? -eq 0 ]"
check "module enabled"          "[ -e $A/mods-enabled/pagespeed.load ]"
check "libraries conf enabled"  "enabled pagespeed_libraries"
check "daemon conf enabled"     "enabled pagespeed_daemon"

echo "== admin disables everything, then upgrade / reconfigure"
a2disconf pagespeed_daemon >/dev/null; a2disconf pagespeed_libraries >/dev/null; a2dismod pagespeed >/dev/null
run postinst configure 1.17.0-r1; rc=$?
check "upgrade postinst exits 0" "[ $rc -eq 0 ]"
check "daemon conf stays disabled"     "! enabled pagespeed_daemon"
check "libraries conf stays disabled"  "! enabled pagespeed_libraries"
check "module stays disabled"          "[ ! -e $A/mods-enabled/pagespeed.load ]"
run postinst configure 1.17.0-r1 >/dev/null

echo "== upgrade from a release without the daemon drop-in enables it once"
run postinst configure 1.15.0-r22; rc=$?
check "postinst exits 0" "[ $rc -eq 0 ]"
check "daemon conf enabled"            "enabled pagespeed_daemon"
check "libraries conf stays disabled"  "! enabled pagespeed_libraries"

echo "== remove, then reinstall (dpkg passes the OLD version: the package has conffiles)"
a2enmod pagespeed >/dev/null; a2enconf pagespeed_libraries pagespeed_daemon >/dev/null
a2disconf pagespeed_libraries >/dev/null   # an admin choice that must survive the remove
run prerm remove; rc=$?
check "prerm exits 0" "[ $rc -eq 0 ]"
run postinst configure 1.17.0-r1 >/dev/null
check "module re-enabled"             "[ -e $A/mods-enabled/pagespeed.load ]"
check "daemon conf re-enabled"        "enabled pagespeed_daemon"
check "admin-disabled conf stays off" "! enabled pagespeed_libraries"

echo "== real dpkg supplies the arguments (stub package built with dpkg-deb)"
for v in 1 2; do
  S=/tmp/stub$v; rm -rf $S; mkdir -p $S/DEBIAN $S$A/mods-available $S$A/conf-available
  printf "Package: mod-pagespeed\nVersion: 1.17.0-r$v\nArchitecture: all\nMaintainer: t <t@example.com>\nDescription: stub\n" > $S/DEBIAN/control
  echo $A/mods-available/pagespeed.conf > $S/DEBIAN/conffiles
  cp /src/install/debian/postinst /src/install/debian/prerm /src/install/debian/postrm $S/DEBIAN/
  cp $A/mods-available/pagespeed.load $A/mods-available/pagespeed.conf $S$A/mods-available/
  cp $A/conf-available/pagespeed_libraries.conf $A/conf-available/pagespeed_daemon.conf $S$A/conf-available/
  dpkg-deb -Zgzip -b $S /tmp/stub$v.deb >/dev/null
done
dpkg -P mod-pagespeed >/dev/null 2>&1
for c in pagespeed_libraries pagespeed_daemon; do a2disconf -p -q $c >/dev/null 2>&1; done; a2dismod -p -q pagespeed >/dev/null 2>&1
dpkg -i /tmp/stub1.deb >/dev/null 2>&1; check "dpkg first install exits 0" "[ $? -eq 0 ]"
check "first install enables all" "enabled pagespeed_daemon && enabled pagespeed_libraries && [ -e $A/mods-enabled/pagespeed.load ]"
a2disconf pagespeed_daemon >/dev/null
dpkg -i /tmp/stub2.deb >/dev/null 2>&1; check "dpkg upgrade exits 0" "[ $? -eq 0 ]"
check "upgrade keeps the admin-disabled daemon conf off" "! enabled pagespeed_daemon"
a2enconf pagespeed_daemon >/dev/null   # the admin re-enables it before removing
dpkg -r mod-pagespeed >/dev/null 2>&1; check "dpkg remove exits 0" "[ $? -eq 0 ]"
dpkg -i /tmp/stub2.deb >/dev/null 2>&1
check "reinstall after remove re-enables the module and confs" "enabled pagespeed_daemon && enabled pagespeed_libraries && [ -e $A/mods-enabled/pagespeed.load ]"
dpkg -P mod-pagespeed >/dev/null 2>&1; check "dpkg purge exits 0" "[ $? -eq 0 ]"
check "purge leaves no pagespeed state under /var/lib/apache2" "! find /var/lib/apache2 -name \"pagespeed*\" | grep -q ."
dpkg -i /tmp/stub2.deb >/dev/null 2>&1
check "reinstall after purge enables all" "enabled pagespeed_daemon && enabled pagespeed_libraries && [ -e $A/mods-enabled/pagespeed.load ]"
check "dpkg --audit clean" "[ -z \"\$(dpkg --audit)\" ]"

exit $fail
'
