#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Test for the stale-web-config check the optimizer deb/rpm maintainer
# scripts run before printing the post-install repoint warning (the
# ACTION REQUIRED half of the cold-start notice).
#
# The warning must be earned: it may fire only when a file the web server
# actually LOADS still names a pre-2.1 path (/var/lib/pagespeed*).
# Operator backups and package leftovers (*.bak*, *.dpkg-*, *.rpmsave,
# *.rpmnew, *.orig, *~) are never loaded, and under Debian's Apache
# layout *-available/ counts only through its *-enabled/ symlink -- so a
# stale .bak in conf-available/ must stay silent on a host whose live
# configuration is already current (a real upgrade warned on exactly
# that).  This test extracts the shipped helper from BOTH maintainer
# scripts and runs it against fixture trees, so what is asserted is what
# the packages install, not a copy of it.
#
# The helper's grep flags are GNU grep's (--exclude, -R following
# symlinks).  Every target distro and the packaging image ship GNU grep;
# a host without it re-executes this test inside debian:12, so the
# semantics under test are always the production ones.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! grep --version 2>/dev/null | head -1 | grep -q '^grep (GNU grep)'; then
  command -v docker >/dev/null 2>&1 || {
    echo "error: host grep is not GNU grep and docker is unavailable;" >&2
    echo "error: run this test inside a Debian-family container" >&2
    exit 2
  }
  exec docker run --rm -v "$HERE/../..":/repo:ro -w /repo debian:12 \
    bash tools/packaging/test-postinst-stale-config.sh
fi

fails=0
check() { # label expected actual
  if [[ "$2" == "$3" ]]; then echo "ok: $1"
  else echo "FAIL: $1 -- expected [$2], got [$3]" >&2; fails=$((fails+1)); fi
}

OLD_PATH='ModPagespeedDaemonVolumePath /var/lib/pagespeed-optimizer/cache'
NEW_PATH='ModPagespeedDaemonVolumePath /var/cache/pagespeed-optimizer/v3/cache'

# ------------------------------------------- extract what the packages ship --
# The deb postinst is a quoted heredoc in build-optimizer-deb.sh; the rpm
# scriptlet is the %post..%preun span of the spec heredoc in
# build-optimizer-rpm.sh.  The spec heredoc is unquoted, so the source
# writes shell '$' as '\$' and a literal backslash as '\\'; both are
# unescaped here before comparison.
heredoc_body() { # file start-pattern -> heredoc content on stdout
  local start
  start="$(grep -n "$2" "$1" | head -1 | cut -d: -f1)"
  [[ -n "$start" ]] || return 1
  tail -n +"$((start + 1))" "$1" | sed -n '1,/^EOF$/p' | sed '$d'
}
helper_of() { # script text on stdin -> the ps_stale_web_config function
  sed -n '/^[[:space:]]*ps_stale_web_config() {/,/^[[:space:]]*}$/p' \
    | sed 's/^[[:space:]]*//'
}

deb_postinst="$(heredoc_body "$HERE/build-optimizer-deb.sh" \
  'cat > "\$staging/DEBIAN/postinst"')"
rpm_spec="$(heredoc_body "$HERE/build-optimizer-rpm.sh" \
  'cat > "\$top/SPECS/\$PKG.spec"')"
rpm_post="$(printf '%s\n' "$rpm_spec" | sed -n '/^%post$/,/^%preun$/p' \
  | sed '1d;$d' | sed 's/\\\\/\\/g; s/\\\$/$/g')"
deb_helper="$(printf '%s\n' "$deb_postinst" | helper_of)"
rpm_helper="$(printf '%s\n' "$rpm_post" | helper_of)"

check "deb postinst carries the scan helper" "nonempty" \
  "$([[ -n "$deb_helper" ]] && echo nonempty || echo empty)"
check "rpm %post carries the scan helper" "nonempty" \
  "$([[ -n "$rpm_helper" ]] && echo nonempty || echo empty)"
check "deb and rpm ship the same helper" "same" \
  "$([[ -n "$deb_helper" && "$deb_helper" == "$rpm_helper" ]] && echo same || echo differ)"
check "deb helper excludes the six backup/leftover name patterns" "6" \
  "$(grep -o -- '--exclude=' <<<"$deb_helper" | wc -l | tr -d ' ')"
check "deb helper reads Apache through its *-enabled/ trees only" "1" \
  "$(grep -c 'apache2/conf-enabled apache2/sites-enabled apache2/mods-enabled' \
    <<<"$deb_helper" || true)"
check "deb helper enumerates nginx load paths, not the whole tree" "5" \
  "$(grep -o 'nginx/' <<<"$deb_helper" | wc -l | tr -d ' ')"
check "deb gates the repoint warning on the scan" "1" \
  "$(grep -c 'if ps_stale_web_config; then' <<<"$deb_postinst" || true)"
check "rpm gates the repoint warning on the scan" "1" \
  "$(grep -c 'if ps_stale_web_config; then' <<<"$rpm_post" || true)"
check "deb gated block is the ACTION REQUIRED warning" "1" \
  "$(printf '%s\n' "$deb_postinst" \
    | sed -n '/if ps_stale_web_config; then/,/^[[:space:]]*fi$/p' \
    | grep -c 'ACTION REQUIRED' || true)"

# ------------------------------------------------- run the shipped helper ----
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
printf '%s\n' "$deb_helper" > "$tmp/helper.sh"
stale() { # fixture-root -> "stale" (warning fires) or "clean" (silent)
  sh -c '. "$1"; if ps_stale_web_config "$2"; then echo stale; else echo clean; fi' \
    _ "$tmp/helper.sh" "$1"
}

# The reported upgrade, verbatim: every enabled Apache file current, the
# only old-path file an operator backup in conf-available/.
fx="$tmp/backup-only"
mkdir -p "$fx/etc/apache2/conf-available" "$fx/etc/apache2/conf-enabled"
printf '%s\n' "$NEW_PATH" > "$fx/etc/apache2/conf-available/pagespeed-daemon.conf"
ln -s ../conf-available/pagespeed-daemon.conf \
  "$fx/etc/apache2/conf-enabled/pagespeed-daemon.conf"
printf '%s\n' "$OLD_PATH" \
  > "$fx/etc/apache2/conf-available/pagespeed-daemon.conf.bak-pre-rc7"
check "backup-only old paths stay silent (the reported upgrade)" "clean" \
  "$(stale "$fx")"

# The warning's positive: the enabled file (a symlink to its
# *-available/ target, as a2enconf lays it out) names the old path.
fx="$tmp/enabled-symlink"
mkdir -p "$fx/etc/apache2/conf-available" "$fx/etc/apache2/conf-enabled"
printf '%s\n' "$OLD_PATH" > "$fx/etc/apache2/conf-available/pagespeed-daemon.conf"
ln -s ../conf-available/pagespeed-daemon.conf \
  "$fx/etc/apache2/conf-enabled/pagespeed-daemon.conf"
printf '%s\n' "$OLD_PATH" \
  > "$fx/etc/apache2/conf-available/pagespeed-daemon.conf.bak-pre-rc7"
check "enabled file with old paths warns (symlinked a2enconf layout)" "stale" \
  "$(stale "$fx")"

# The same positive through mods-enabled/, where Debian's module config
# (the ModPagespeed* directives) is loaded from.
fx="$tmp/mods-enabled"
mkdir -p "$fx/etc/apache2/mods-available" "$fx/etc/apache2/mods-enabled"
printf '%s\n' "$OLD_PATH" > "$fx/etc/apache2/mods-available/pagespeed.conf"
ln -s ../mods-available/pagespeed.conf "$fx/etc/apache2/mods-enabled/pagespeed.conf"
check "mods-enabled file with old paths warns" "stale" "$(stale "$fx")"

# A scan error must not un-earn the warning: grep exits 2 when any entry
# in a scanned tree errors, and a dangling *-enabled/ symlink (the
# leftover of a removed package or a half-done edit) is a plausible one
# -- a genuinely stale enabled file beside it must still warn.
fx="$tmp/apache2-dangling"
mkdir -p "$fx/etc/apache2/conf-enabled"
printf '%s\n' "$OLD_PATH" > "$fx/etc/apache2/conf-enabled/pagespeed-daemon.conf"
ln -s ../conf-available/gone.conf "$fx/etc/apache2/conf-enabled/gone.conf"
check "dangling symlink cannot hide a stale enabled file" "stale" \
  "$(stale "$fx")"

# nginx loads nginx.conf, conf.d/, sites-enabled/, modules-enabled/ and default.d/;
# sites-available/ is read only when linked from sites-enabled/. The live config
# warns, dpkg leftovers do not.
fx="$tmp/nginx"
mkdir -p "$fx/etc/nginx"
printf '%s\n' "pagespeed_cache_path /var/cache/pagespeed-optimizer/v1/cache;" \
  > "$fx/etc/nginx/nginx.conf"
printf '%s\n' "pagespeed_cache_path /var/lib/pagespeed-optimizer/cache;" \
  > "$fx/etc/nginx/pagespeed.conf.dpkg-dist"
check "nginx dpkg leftover with old paths stays silent" "clean" "$(stale "$fx")"
printf '%s\n' "pagespeed_cache_path /var/lib/pagespeed-optimizer/cache;" \
  >> "$fx/etc/nginx/nginx.conf"
check "live nginx config with old paths warns" "stale" "$(stale "$fx")"

# Debian's nginx keeps sites in sites-available/ and loads them only
# through sites-enabled/ symlinks (same discipline as Apache's
# *-available/ split): a stale site whose symlink was removed is a
# disabled file nginx never reads, and the same file linked back in is
# live again.
fx="$tmp/nginx-sites"
mkdir -p "$fx/etc/nginx/sites-available" "$fx/etc/nginx/sites-enabled"
printf '%s\n' "pagespeed_cache_path /var/lib/pagespeed-optimizer/cache;" \
  > "$fx/etc/nginx/sites-available/mysite"
check "nginx disabled site in sites-available only stays silent" "clean" \
  "$(stale "$fx")"
ln -s ../sites-available/mysite "$fx/etc/nginx/sites-enabled/mysite"
check "nginx site linked from sites-enabled warns" "stale" "$(stale "$fx")"

# RHEL httpd: the live conf.d file warns; rpm leftovers and editor
# backups (anywhere) do not.
fx="$tmp/httpd"
mkdir -p "$fx/etc/httpd/conf.d"
printf '%s\n' "$NEW_PATH" > "$fx/etc/httpd/conf.d/pagespeed.conf"
printf '%s\n' "$OLD_PATH" > "$fx/etc/httpd/conf.d/pagespeed.conf.rpmsave"
printf '%s\n' "$OLD_PATH" > "$fx/etc/httpd/conf.d/00-base.conf.rpmnew"
printf '%s\n' "$OLD_PATH" > "$fx/etc/httpd/conf.d/00-base.conf.orig"
printf '%s\n' "$OLD_PATH" > "$fx/etc/httpd/conf.d/00-base.conf~"
check "httpd rpmnew/rpmsave/orig/editor backups stay silent" "clean" \
  "$(stale "$fx")"
printf '%s\n' "$OLD_PATH" >> "$fx/etc/httpd/conf.d/pagespeed.conf"
check "live httpd config with old paths warns" "stale" "$(stale "$fx")"

# The optional second argument names the path to look for: the v2
# cache-path notice fires only while a loaded file still names it.
fx="$tmp/v2-needle"
mkdir -p "$fx/etc/nginx"
printf '%s\n' "pagespeed DaemonVolumePath /var/cache/pagespeed-optimizer/v3/cache;" \
  > "$fx/etc/nginx/nginx.conf"
needle() { sh -c '. "$1"; if ps_stale_web_config "$2" /var/cache/pagespeed-optimizer/v2; then echo stale; else echo clean; fi' _ "$tmp/helper.sh" "$1"; }
check "configured v3 path does not match the v2 needle" "clean" "$(needle "$fx")"
printf '%s\n' "pagespeed DaemonVolumePath /var/cache/pagespeed-optimizer/v2/cache;" \
  > "$fx/etc/nginx/nginx.conf"
check "configured v2 path matches the v2 needle" "stale" "$(needle "$fx")"
needle_v1() { sh -c '. "$1"; if ps_stale_web_config "$2" /var/cache/pagespeed-optimizer/v1; then echo stale; else echo clean; fi' _ "$tmp/helper.sh" "$1"; }
check "configured v2 path does not match the v1 needle" "clean" "$(needle_v1 "$fx")"
printf '%s\n' "pagespeed DaemonVolumePath /var/cache/pagespeed-optimizer/v1/cache;" \
  > "$fx/etc/nginx/nginx.conf"
check "configured v1 path matches the v1 needle" "stale" "$(needle_v1 "$fx")"
check "configured v1 path does not match the v2 needle" "clean" "$(needle "$fx")"

# A host with no web-server configuration at all: nothing to warn about.
fx="$tmp/no-webserver"
mkdir -p "$fx/etc"
check "no web-server configuration stays silent" "clean" "$(stale "$fx")"

# A failing `systemctl daemon-reload` must not abort configuration: run
# the shipped systemd block (deb under set -e, rpm %post) with a fake
# systemctl whose daemon-reload exits 1, and require the later steps ran.
fake="$tmp/fakebin"
mkdir -p "$fake"
cat > "$fake/systemctl" <<'SH'
#!/bin/sh
echo "$*" >> "$SYSTEMCTL_LOG"
[ "$1" = daemon-reload ] && exit 1
exit 0
SH
chmod +x "$fake/systemctl"
reload_case() { # label script-text -> logs the calls, checks restart ran
  local body
  body="$(printf '%s\n' "$2" \
    | sed -n '/^if \[ -d \/run\/systemd\/system \]; then$/,/^fi$/p' \
    | sed "s#/run/systemd/system#$tmp#; s/\$PKG/pagespeed-optimizer/g")"
  printf '#!/bin/sh\nset -e\n%s\necho done\n' "$body" > "$tmp/systemd-block.sh"
  : > "$tmp/systemctl.log"
  out="$(SYSTEMCTL_LOG="$tmp/systemctl.log" PATH="$fake:$PATH" \
    sh "$tmp/systemd-block.sh" configure 2>&1)" || out="aborted"
  check "$1: failing daemon-reload does not abort" "done" "$(tail -n 1 <<<"$out")"
  check "$1: restart still runs after failing daemon-reload" "1" \
    "$(grep -c '^restart pagespeed-optimizer.service$' "$tmp/systemctl.log" || true)"
}
reload_case "deb postinst" "$deb_postinst"
reload_case "rpm %post" "$rpm_post"

if [[ "$fails" -gt 0 ]]; then
  echo "test-postinst-stale-config: $fails FAILURE(S)" >&2
  exit 1
fi
echo "test-postinst-stale-config: all checks passed"
