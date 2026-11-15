#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
#
# test-defects.sh -- regression tests for pagespeed-doctor against stub
# nginx / apache2ctl binaries: no server and no package needed, but it must
# run as root (the config test is skipped otherwise) in a THROWAWAY
# environment such as a container: it creates /etc/apache2 if missing and
# runs the doctor on files it plants. Used by run-container-test.sh's
# image, or directly:
#
#   docker run --rm -v "$PWD:/src:ro" ubuntu:24.04 bash -c \
#     'apt-get update -qq && apt-get install -y -qq jq curl >/dev/null && bash /src/test/doctor/test-defects.sh'
#
#   DOCTOR=path   the doctor to test (default: install/common/pagespeed-doctor)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DOCTOR="${DOCTOR:-$ROOT/install/common/pagespeed-doctor}"
[ "$(id -u)" = 0 ] || { echo "FAIL: run as root, in a throwaway container"; exit 1; }
command -v jq >/dev/null || { echo "FAIL: jq is required"; exit 1; }

W=$(mktemp -d /tmp/doctor-defects.XXXXXX)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/bin" /etc/apache2
export STUB_MARK="$W/mark"
cat > "$W/bin/nginx" <<'STUB'
#!/bin/sh
case "$1" in
  -v) echo "nginx version: nginx/1.24.0" >&2 ;;
  -V) echo "nginx version: nginx/1.24.0" >&2; echo "configure arguments: --prefix=/tmp --conf-path=/etc/nginx/nginx.conf" >&2 ;;
  *) echo "ran $*" >> "$STUB_MARK" ;;
esac
STUB
cat > "$W/bin/apache2ctl" <<'STUB'
#!/bin/sh
case "$1" in
  -v) echo "Server version: Apache/2.4.58 (Ubuntu)" ;;
  -V) echo 'Server version: Apache/2.4.58'; echo ' -D HTTPD_ROOT="/etc/apache2"'; echo ' -D SERVER_CONFIG_FILE="apache2.conf"' ;;
  *) echo "ran $*" >> "$STUB_MARK" ;;
esac
STUB
chmod +x "$W/bin/nginx" "$W/bin/apache2ctl"
export PATH="$W/bin:$PATH"

fail=0
check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fail=1; fi; }
# run SERVER CONF -> $OUT; the stub's marker file shows whether the config test ran
run() {
  rm -f "$STUB_MARK"
  OUT=$(sh "$DOCTOR" --json --server "$1" --config "$2" 2>/dev/null)
}
# shellcheck disable=SC2329 # called from the eval in check
field() { echo "$OUT" | jq -r --arg id "$1" ".checks[] | select(.id == \$id) | $2"; }

# 1. An Apache include the doctor cannot resolve may hold the LoadModule
# line: the config test must be skipped (fail closed), never run unshielded.
mkdir -p "$W/c1"
# shellcheck disable=SC2016 # a literal ${VARIABLE} for the doctor to read
printf 'Include ${PSD_DEFINITELY_UNSET}/mods/*.load\nModPagespeedFileCachePath %s/c1\n' "$W" > "$W/c1/a.conf"
run apache "$W/c1/a.conf"
check "unresolved include: the config test did not run" "[ ! -e '$STUB_MARK' ]"
check "unresolved include: D03 skipped configtest_not_isolated" \
  "[ \"\$(field D03 '\"\\(.status) \\(.code)\"')\" = 'skip configtest_not_isolated' ]"

# 2. A relative cache path is never handed to find: "-delete" would be an
# expression run against the working directory.
mkdir -p "$W/c2/-delete"; chmod 777 "$W/c2/-delete" "$W"; : > "$W/c2/victim"
printf 'load_module modules/ngx_pagespeed_module.so;\npagespeed on;\npagespeed FileCachePath -delete;\n' > "$W/c2.conf"
( cd "$W/c2" && sh "$DOCTOR" --json --server nginx --config "$W/c2.conf" > "$W/c2.out" 2>/dev/null )
OUT=$(cat "$W/c2.out")
check "relative cache path: the working directory is untouched" "[ -e '$W/c2/victim' ]"
check "relative cache path: D11 reports cache_path_relative" \
  "[ \"\$(field D11 '.code')\" = cache_path_relative ]"

# 3. A glob in a cache path is a literal name, not its sibling directories.
mkdir -p "$W/c3/cache1" "$W/c3/cache2"
printf 'load_module modules/ngx_pagespeed_module.so;\npagespeed on;\npagespeed FileCachePath "%s/c3/cache*";\n' "$W" > "$W/c3.conf"
run nginx "$W/c3.conf"
check "glob cache path: D11 names one path" "[ \"\$(field D11 '.evidence | length')\" = 1 ]"
check "glob cache path: no sibling directory in the output" "! echo \"\$OUT\" | grep -q 'cache1'"

# 4. A directive split across lines is parsed (nginx: statement to its ';';
# Apache: backslash continuation). The cache directory does not exist, so the
# skip reason names it.
printf 'load_module modules/ngx_pagespeed_module.so;\npagespeed on;\npagespeed\n  FileCachePath\n    %s/c4/ngx-cache;\n' "$W" > "$W/c4.conf"
run nginx "$W/c4.conf"
check "nginx split directive: the cache path is found" \
  "field D03 '.reason' | grep -q '$W/c4/ngx-cache'"
printf 'LoadModule pagespeed_module /x/mod_pagespeed.so\nModPagespeedFileCachePath \\\n    %s/c4/apache-cache\n' "$W" > "$W/c4a.conf"
run apache "$W/c4a.conf"
check "apache continuation: the cache path is found" \
  "field D03 '.reason' | grep -q '$W/c4/apache-cache'"

[ "$fail" = 0 ] && echo "ALL PASS (defects)" || echo "SOME FAILED (defects)"
exit "$fail"
