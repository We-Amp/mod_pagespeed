#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
#
# run-container-test.sh -- install the current public nginx-module-pagespeed
# package in a throwaway ubuntu:24.04 container and run pagespeed-doctor (from
# this tree) against a working configuration and several broken ones,
# asserting the exit code, the JSON verdict and each check's code. Every run
# also asserts that the fake secrets in the configuration never appear in the
# output. It finally builds a .deb from this tree around the installed module
# and checks that /usr/sbin/pagespeed-doctor is registered and removed through
# update-alternatives.
#
# Nothing on the host is touched; the container is removed at the end.
#
# Usage: test/doctor/run-container-test.sh [image]
#   NAME=<container name>   default fwn-doctor-nginx
set -euo pipefail
IMAGE="${1:-ubuntu:24.04}"
NAME="${NAME:-fwn-doctor-nginx}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

docker rm -f "$NAME" >/dev/null 2>&1 || true
trap 'docker rm -f "$NAME" >/dev/null 2>&1 || true' EXIT

# CAP_SYS_ADMIN (and no AppArmor profile) lets the doctor shield the cache
# with a private mount namespace during the config test; the direct path is
# exercised too, by PAGESPEED_DOCTOR_NO_NAMESPACE=1.
docker run --rm -d --name "$NAME" --memory 1g --cap-add SYS_ADMIN \
  --security-opt apparmor=unconfined -v "$ROOT:/src:ro" "$IMAGE" sleep 3600 >/dev/null

docker exec -i "$NAME" bash -s <<'INNER'
set -u
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends curl ca-certificates jq binutils \
  >/dev/null 2>&1 || { echo "FAIL: apt prerequisites"; exit 1; }
curl -fsSL https://packages.modpagespeed.com/install.sh | sh >/dev/null 2>&1 \
  || { echo "FAIL: repository setup"; exit 1; }
apt-get install -y -qq nginx-module-pagespeed >/dev/null 2>&1 \
  || { echo "FAIL: install nginx-module-pagespeed"; exit 1; }
echo "installed: $(dpkg-query -W -f='${Package} ${Version}' nginx-module-pagespeed), nginx $(dpkg-query -W -f='${Version}' nginx-core 2>/dev/null || dpkg-query -W -f='${Version}' nginx)"

install -m 755 /src/install/common/pagespeed-doctor /usr/local/sbin/pagespeed-doctor
D=/usr/local/sbin/pagespeed-doctor
SECRET1="FAKEsecretKEY0123456789abcdefXYZ"
SECRET2="hunter2-fake-password"
fail=0

check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fail=1; fi; }

cat > /var/www/html/index.html <<'HTML'
<html><head><title>doctor test</title>
<link rel="stylesheet" href="/style.css">
</head><body><p>Hello</p><img src="/pixel.gif"></body></html>
HTML
printf 'body {  color: red;  }\n\n p { margin: 0 }\n' > /var/www/html/style.css
printf 'GIF89a\001\000\001\000\200\000\000\000\000\000\377\377\377!\371\004\001\000\000\000\000,\000\000\000\000\001\000\001\000\000\002\002D\001\000;' > /var/www/html/pixel.gif

# The statistics handler, restricted to localhost as the docs set it up, and
# resources served private (D05 must name that as the reason).
sed -i 's|^\(\s*server_name _;\)|\1\n\tlocation = /ngx_pagespeed_statistics { allow 127.0.0.1; allow ::1; deny all; }\n\tlocation ~ ^/cc\\. { add_header Cache-Control "private, max-age=600"; }|' \
  /etc/nginx/sites-enabled/default
sed -e 's|/style.css|/cc.css|' -e 's|/pixel.gif|/cc.gif|' /var/www/html/index.html > /var/www/html/cc.html
cp /var/www/html/style.css /var/www/html/cc.css; cp /var/www/html/pixel.gif /var/www/html/cc.gif

good_conf() {
  cat > /etc/nginx/conf.d/pagespeed.conf <<CONF
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed StatisticsPath /ngx_pagespeed_statistics;
pagespeed RedisPassword "$SECRET2";
CONF
  mkdir -p /var/cache/ngx_pagespeed
  chown -R www-data: /var/cache/ngx_pagespeed
}

# run NAME EXPECTED_EXIT [doctor args...] -> leaves $OUT (json) and $HUM
run() {
  local name="$1" want="$2"; shift 2
  OUT=$("$D" --json "$@" 2>/tmp/doctor.err); rc=$?
  HUM=$("$D" "$@" 2>&1)
  echo "== $name: exit $rc, verdict $(echo "$OUT" | jq -r .verdict 2>/dev/null)"
  echo "$HUM" | sed 's/^/    /'
  check "$name: exit code $want" "[[ $rc =~ ^($want)\$ ]]"
  check "$name: one valid JSON document" "echo \"\$OUT\" | jq -e '.schema == \"pagespeed-doctor/1\" and (.checks | length) == 6' >/dev/null"
  check "$name: JSON exit_code matches" "[ \"\$(echo \"\$OUT\" | jq .exit_code)\" = $rc ]"
  check "$name: no secret in JSON" "! echo \"\$OUT\" | grep -q -e '$SECRET1' -e '$SECRET2'"
  check "$name: no secret in text" "! echo \"\$HUM\" | grep -q -e '$SECRET1' -e '$SECRET2'"
}
code_of() { echo "$OUT" | jq -r --arg id "$1" '.checks[] | select(.id == $id) | "\(.status) \(.code)"'; }

# --- working --------------------------------------------------------------
good_conf
nginx -t >/dev/null 2>&1 || { echo "FAIL: good config does not pass nginx -t"; nginx -t; exit 1; }
chown -R www-data: /var/cache/ngx_pagespeed
nginx
for i in 1 2 3 4 5 6; do curl -s -o /dev/null http://127.0.0.1/; curl -s -o /dev/null http://127.0.0.1/style.css; sleep 1; done
# nginx -t and the nginx start above ran as root (mod_pagespeed#780); hand the
# cache back to the worker so the next assertion measures the doctor alone.
echo "root-owned cache files after nginx -t + start as root: $(find /var/cache/ngx_pagespeed -user 0 ! -type d | wc -l)"
find /var/cache/ngx_pagespeed -user 0 -exec chown www-data: {} +
echo "statistics: HTTP $(curl -s -o /dev/null -w '%{http_code}' 'http://127.0.0.1/ngx_pagespeed_statistics')"
# A build host's nearly full disk is a real D11 warning, not a test failure.
DISK_FULL=$(df -P /var/cache/ngx_pagespeed | awk 'NR == 2 { sub(/%/, "", $5); print ($5 > 90) ? 1 : 0 }')
WANT_OK=0; WANT_D11='pass null'
[ "$DISK_FULL" = 1 ] && { WANT_OK=1; WANT_D11='warn cache_disk_full'; echo "note: disk over 90% full here; expecting the D11 disk warning"; }
run working "$WANT_OK"
check "working: D02 pass" "[ \"\$(code_of D02)\" = 'pass null' ]"
check "working: D03 pass" "[ \"\$(code_of D03)\" = 'pass null' ]"
check "working: D04 pass" "[ \"\$(code_of D04)\" = 'pass null' ]"
check "working: D05 pass" "[ \"\$(code_of D05)\" = 'pass null' ]"
check "working: D11 $WANT_D11" "[ \"\$(code_of D11)\" = '$WANT_D11' ]"
check "working: D05 read the statistics" "echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D05\") | .message' | grep -q 'statistics at /ngx_pagespeed_statistics'"
check "working: config test shielded" "[ \"\$(echo \"\$OUT\" | jq -r .config.config_test)\" = namespace ]"
check "working: no root-owned file left by the doctor" "[ -z \"\$(find /var/cache/ngx_pagespeed -user 0 ! -type d)\" ]"

# --- resources served private: D05 names the cause ------------------------
for i in 1 2 3; do curl -s -o /dev/null http://127.0.0.1/cc.html; sleep 1; done
# A failure when the counter rose during the doctor's own requests, else a
# warning naming the same cause.
run private_resources '1|2' --path /cc.html
check "private: D05 names cache_control_not_rewritable" "[[ \"\$(code_of D05)\" =~ ^(fail|warn)' cache_control_not_rewritable'\$ ]]"

# --- secrets in a failing line -------------------------------------------
echo "pagespeed UrlSigningKey $SECRET1 extra;" >> /etc/nginx/conf.d/pagespeed.conf
run secret_in_failing_line 2
check "secret line: D03 directive_arguments" "[ \"\$(code_of D03)\" = 'fail directive_arguments' ]"
good_conf

# --- typo -----------------------------------------------------------------
echo "pagespeed FileCachePathh /tmp/x;" >> /etc/nginx/conf.d/pagespeed.conf
run typo 2
check "typo: D03 directive_typo" "[ \"\$(code_of D03)\" = 'fail directive_typo' ]"
check "typo: suggests FileCachePath" "echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | .message' | grep -q 'did you mean FileCachePath?'"
check "typo: evidence names the line" "[ \"\$(echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | .evidence[0].line')\" = 5 ]"
check "typo: D04 skipped" "[ \"\$(code_of D04)\" = 'skip null' ]"
good_conf

# --- removed option -------------------------------------------------------
echo "pagespeed FileCacheInodeLimit 500000;" >> /etc/nginx/conf.d/pagespeed.conf
OUT=$("$D" --json 2>/dev/null); rc=$?
echo "== removed_option: exit $rc, D03 $(code_of D03)"
check "removed: exit code 2 or 1" "[ $rc -eq 2 ] || [ $rc -eq 1 ]"
check "removed: removed (fails) or deprecated (warns)" \
  "[ \"\$(code_of D03)\" = 'fail directive_removed' ] || [ \"\$(code_of D03)\" = 'warn directive_deprecated' ]"
good_conf

# --- scope error ------------------------------------------------------------
sed -i 's|^\(\s*server_name _;\)|\1\n\tpagespeed UseNativeFetcher on;|' /etc/nginx/sites-enabled/default
run scope 2
check "scope: D03 directive_scope" "[ \"\$(code_of D03)\" = 'fail directive_scope' ]"
sed -i '/pagespeed UseNativeFetcher on;/d' /etc/nginx/sites-enabled/default

# --- module not loaded ----------------------------------------------------
mv /etc/nginx/modules-enabled/50-mod-pagespeed.conf /tmp/50-mod-pagespeed.conf
run not_loaded 2
check "not_loaded: D02 module_not_loaded" "[ \"\$(code_of D02)\" = 'fail module_not_loaded' ]"
check "not_loaded: D03 skipped" "[ \"\$(code_of D03)\" = 'skip null' ]"
mv /tmp/50-mod-pagespeed.conf /etc/nginx/modules-enabled/50-mod-pagespeed.conf

# --- loaded twice ---------------------------------------------------------
sed -i '1i load_module modules/ngx_pagespeed_module.so;' /etc/nginx/nginx.conf
run loaded_twice 2
check "loaded_twice: D02 module_loaded_twice" "[ \"\$(code_of D02)\" = 'fail module_loaded_twice' ]"
sed -i '1d' /etc/nginx/nginx.conf

# --- switched off ---------------------------------------------------------
sed -i 's/^pagespeed on;/pagespeed off;/' /etc/nginx/conf.d/pagespeed.conf
nginx -s reload; sleep 1
run switched_off 2
check "off: D04 module_off" "[ \"\$(code_of D04)\" = 'fail module_off' ]"
good_conf; nginx -s reload; sleep 1

# --- root-owned cache file, config test without the namespace --------------
touch /var/cache/ngx_pagespeed/root-owned.dat
export PAGESPEED_DOCTOR_NO_NAMESPACE=1
run root_owned 1
unset PAGESPEED_DOCTOR_NO_NAMESPACE
check "root_owned: D11 root_owned_cache_files" "[ \"\$(code_of D11)\" = 'warn root_owned_cache_files' ]"
check "root_owned: chown fix offered, marked mutating" \
  "echo \"\$OUT\" | jq -e '.checks[] | select(.id==\"D11\") | .fix.commands[0].mutates == true' >/dev/null"
check "root_owned: config test ran directly" "[ \"\$(echo \"\$OUT\" | jq -r .config.config_test)\" = direct ]"
rm -f /var/cache/ngx_pagespeed/root-owned.dat
find /var/cache/ngx_pagespeed -user 0 -exec chown www-data: {} +

# --- not writable ---------------------------------------------------------
chown root: /var/cache/ngx_pagespeed; chmod 755 /var/cache/ngx_pagespeed
run not_writable 2
check "not_writable: D11 cache_dir_not_writable" "[ \"\$(code_of D11)\" = 'fail cache_dir_not_writable' ]"
chown -R www-data: /var/cache/ngx_pagespeed

# --- server down ----------------------------------------------------------
nginx -s stop; sleep 1
run server_down 2
check "down: D04 server_not_reachable" "[ \"\$(code_of D04)\" = 'fail server_not_reachable' ]"

# --- bad flag -------------------------------------------------------------
"$D" --bogus >/dev/null 2>&1; rc=$?
check "bad flag exits 3" "[ $rc -eq 3 ]"

# --- packaging: build a .deb from this tree around the installed module ----
mkdir -p /tmp/pkg && cp /usr/lib/nginx/modules/ngx_pagespeed_module.so /tmp/pkg/
if bash /src/install/nginx/build_nginx_deb.sh -s /tmp/pkg/ngx_pagespeed_module.so -o /tmp/pkg >/tmp/pkg/build.log 2>&1; then
  dpkg -i /tmp/pkg/nginx-module-pagespeed_*.deb >/tmp/pkg/install.log 2>&1
  check "deb: /usr/sbin/pagespeed-doctor via alternatives" \
    "[ \"\$(readlink -f /usr/sbin/pagespeed-doctor)\" = /usr/share/nginx-module-pagespeed/pagespeed-doctor ]"
  check "deb: packaged doctor carries the version" \
    "! grep -q '^DOCTOR_VERSION=\"dev\"' /usr/share/nginx-module-pagespeed/pagespeed-doctor"
  /usr/sbin/pagespeed-doctor --json --no-configtest >/tmp/pkg/run.json 2>/dev/null
  check "deb: packaged doctor runs" "jq -e .verdict /tmp/pkg/run.json >/dev/null"
  dpkg -r nginx-module-pagespeed >/dev/null 2>&1
  check "deb: alternative removed with the package" "[ ! -e /usr/sbin/pagespeed-doctor ]"
else
  echo "FAIL: build_nginx_deb.sh"; tail -20 /tmp/pkg/build.log; fail=1
fi

echo
[ "$fail" = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit "$fail"
INNER
