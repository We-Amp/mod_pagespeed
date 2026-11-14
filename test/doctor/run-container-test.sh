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
ANAME="${ANAME:-fwn-doctor-apache}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

docker rm -f "$NAME" "$ANAME" >/dev/null 2>&1 || true
trap 'docker rm -f "$NAME" "$ANAME" >/dev/null 2>&1 || true' EXIT

# CAP_SYS_ADMIN (and no AppArmor profile) lets the doctor shield the cache
# with a private mount namespace during the config test; the direct path is
# exercised too, by PAGESPEED_DOCTOR_NO_NAMESPACE=1.
docker run --rm -d --name "$NAME" --memory 1g --cap-add SYS_ADMIN \
  --security-opt apparmor=unconfined -v "$ROOT:/src:ro" "$IMAGE" sleep 3600 >/dev/null

rc_nginx=0
docker exec -i "$NAME" bash -s <<'INNER' || rc_nginx=$?
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

check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fail=1; return 1; fi; }

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
  # Per-server caches: a custom path, one nested inside it, and one whose
  # name carries a quote and a command substitution (must never run).
  cat > /etc/nginx/conf.d/psc-servers.conf <<'CONF'
server { listen 8081; root /var/www/html; pagespeed FileCachePath /srv/psc/custom; }
server { listen 8082; root /var/www/html; pagespeed FileCachePath /srv/psc/custom/nested; }
server { listen 8083; root /var/www/html; pagespeed FileCachePath "/srv/psc/it's'$(touch /tmp/pwned)"; }
CONF
  mkdir -p /var/cache/ngx_pagespeed
  chown -R www-data: /var/cache/ngx_pagespeed
}
give_back() { # hand every cache tree back to the worker user
  find /var/cache/ngx_pagespeed /srv/psc -user 0 -exec chown www-data: {} + 2>/dev/null
}
root_files() { find /var/cache/ngx_pagespeed /srv/psc -user 0 ! -type d 2>/dev/null | wc -l; }

# run NAME EXPECTED_EXIT [doctor args...] -> leaves $OUT (json) and $HUM
run() {
  local name="$1" want="$2"; shift 2
  OUT=$("$D" --json "$@" 2>/tmp/doctor.err); rc=$?
  HUM=$("$D" "$@" 2>&1)
  echo "== $name: exit $rc, verdict $(echo "$OUT" | jq -r .verdict 2>/dev/null)"
  echo "$HUM" | sed 's/^/    /'
  check "$name: exit code $want" "[[ $rc =~ ^($want)\$ ]]" || \
    echo "$OUT" | jq -r '.checks[] | "    json: \(.id) \(.status) \(.code) \(.message)"' 2>/dev/null
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
echo "root-owned cache files after nginx -t + start as root: $(root_files) ($(find /srv/psc -user 0 ! -type d 2>/dev/null | wc -l) under /srv/psc)"
give_back
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
check "working: no root-owned file left by the doctor (all 4 cache paths)" "[ \"\$(root_files)\" = 0 ]"
check "working: D11 checked all 4 cache paths" "[ \"\$(echo \"\$OUT\" | jq '.checks[] | select(.id==\"D11\") | .evidence | length')\" = 4 ]"
check "working: D11 says shielded" "echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D11\") | .message' | grep -q 'shielded'"
check "working: the quote/substitution path never ran" "[ ! -e /tmp/pwned ]"
check "working: no printed command carries an unquoted path" "! echo \"\$HUM\" | grep -q 'chown -R [^ ]* /srv'"

# --- no private namespace: fail closed ---------------------------------------
give_back
export PAGESPEED_DOCTOR_NO_NAMESPACE=1
run not_isolated 1
unset PAGESPEED_DOCTOR_NO_NAMESPACE
check "not_isolated: D03 skipped configtest_not_isolated" "[ \"\$(code_of D03)\" = 'skip configtest_not_isolated' ]"
check "not_isolated: names --allow-cache-writes" "echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | .reason' | grep -q -- '--allow-cache-writes'"
check "not_isolated: config test not run, no root-owned file" "[ \"\$(root_files)\" = 0 ]"
check "not_isolated: verdict raised to working_with_warnings" "[ \"\$(echo \"\$OUT\" | jq -r .verdict)\" = working_with_warnings ]"
check "not_isolated: summary note names the skip" "echo \"\$OUT\" | jq -r '.summary.notes[0]' | grep -q '^config test skipped: cannot isolate'"
check "not_isolated: text summary line" "echo \"\$HUM\" | grep -q '^config test skipped: cannot isolate'"

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
check "scope: hint says http {} only" "echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | .fix.hint' | grep -q 'http {} block only'"
sed -i '/pagespeed UseNativeFetcher on;/d' /etc/nginx/sites-enabled/default

# --- top-level-only option inside a server block ------------------------------
sed -i 's|^\(\s*server_name _;\)|\1\n\tpagespeed ProcessScriptVariables on;|' /etc/nginx/sites-enabled/default
run top_level_only 2
check "top_level_only: D03 directive_scope" "[ \"\$(code_of D03)\" = 'fail directive_scope' ]"
sed -i '/pagespeed ProcessScriptVariables on;/d' /etc/nginx/sites-enabled/default

# --- other config errors: class and file:line only, never the text -----------
echo "add_header X-Leak \"$SECRET1\" a b c;" > /etc/nginx/conf.d/zz-leak.conf
run generic_wrong_arguments 2
check "generic: config_test_failed / wrong_arguments / add_header" \
  "[ \"\$(echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | \"\\(.code) \\(.evidence[0].error_class) \\(.evidence[0].directive)\"')\" = 'config_test_failed wrong_arguments add_header' ]"
echo "$SECRET1 on;" > /etc/nginx/conf.d/zz-leak.conf
run generic_unknown_directive 2
check "generic: unknown_directive, directive not echoed" \
  "[ \"\$(echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | \"\\(.evidence[0].error_class) \\(.evidence[0].directive)\"')\" = 'unknown_directive null' ]"
rm -f /etc/nginx/conf.d/zz-leak.conf

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
# Re-warm after the reload, so D05 sees optimized URLs again.
for i in 1 2 3 4; do curl -s -o /dev/null http://127.0.0.1/; curl -s -o /dev/null http://127.0.0.1/style.css; sleep 1; done

# --- root-owned cache file, config test without the namespace --------------
touch /var/cache/ngx_pagespeed/root-owned.dat
export PAGESPEED_DOCTOR_NO_NAMESPACE=1
run root_owned 1 --allow-cache-writes
unset PAGESPEED_DOCTOR_NO_NAMESPACE
check "root_owned: D11 root_owned_cache_files" "[ \"\$(code_of D11)\" = 'warn root_owned_cache_files' ]"
check "root_owned: chown fix offered, marked mutating" \
  "echo \"\$OUT\" | jq -e '.checks[] | select(.id==\"D11\") | .fix.commands[0].mutates == true' >/dev/null"
check "root_owned: config test ran directly" "[ \"\$(echo \"\$OUT\" | jq -r .config.config_test)\" = direct ]"
rm -f /var/cache/ngx_pagespeed/root-owned.dat
give_back

# --- not writable: the directory whose name carries a quote ----------------
MAL="/srv/psc/it's'\$(touch /tmp/pwned)"
chown root: "$MAL"; chmod 755 "$MAL"
run not_writable 2
check "not_writable: D11 cache_dir_not_writable" "[ \"\$(code_of D11)\" = 'fail cache_dir_not_writable' ]"
# The printed fix must be one quoted command: evaluating it with printf in
# place of chown gives exactly the user and the path, and runs nothing.
CMD=$(echo "$OUT" | jq -r '.checks[] | select(.id=="D11") | .fix.commands[0].command')
echo "    fix command: $CMD"
ARGS=$(bash -c "set -- ${CMD#chown -R }; printf '%s\n' \"\$#\" \"\$1\" \"\$2\"")
check "not_writable: fix command quotes the path" "[ \"\$ARGS\" = \"\$(printf '2\nwww-data:\n%s' \"\$MAL\")\" ]"
check "not_writable: nothing ran from the path" "[ ! -e /tmp/pwned ]"
chown -R www-data: "$MAL"

# --- module loaded, no pagespeed configuration --------------------------
# The packaged default is the shield; while it does not exist the test
# fails closed with a hint, once it exists the test runs shielded.
mkdir -p /tmp/held && mv /etc/nginx/conf.d/pagespeed.conf /etc/nginx/conf.d/psc-servers.conf /tmp/held/
rm -rf /var/cache/ngx_pagespeed.held; mv /var/cache/ngx_pagespeed /var/cache/ngx_pagespeed.held
run loaded_unconfigured_nodir '1|2'
check "loaded_unconfigured, default missing: clean skip with a create hint" \
  "[ \"\$(code_of D03)\" = 'skip configtest_not_isolated' ] && echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | .fix.hint' | grep -q '^Create /var/cache/ngx_pagespeed first'"
check "loaded_unconfigured: the default cache was not created" "[ ! -e /var/cache/ngx_pagespeed ]"
mkdir -p /var/cache/ngx_pagespeed && chown www-data: /var/cache/ngx_pagespeed
run loaded_unconfigured '0|1|2'
check "loaded_unconfigured, default present: D03 ran shielded and passed" \
  "[ \"\$(code_of D03)\" = 'pass null' ] && [ \"\$(echo \"\$OUT\" | jq -r .config.config_test)\" = namespace ]"
check "loaded_unconfigured: no root-owned file in the default cache" "[ -z \"\$(find /var/cache/ngx_pagespeed -user 0 ! -type d)\" ]"
rm -rf /var/cache/ngx_pagespeed

# --- pagespeed configured without any FileCachePath: fail closed ------------
echo "pagespeed on;" > /etc/nginx/conf.d/pagespeed.conf
run no_cache_path '1|2'
check "no_cache_path: verdict is not working" "[ \"\$(echo \"\$OUT\" | jq -r .verdict)\" != working ]"
check "no_cache_path: D03 skipped configtest_not_isolated" "[ \"\$(code_of D03)\" = 'skip configtest_not_isolated' ]"
check "no_cache_path: reason names the missing FileCachePath" \
  "echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | .reason' | grep -q 'no FileCachePath'"
check "no_cache_path: the default cache was not created" "[ ! -e /var/cache/ngx_pagespeed ]"
run no_cache_path_allowed '0|1|2' --allow-cache-writes
check "no_cache_path --allow-cache-writes: the test runs" "[ \"\$(code_of D03 | cut -d' ' -f1)\" != skip ]"
rm -f /etc/nginx/conf.d/pagespeed.conf; rm -rf /var/cache/ngx_pagespeed

# --- module not loaded at all: the only unshielded run ---------------------
mv /etc/nginx/modules-enabled/50-mod-pagespeed.conf /tmp/held/
run module_absent 2
check "module_absent: config test ran unshielded (module not loaded)" \
  "[ \"\$(echo \"\$OUT\" | jq -r .config.config_test)\" = module_not_loaded ]"
check "module_absent: D02 module_not_loaded" "[ \"\$(code_of D02)\" = 'fail module_not_loaded' ]"
mv /tmp/held/50-mod-pagespeed.conf /etc/nginx/modules-enabled/
mv /tmp/held/pagespeed.conf /tmp/held/psc-servers.conf /etc/nginx/conf.d/
mv /var/cache/ngx_pagespeed.held /var/cache/ngx_pagespeed

# --- --config (nginx -c), relative includes ---------------------------------
rm -rf /etc/nginx-alt && cp -a /etc/nginx /etc/nginx-alt && mkdir -p /etc/nginx-alt/alt.d
sed -i 's|include /etc/nginx/conf.d/\*.conf;|include alt.d/*.conf;|' /etc/nginx-alt/nginx.conf
cat > /etc/nginx-alt/alt.d/ps.conf <<'CONF'
pagespeed on;
pagespeed FileCachePath /srv/psc/altcache;
server { listen 8090; root /var/www/html; pagespeed FileCachePath /srv/newtop/cache; }
CONF
rm -rf /srv/psc/altcache /srv/newtop
run alt_config_nodir '1|2' --config /etc/nginx-alt/nginx.conf
check "alt_config, caches missing: clean skip with a create hint" \
  "[ \"\$(code_of D03)\" = 'skip configtest_not_isolated' ] && echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | .fix.hint' | grep -q '^Create /srv/'"
check "alt_config: the missing caches were not created" "[ ! -e /srv/psc/altcache ] && [ ! -e /srv/newtop ]"
mkdir -p /srv/psc/altcache /srv/newtop/cache && chown -R www-data: /srv/psc/altcache /srv/newtop
run alt_config '1|2' --config /etc/nginx-alt/nginx.conf
check "alt_config: D03 passed, shielded" \
  "[ \"\$(code_of D03)\" = 'pass null' ] && [ \"\$(echo \"\$OUT\" | jq -r .config.config_test)\" = namespace ]"
check "alt_config: D11 saw the cache paths of the -c configuration" \
  "echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D11\") | .evidence[].path' | grep -qx /srv/psc/altcache"
check "alt_config: shield verified against nginx -T" "[ \"\$(echo \"\$OUT\" | jq -r .config.shield_verified)\" = true ]"
check "alt_config: no root-owned file in the -c caches" "[ -z \"\$(find /srv/psc/altcache /srv/newtop -user 0 ! -type d)\" ]"
rm -rf /etc/nginx-alt /srv/newtop

# --- reproduction 1: a big log next to a missing cache directory ----------
# The cache directory's parent (/var) holds a 200 MB log nginx -T opens for
# append. The doctor must not cover the parent: a clean, quick skip.
mkdir -p /var/log/psc && head -c 200M /dev/zero > /var/log/psc/big.log
cat > /etc/nginx/conf.d/zz-big.conf <<'CONF'
access_log /var/log/psc/big.log;
server { listen 8091; root /var/www/html; pagespeed FileCachePath /var/ngx_pagespeed_cache; }
CONF
rm -rf /var/ngx_pagespeed_cache
t0=$(date +%s)
run big_log '1|2'
t1=$(date +%s)
check "big_log: clean skip, not a failure" "[ \"\$(code_of D03)\" = 'skip configtest_not_isolated' ]"
check "big_log: hint says create /var/ngx_pagespeed_cache" \
  "echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | .fix.hint' | grep -q '^Create /var/ngx_pagespeed_cache first'"
check "big_log: both doctor runs took under 30 s" "[ \$((t1 - t0)) -lt 30 ]"
check "big_log: the missing cache was not created" "[ ! -e /var/ngx_pagespeed_cache ]"
rm -f /etc/nginx/conf.d/zz-big.conf; rm -rf /var/log/psc

# --- reproduction 2: an include on a submount next to a missing cache ------
mkdir -p /srv/psc/mnt && mount -t tmpfs -o size=1m tmpfs /srv/psc/mnt
echo 'map $host $psc_from_mount { default 1; }' > /srv/psc/mnt/extra.conf
cat > /etc/nginx/conf.d/zz-mnt.conf <<'CONF'
include /srv/psc/mnt/extra.conf;
server { listen 8092; root /var/www/html; pagespeed FileCachePath /srv/psc/mntcache; }
CONF
rm -rf /srv/psc/mntcache
run submount_nodir '1|2'
check "submount, cache missing: clean skip, not a failure" "[ \"\$(code_of D03)\" = 'skip configtest_not_isolated' ]"
check "submount: the missing cache was not created" "[ ! -e /srv/psc/mntcache ]"
mkdir -p /srv/psc/mntcache && chown www-data: /srv/psc/mntcache
run submount '0|1|2'
check "submount, cache present: D03 passes shielded (the include on the mount is read)" \
  "[ \"\$(code_of D03)\" = 'pass null' ] && [ \"\$(echo \"\$OUT\" | jq -r .config.config_test)\" = namespace ]"
rm -f /etc/nginx/conf.d/zz-mnt.conf; umount /srv/psc/mnt; rm -rf /srv/psc/mnt /srv/psc/mntcache

# --- server down ----------------------------------------------------------
nginx -s stop; sleep 1
run server_down 2
check "down: D04 server_not_reachable" "[ \"\$(code_of D04)\" = 'fail server_not_reachable' ]"

# --- bad flag -------------------------------------------------------------
"$D" --bogus >/dev/null 2>&1; rc=$?
check "bad flag exits 3" "[ $rc -eq 3 ]"
OUT=$("$D" --bogus --json 2>/dev/null); rc=$?
check "bad flag with --json: exit 3 and a doctor_error document" \
  "[ $rc -eq 3 ] && echo \"\$OUT\" | jq -e '.verdict == \"doctor_error\" and .exit_code == 3' >/dev/null"
OUT=$("$D" --json --server bogus 2>/dev/null); rc=$?
check "bad --server with --json: exit 3 and JSON" "[ $rc -eq 3 ] && echo \"\$OUT\" | jq -e .error >/dev/null"

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
docker rm -f "$NAME" >/dev/null 2>&1 || true

# ===========================================================================
# Apache: ServerRoot, a directory include with a subdirectory, ${VAR}, and
# the shield checked against DUMP_INCLUDES.
docker run --rm -d --name "$ANAME" --memory 1g --cap-add SYS_ADMIN \
  --security-opt apparmor=unconfined -v "$ROOT:/src:ro" "$IMAGE" sleep 3600 >/dev/null
rc_apache=0
docker exec -i "$ANAME" bash -s <<'INNER' || rc_apache=$?
set -u
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends curl ca-certificates jq \
  >/dev/null 2>&1 || { echo "FAIL: apt prerequisites"; exit 1; }
curl -fsSL https://packages.modpagespeed.com/install.sh | sh >/dev/null 2>&1 \
  || { echo "FAIL: repository setup"; exit 1; }
apt-get install -y -qq mod-pagespeed >/dev/null 2>&1 \
  || { echo "FAIL: install mod-pagespeed"; exit 1; }
echo "installed: $(dpkg-query -W -f='${Package} ${Version}' mod-pagespeed), apache2 $(dpkg-query -W -f='${Version}' apache2)"
install -m 755 /src/install/common/pagespeed-doctor /usr/local/sbin/pagespeed-doctor
D=/usr/local/sbin/pagespeed-doctor
fail=0
check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fail=1; fi; }
run() {
  local name="$1"; shift
  OUT=$("$D" --json "$@" 2>/dev/null); rc=$?
  echo "== $name: exit $rc, verdict $(echo "$OUT" | jq -r .verdict 2>/dev/null)"
  "$D" "$@" 2>&1 | sed 's/^/    /'
  check "$name: one valid JSON document" "echo \"\$OUT\" | jq -e '.schema == \"pagespeed-doctor/1\"' >/dev/null"
}
code_of() { echo "$OUT" | jq -r --arg id "$1" '.checks[] | select(.id == $id) | "\(.status) \(.code)"'; }
d11_has() { echo "$OUT" | jq -r '.checks[] | select(.id=="D11") | .evidence[].path' | grep -qxF -- "$1"; }
mkdir -p /srv/psc
apache2ctl start >/dev/null 2>&1; sleep 2
find /var/cache/mod_pagespeed -user 0 -exec chown www-data: {} + 2>/dev/null

# The optimizer worker (not started here: no systemd) creates its volume
# directory on first start. Until then the doctor cannot shield it.
rm -rf /var/cache/pagespeed-optimizer/v3
run apache_volume_missing
check "apache, worker volume missing: clean skip with a create hint" \
  "[ \"\$(code_of D03)\" = 'skip configtest_not_isolated' ] && echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | .fix.hint' | grep -q '^Create /var/cache/pagespeed-optimizer/v3 first'"
mkdir -p /var/cache/pagespeed-optimizer/v3
chown pagespeed:pagespeed /var/cache/pagespeed-optimizer/v3 2>/dev/null || true

run apache_default
check "apache: D02 pass" "[ \"\$(code_of D02)\" = 'pass null' ]"
check "apache: D03 ran shielded" "[ \"\$(echo \"\$OUT\" | jq -r .config.config_test)\" = namespace ]"
check "apache: D03 pass" "[ \"\$(code_of D03 | cut -d' ' -f1)\" = pass ] || [ \"\$(code_of D03 | cut -d' ' -f1)\" = warn ]"
check "apache: shield verified against DUMP_INCLUDES" "[ \"\$(echo \"\$OUT\" | jq -r .config.shield_verified)\" = true ]"
check "apache: D04 pass (X-Mod-Pagespeed)" "[ \"\$(code_of D04)\" = 'pass null' ]"
check "apache: no root-owned file in the cache" "[ -z \"\$(find /var/cache/mod_pagespeed -user 0 ! -type d)\" ]"

# A directory include (relative to ServerRoot) whose file sits in a subdirectory.
mkdir -p /etc/apache2/psc.d/sub
echo "ModPagespeedFileCachePath /srv/psc/sub-cache" > /etc/apache2/psc.d/sub/cache.conf
echo "IncludeOptional psc.d" >> /etc/apache2/apache2.conf
mkdir -p /srv/psc/sub-cache && chown www-data: /srv/psc/sub-cache
run apache_subdir
check "subdir: D11 saw the cache path in the subdirectory" "d11_has /srv/psc/sub-cache"
check "subdir: D03 ran shielded" "[ \"\$(echo \"\$OUT\" | jq -r .config.config_test)\" = namespace ]"
check "subdir: no root-owned file in the cache" "[ -z \"\$(find /srv/psc/sub-cache -user 0 ! -type d)\" ]"
sed -i '/^IncludeOptional psc.d$/d' /etc/apache2/apache2.conf

# ${VAR} the doctor cannot resolve: fail closed.
echo 'IncludeOptional ${PSC_UNSET_VARIABLE}/extra.conf' >> /etc/apache2/apache2.conf
run apache_var
check "var: D03 skipped configtest_not_isolated" "[ \"\$(code_of D03)\" = 'skip configtest_not_isolated' ]"
check "var: summary note names the skipped config test" "echo \"\$OUT\" | jq -r '.summary.notes[0]' | grep -q '^config test skipped:'"
check "var: reason names the variable problem" "echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | .reason' | grep -q 'VARIABLE'"
sed -i '/PSC_UNSET_VARIABLE/d' /etc/apache2/apache2.conf

# A non-default ServerRoot, named with --config.
rm -rf /srv/aroot && cp -a /etc/apache2 /srv/aroot
sed -i '1i ServerRoot "/srv/aroot"' /srv/aroot/apache2.conf
echo "ModPagespeedFileCachePath /srv/psc/aroot-cache" > /srv/aroot/conf-enabled/zz-psc.conf
rm -rf /srv/psc/aroot-cache
run apache_serverroot_nodir --config /srv/aroot/apache2.conf
check "serverroot, cache missing: clean skip with a create hint" \
  "[ \"\$(code_of D03)\" = 'skip configtest_not_isolated' ] && echo \"\$OUT\" | jq -r '.checks[] | select(.id==\"D03\") | .fix.hint' | grep -q '^Create /srv/psc/aroot-cache first'"
mkdir -p /srv/psc/aroot-cache && chown www-data: /srv/psc/aroot-cache
run apache_serverroot --config /srv/aroot/apache2.conf
check "serverroot: D11 saw the cache path under the custom ServerRoot" "d11_has /srv/psc/aroot-cache"
check "serverroot: D03 ran shielded" "[ \"\$(echo \"\$OUT\" | jq -r .config.config_test)\" = namespace ]"
check "serverroot: no root-owned file in the cache" "[ -z \"\$(find /srv/psc/aroot-cache -user 0 ! -type d)\" ]"

echo
[ "$fail" = 0 ] && echo "ALL PASS (apache)" || echo "SOME FAILED (apache)"
exit "$fail"
INNER
[ "$rc_nginx" = 0 ] && [ "$rc_apache" = 0 ]
