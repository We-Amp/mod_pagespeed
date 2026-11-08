#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# The first-ship tests for the nginx.org module package: what happens to an
# installed host when nginx.org ships a newer nginx, to a host on the
# distribution's nginx that can see the package, and to a host that installs
# from a repository carrying the package for two nginx versions. Each scenario
# runs in its own clean container, with the real nginx.org and
# packages.modpagespeed.com repositories.
# Usage: check-nginx-org-upgrade.sh <image> <old nginx> <old package> <new nginx> <new package>
# SCENARIOS="a b" runs only the named scenarios (default: all).
set -uo pipefail
# shellcheck source=tools/ci/docker_pull_retry.sh
source "$(dirname "${BASH_SOURCE[0]}")/../../tools/ci/docker_pull_retry.sh"
IMAGE="${1:?image}"; OLDV="${2:?old nginx}"; OLDP="${3:?old package}"; NEWV="${4:?new nginx}"; NEWP="${5:?new package}"
DIR="$(cd "$(dirname "$OLDP")" && pwd)"
[ "$DIR" = "$(cd "$(dirname "$NEWP")" && pwd)" ] || { echo "both packages must share a directory"; exit 2; }
COMMON='
fail() { echo "FAIL: $*"; exit 1; }
if command -v apt-get >/dev/null; then
  FAMILY=deb; export DEBIAN_FRONTEND=noninteractive; . /etc/os-release
  apt-get -qq update >/dev/null; apt-get -qq install -y curl ca-certificates gnupg >/dev/null 2>&1
  curl -fsSL https://nginx.org/keys/nginx_signing.key | gpg --dearmor > /usr/share/keyrings/nginx.gpg
  echo "deb [signed-by=/usr/share/keyrings/nginx.gpg] https://nginx.org/packages/${ID} ${VERSION_CODENAME} nginx" > /etc/apt/sources.list.d/nginx.list
  printf "Package: *\nPin: origin nginx.org\nPin-Priority: 900\n" > /etc/apt/preferences.d/99nginx
  apt-get -qq update >/dev/null
  ngx_ver() { dpkg-query -W -f="\${Version}" nginx; }
  have_mod() { dpkg-query -W -f="\${Status}" nginx-module-pagespeed-nginxorg 2>/dev/null | grep -q "install ok installed"; }
  inst_ngx() { apt-get -y --allow-downgrades install "nginx=$1-1~${VERSION_CODENAME}" 2>&1; }
  inst_file() { cp "/pkgs/$1" /tmp/; apt-get -y install "/tmp/$1" 2>&1; }
  INCL="/etc/nginx/modules-enabled/*.conf"
else
  FAMILY=rpm
  cat > /etc/yum.repos.d/nginx.repo <<R
[nginx-stable]
name=nginx stable
baseurl=https://nginx.org/packages/rhel/\$releasever/\$basearch/
gpgcheck=1
enabled=1
gpgkey=https://nginx.org/keys/nginx_signing.key
module_hotfixes=true
R
  ngx_ver() { rpm -q --qf "%{VERSION}" nginx; }
  have_mod() { rpm -q nginx-module-pagespeed-nginxorg >/dev/null 2>&1; }
  inst_ngx() { dnf -y install "nginx-2:$1" --disablerepo=appstream 2>&1; }
  inst_file() { dnf -y install "/pkgs/$1" 2>&1; }
  INCL="/usr/share/nginx/modules/*.conf"
  command -v curl >/dev/null || dnf -q -y install curl-minimal >/dev/null 2>&1
fi
setup_ngx() {
  out="$(inst_ngx "$OLDV")" || { echo "$out" | tail -n 6; fail "nginx $OLDV did not install"; }
}
setup() {
  setup_ngx
  out="$(inst_file "$OLDP")" || { echo "$out" | tail -n 6; fail "the old package did not install"; }
  [ "$(ngx_ver | cut -d- -f1)" = "$OLDV" ] || fail "installing the package changed nginx to $(ngx_ver)"
  setup_conf
}
setup_conf() {
  sed -i "1i include ${INCL};" /etc/nginx/nginx.conf
  mkdir -p /var/cache/ngx_pagespeed; chown nginx /var/cache/ngx_pagespeed
  cat > /etc/nginx/conf.d/ps.conf <<C
server {
  listen 8080;
  root /usr/share/nginx/html;
  pagespeed on;
  pagespeed FileCachePath /var/cache/ngx_pagespeed;
  pagespeed EnableFilters collapse_whitespace,rewrite_css,inline_css;
  location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" { add_header "" ""; }
}
C
  printf "<html><head><link rel=stylesheet href=/a.css></head><body>\n\n\n  <p>hello</p>\n\n  </body></html>" > /usr/share/nginx/html/t.html
  printf "body {  color : red ;   margin : 0 ; }\n" > /usr/share/nginx/html/a.css
}
serve_ok() {
  nginx -t >/dev/null 2>&1 || { nginx -t 2>&1 | tail -n 3; fail "nginx -t: $1"; }
  nginx || fail "nginx did not start: $1"
  for _ in 1 2 3 4 5 6; do curl -s -o /dev/null http://localhost:8080/t.html || fail "not answering: $1"; sleep 2; done
  curl -s http://localhost:8080/t.html | grep -q "body{color:red;margin:0}" || fail "no rewrite: $1"
  if grep -E "\[(alert|emerg|crit)\]" /var/log/nginx/error.log; then fail "alerts: $1"; fi
  nginx -s stop; sleep 1
}
'
run() { # name, script
  local name="$1" body="$2"
  case " ${SCENARIOS:-all} " in *" all "*|*" $name "*) ;; *) return 0 ;; esac
  echo "--- $name"
  docker_pull_retry "$IMAGE" || return 1
  docker run --rm --name "pagespeed-nginxorg-upg-$$-$name" -v "$DIR":/pkgs:ro \
    -e OLDV="$OLDV" -e NEWV="$NEWV" -e OLDP="$(basename "$OLDP")" -e NEWP="$(basename "$NEWP")" \
    "$IMAGE" bash -uc "$COMMON
$body" </dev/null
  local rc=$?
  echo "RESULT $name=$rc"
  [ "$rc" = 0 ] || FAILED=1
}
FAILED=0

run together '
setup; serve_ok "on the old pair"
if [ "$FAMILY" = deb ]; then
  cp "/pkgs/$NEWP" /tmp/
  out="$(apt-get -y --allow-downgrades install "nginx=${NEWV}-1~${VERSION_CODENAME}" "/tmp/$NEWP" 2>&1)" || { echo "$out" | tail -n 6; fail "the pair did not upgrade"; }
else
  out="$(dnf -y install "nginx-2:${NEWV}" "/pkgs/$NEWP" --disablerepo=appstream 2>&1)" || { echo "$out" | tail -n 6; fail "the pair did not upgrade"; }
fi
[ "$(ngx_ver | cut -d- -f1)" = "$NEWV" ] || fail "nginx is $(ngx_ver), wanted $NEWV"
serve_ok "after upgrading the pair"
echo "across-patch upgrade ok: nginx $(ngx_ver)"
'

# A plain update of the whole host, with a newer nginx.org nginx published and
# no matching package yet. The host must stay intact, and the update of
# everything else must go through: nginx is held, nothing else is.
run plain_upgrade '
setup
BLOCKED=0
if [ "$FAMILY" = deb ]; then
  apt-get -y upgrade >/tmp/o 2>&1; rc=$?
  echo "apt upgrade rc=$rc, nginx now $(ngx_ver)"
  grep -iE "kept back|^The following packages have been" /tmp/o | head -n 2 | cut -c1-110
  [ "$rc" = 0 ] || BLOCKED=1
else
  dnf -y update nginx --disablerepo=appstream >/tmp/o 2>&1; rc=$?
  echo "dnf update nginx rc=$rc, nginx now $(ngx_ver)"
  tail -n 4 /tmp/o | cut -c1-130
  dnf -y update --disablerepo=appstream >/tmp/o2 2>&1; rc2=$?
  echo "dnf update (all) rc=$rc2, nginx now $(ngx_ver)"
  tail -n 4 /tmp/o2 | cut -c1-130
  if [ "$rc2" != 0 ]; then
    BLOCKED=1
    dnf -y update --exclude=nginx --disablerepo=appstream >/tmp/o3 2>&1; echo "dnf update --exclude=nginx rc=$?, nginx now $(ngx_ver)"
    dnf -y update --nobest --disablerepo=appstream >/tmp/o4 2>&1; echo "dnf update --nobest rc=$?, nginx now $(ngx_ver)"
    grep -iE "skipping|broken" /tmp/o4 | head -n 2 | cut -c1-120
  fi
fi
[ "$(ngx_ver | cut -d- -f1)" = "$OLDV" ] && echo "nginx held at $OLDV"
have_mod && echo "module still installed" || echo "module removed"
serve_ok "after a plain upgrade"
[ "$BLOCKED" = 0 ] || fail "a plain update of the host fails as a whole while this package lags nginx.org: the host takes no other updates until the operator excludes nginx"
'

run full_upgrade_removal '
setup
if [ "$FAMILY" = deb ]; then
  apt-get -y full-upgrade >/tmp/o 2>&1; echo "full-upgrade rc=$?, nginx now $(ngx_ver)"
  have_mod && echo "module still installed" || echo "module removed"
  have_mod && fail "full-upgrade kept the module"
  rm -f /etc/nginx/conf.d/ps.conf; nginx -t >/dev/null 2>&1 || { nginx -t 2>&1 | tail -n 3; fail "nginx -t after full-upgrade removed the module"; }; echo "nginx -t clean after removal"
else
  out="$(dnf -y install "nginx-2:${NEWV}" --allowerasing --disablerepo=appstream 2>&1)"; echo "dnf --allowerasing rc=$?, nginx now $(ngx_ver)"
  have_mod && echo "module still installed" || echo "module removed"
  have_mod && fail "the erase kept the module"
  rm -f /etc/nginx/conf.d/ps.conf; nginx -t >/dev/null 2>&1 || { nginx -t 2>&1 | tail -n 3; fail "nginx -t after the module was erased"; }; echo "nginx -t clean after removal"
fi
'

run side_by_side '
setup
if [ "$FAMILY" = deb ]; then
  mkdir -p /tmp/d/DEBIAN
  printf "Package: nginx-module-pagespeed\nVersion: 9.9.9\nArchitecture: all\nMaintainer: test <t@example.com>\nDescription: test\n" > /tmp/d/DEBIAN/control
  dpkg-deb -b /tmp/d /tmp/stock.deb >/dev/null
  if dpkg -i /tmp/stock.deb >/tmp/o 2>&1; then fail "the stock package installed beside this one"; fi
  grep -i conflict /tmp/o | head -n 1 | cut -c1-120
else
  dnf -q -y install rpm-build >/dev/null 2>&1
  mkdir -p /tmp/r; cat > /tmp/r/t.spec <<S
Name: nginx-module-pagespeed
Version: 9.9.9
Release: 1
Summary: test
License: test
BuildArch: noarch
%description
test
%files
S
  rpmbuild -bb --define "_rpmdir /tmp/r" /tmp/r/t.spec >/dev/null 2>&1 || fail "could not build the stand-in"
  if rpm -i /tmp/r/noarch/*.rpm >/tmp/o 2>&1; then fail "the stock package installed beside this one"; fi
  grep -i conflict /tmp/o | head -n 1 | cut -c1-120
fi
echo "refused side by side"
'

# A host on the distribution's own nginx and its own module package, with the
# nginx.org package visible in a repository: a plain and a full upgrade leave it
# as it is, and the nginx.org package does not install onto it. The module
# package comes from the live packages.modpagespeed.com, so this scenario also
# depends on that repository and on the distribution's nginx being the version
# the published module was built for.
run distro_host '
if [ "$FAMILY" = deb ]; then
  rm -f /etc/apt/sources.list.d/nginx.list /etc/apt/preferences.d/99nginx
  curl -fsSL https://packages.modpagespeed.com/install.sh | sh >/tmp/i 2>&1 || { tail -n 5 /tmp/i; fail "the package repository did not configure"; }
  apt-get -qq update >/dev/null
  apt-get -y install nginx nginx-module-pagespeed apt-utils >/tmp/o 2>&1 || { tail -n 5 /tmp/o; fail "the distribution nginx and its published module did not install (a public repository condition)"; }
  before="$(ngx_ver)"
  mkdir -p /srv/repo && cp "/pkgs/$NEWP" /srv/repo/ && (cd /srv/repo && apt-ftparchive packages . > Packages && gzip -kf Packages) || fail "local repository"
  echo "deb [trusted=yes] file:/srv/repo ./" > /etc/apt/sources.list.d/local.list
  apt-get -qq update >/dev/null
  apt-get -y upgrade >/tmp/o 2>&1 || { tail -n 5 /tmp/o; fail "apt upgrade failed on a host on the distribution nginx"; }
  apt-get -y full-upgrade >/tmp/o2 2>&1 || { tail -n 5 /tmp/o2; fail "apt full-upgrade failed on a host on the distribution nginx"; }
  [ "$(ngx_ver)" = "$before" ] || fail "the distribution nginx moved from $before to $(ngx_ver)"
  have_mod && fail "the nginx.org package was installed on a host on the distribution nginx"
  dpkg-query -W -f="\${Status}" nginx-module-pagespeed | grep -q "install ok installed" || fail "the distribution module was removed"
  nginx -t >/dev/null 2>&1 || { nginx -t 2>&1 | tail -n 3; fail "nginx -t on the distribution host"; }
  if apt-get -s install nginx-module-pagespeed-nginxorg >/tmp/o3 2>&1; then fail "the nginx.org package would install onto the distribution nginx"; fi
  grep -qE "Depends: nginx|Conflicts" /tmp/o3 || { tail -n 3 /tmp/o3; fail "the refusal did not come from the dependency or conflict of the package"; }
  grep -E "Depends: nginx|Conflicts" /tmp/o3 | head -n 1 | cut -c1-120
else
  rm -f /etc/yum.repos.d/nginx.repo
  command -v gpg >/dev/null || dnf -q -y install gnupg2 >/dev/null 2>&1
  curl -fsSL https://packages.modpagespeed.com/install.sh | sh >/tmp/i 2>&1 || { tail -n 5 /tmp/i; fail "the package repository did not configure"; }
  dnf -y install nginx nginx-module-pagespeed createrepo_c >/tmp/o 2>&1 || { tail -n 5 /tmp/o; fail "the distribution nginx and its published module did not install (a public repository condition)"; }
  before="$(rpm -q nginx)"
  mkdir -p /srv/repo && cp "/pkgs/$NEWP" /srv/repo/ && createrepo_c -q /srv/repo || fail "local repository"
  printf "[local]\nname=local\nbaseurl=file:///srv/repo\ngpgcheck=0\nenabled=1\n" > /etc/yum.repos.d/local.repo
  dnf -y update >/tmp/o 2>&1 || { tail -n 5 /tmp/o; fail "dnf update failed on a host on the distribution nginx"; }
  [ "$(rpm -q nginx)" = "$before" ] || fail "the distribution nginx moved from $before to $(rpm -q nginx)"
  have_mod && fail "the nginx.org package was installed on a host on the distribution nginx"
  rpm -q nginx-module-pagespeed >/dev/null || fail "the distribution module was removed"
  nginx -t >/dev/null 2>&1 || { nginx -t 2>&1 | tail -n 3; fail "nginx -t on the distribution host"; }
  if dnf -y install nginx-module-pagespeed-nginxorg >/tmp/o3 2>&1; then fail "the nginx.org package would install onto the distribution nginx"; fi
  grep -qiE "nothing provides|conflict" /tmp/o3 || { tail -n 3 /tmp/o3; fail "the refusal did not come from the dependency or conflict of the package"; }
  grep -iE "nothing provides|conflict" /tmp/o3 | head -n 1 | cut -c1-120
fi
echo "a host on the distribution nginx is unaffected: nginx $(ngx_ver)"
'

# After a rebuild the repository carries the package for the previous nginx
# and for the new one. A host on the previous nginx, with nginx.org's
# repository configured, installs the matching package by version and keeps
# its nginx; the plan of an unpinned install is shown for the record. A plain
# upgrade then moves nginx and the module together, which assumes the new
# nginx is still nginx.org's newest stable: a newer one holds nginx back.
run two_versions '
setup_ngx
mkdir -p /srv/repo && cp "/pkgs/$OLDP" "/pkgs/$NEWP" /srv/repo/ || fail "local repository"
if [ "$FAMILY" = deb ]; then
  apt-get -qq install -y apt-utils >/dev/null 2>&1
  (cd /srv/repo && apt-ftparchive packages . > Packages && gzip -kf Packages) || fail "local repository index"
  echo "deb [trusted=yes] file:/srv/repo ./" > /etc/apt/sources.list.d/local.list
  apt-get -qq update >/dev/null
  apt-get -s install nginx-module-pagespeed-nginxorg >/tmp/o 2>&1
  echo "an unpinned install on nginx $OLDV would: $(grep -E "^Inst nginx" /tmp/o | cut -d" " -f2,3 | tr "\n" ";" | cut -c1-120)"
  # The documented way to install the package for the nginx at hand: look the
  # version up with apt-cache madison and install exactly that one, so a build
  # fails rather than moving nginx when no package exists for its nginx. apt
  # takes no version glob on the command line, and a version pin in a
  # preference is a prefix match, so neither selects a version by its middle.
  madison() { apt-cache madison nginx-module-pagespeed-nginxorg | awk -v n="+ngx$1~" "index(\$3, n) { print \$3; exit }"; }
  v="$(madison 9.9.9)"
  [ -z "$v" ] || fail "a lookup for an nginx without a package returned $v"
  echo "no package for an nginx without one: the lookup is empty"
  v="$(madison "$OLDV")"
  [ -n "$v" ] || fail "the lookup found no package for nginx $OLDV"
  case "$v" in *"+ngx${OLDV}~"*) ;; *) fail "the lookup returned $v for nginx $OLDV" ;; esac
  apt-get -y install "nginx-module-pagespeed-nginxorg=$v" >/tmp/o 2>&1 || { tail -n 5 /tmp/o; fail "the version-matched install failed"; }
  echo "version-matched install: $(dpkg-query -W -f="\${Version}" nginx-module-pagespeed-nginxorg)"
else
  dnf -q -y install createrepo_c >/dev/null 2>&1
  createrepo_c -q /srv/repo || fail "local repository index"
  printf "[local]\nname=local\nbaseurl=file:///srv/repo\ngpgcheck=0\nenabled=1\n" > /etc/yum.repos.d/local.repo
  dnf install --assumeno nginx-module-pagespeed-nginxorg --disablerepo=appstream >/tmp/o 2>&1
  echo "an unpinned install on nginx $OLDV would: $(grep -E "^ (nginx|nginx-module-pagespeed-nginxorg) " /tmp/o | awk "{print \$1\" \"\$3}" | tr "\n" ";" | cut -c1-120)"
  dnf -y install "nginx-module-pagespeed-nginxorg*ngx${OLDV}.el9*" --disablerepo=appstream >/tmp/o 2>&1 || { tail -n 5 /tmp/o; fail "the version-matched install failed"; }
  echo "version-matched install: $(rpm -q nginx-module-pagespeed-nginxorg)"
fi
[ "$(ngx_ver | cut -d- -f1)" = "$OLDV" ] || fail "the version-matched install moved nginx to $(ngx_ver)"
setup_conf
serve_ok "on the version-matched package"
if [ "$FAMILY" = deb ]; then
  apt-get -y upgrade >/tmp/o 2>&1 || { tail -n 5 /tmp/o; fail "the plain upgrade with both builds available failed"; }
  modver="$(dpkg-query -W -f="\${Version}" nginx-module-pagespeed-nginxorg)"
else
  dnf -y update --disablerepo=appstream >/tmp/o 2>&1 || { tail -n 5 /tmp/o; fail "the plain update with both builds available failed"; }
  modver="$(rpm -q nginx-module-pagespeed-nginxorg)"
fi
[ "$(ngx_ver | cut -d- -f1)" = "$NEWV" ] || fail "the plain upgrade did not move nginx to $NEWV: $(ngx_ver) (is $NEWV still the newest stable on nginx.org?)"
case "$modver" in *"ngx${NEWV}"*) ;; *) fail "the module did not move with nginx: $modver" ;; esac
serve_ok "after the plain upgrade moved the pair"
echo "a plain upgrade moved the pair to nginx $NEWV ($modver)"
'

[ "$FAILED" = 0 ] && echo "UPGRADE-PASS" || echo "UPGRADE-FAIL"
exit "$FAILED"
