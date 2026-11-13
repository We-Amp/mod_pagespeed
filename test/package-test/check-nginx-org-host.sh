#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Installs nginx.org's nginx at the exact version in a clean container of a
# supported distribution, then the module package, and proves what a user needs:
# the pin, an unchanged nginx, a rewriting request, and clean removal.
# Usage: check-nginx-org-host.sh <image> <nginx version> <package file>
set -euo pipefail
# shellcheck source=tools/ci/docker_pull_retry.sh
source "$(dirname "${BASH_SOURCE[0]}")/../../tools/ci/docker_pull_retry.sh"

# dnf with retries, for the throwaway EL containers: a short mirror or
# metadata outage (a Rocky/Alma mirror serving an empty repomd.xml, a curl
# error, a 5xx) is waited out, anything else (a missing package, a conflict,
# a dependency error) fails on the first attempt. Up to 3 tries, with
# `dnf clean metadata` and a growing pause between them; a lasting outage
# still fails loud with dnf's last output. Usage: dnf_retry install -y PKG...
# or dnf_retry repoquery ... (stderr is shown after each attempt, stdout after
# the last one). DNF_RETRY_SLEEP_UNIT (default 10) scales the pause. The function
# is spliced into container commands with declare -f, so it runs in the
# container's shell.
dnf_retry() {
  local attempt rc out err
  out="$(mktemp)" && err="$(mktemp)" || return 1
  for attempt in 1 2 3; do
    rc=0
    dnf "$@" --setopt=skip_if_unavailable=False >"$out" 2>"$err" || rc=$?
    cat "$err" >&2
    if [ "$rc" -eq 0 ]; then
      cat "$out"
      rm -f "$out" "$err"
      return 0
    fi
    if ! grep -Eqi 'Failed to download metadata|repomd\.xml parser error|Errors during downloading metadata|Curl error|Cannot download|Status code: 5[0-9][0-9]|Operation timed out|Timeout was reached|timed out|Could not resolve host|Cannot prepare internal mirrorlist|Failed to synchronize cache|Connection (reset|refused)' "$out" "$err"; then
      cat "$out"
      rm -f "$out" "$err"
      return "$rc"
    fi
    if [ "$attempt" -lt 3 ]; then
      echo "::warning::dnf $1 failed on a repository or network error (attempt $attempt of 3), retrying" >&2
      dnf clean metadata >/dev/null 2>&1 || true
      sleep $((attempt * ${DNF_RETRY_SLEEP_UNIT:-10}))
    fi
  done
  echo "::error::dnf $1 failed after 3 attempts" >&2
  cat "$out"
  rm -f "$out" "$err"
  return "$rc"
}

IMAGE="${1:?image}"; NGXVER="${2:?nginx version}"; PKG="${3:?package}"
NAME="pagespeed-nginxorg-host-$$"
cleanup() { docker rm -f "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT
BASE="$(basename "$PKG")"
docker_pull_retry "$IMAGE" || exit 1
docker run --rm --name "$NAME" -v "$(cd "$(dirname "$PKG")" && pwd)":/pkgs:ro \
  -e NGXVER="$NGXVER" -e BASE="$BASE" "$IMAGE" bash -euc '
'"$(declare -f dnf_retry)"'
fail() { echo "FAIL: $*"; exit 1; }
if command -v apt-get >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  . /etc/os-release
  apt-get -qq update >/dev/null
  apt-get -qq install -y curl ca-certificates gnupg >/dev/null 2>&1
  # Pinned set: the three keys in https://nginx.org/keys/nginx_signing.key (fingerprints per https://nginx.org/en/linux_packages.html); accept the file only if every key in it is listed.
  NGINX_KEY_FPR="573BFD6B3D8FBC641079A6ABABF5BD827BD9BF62 8540A6F18833A80E9C1653A42FD21310B49F6B46 9E9BE90EACBCDE69FE9B204CBCDCD8A38D88A2B3"
  nkey="$(mktemp -d)"
  curl -fsSL --retry 5 --retry-all-errors --connect-timeout 20 --max-time 60 -o "$nkey/nginx.asc" https://nginx.org/keys/nginx_signing.key || fail "could not fetch the nginx.org signing key"
  GNUPGHOME="$nkey" gpg --batch --quiet --import "$nkey/nginx.asc"
  got="$(GNUPGHOME="$nkey" gpg --batch --with-colons --list-keys | grep -A1 "^pub:" | grep "^fpr:" | cut -d: -f10)"
  [ -n "$got" ] || fail "no key found in the nginx.org key file"
  for f in $got; do case " $NGINX_KEY_FPR " in *" $f "*) ;; *) fail "unpinned key $f in the nginx.org key file";; esac; done
  gpg --dearmor < "$nkey/nginx.asc" > /usr/share/keyrings/nginx.gpg
  rm -rf "$nkey"
  echo "deb [signed-by=/usr/share/keyrings/nginx.gpg] https://nginx.org/packages/${ID} ${VERSION_CODENAME} nginx" > /etc/apt/sources.list.d/nginx.list
  printf "Package: *\nPin: origin nginx.org\nPin-Priority: 900\n" > /etc/apt/preferences.d/99nginx
  apt-get -qq update >/dev/null
  out="$(apt-get -y install "nginx=${NGXVER}-1~${VERSION_CODENAME}" 2>&1)" || { echo "$out" | tail -n 8; fail "nginx.org nginx ${NGXVER} did not install"; }
  before="$(dpkg-query -W -f="\${Version}" nginx)"
  echo "Depends: $(dpkg-deb -f "/pkgs/$BASE" Depends)"
  dpkg-deb -f "/pkgs/$BASE" Depends | grep -Eq "nginx \(>= ${NGXVER}\), nginx \(<< " || fail "the package does not pin nginx ${NGXVER}"
  dpkg-deb -f "/pkgs/$BASE" Conflicts | grep -Eq "(^|[ ,])nginx-module-pagespeed($|[ ,])" || fail "the package does not conflict with the stock build"
  cp "/pkgs/$BASE" /tmp/; out="$(apt-get -y install "/tmp/$BASE" 2>&1)" || { echo "$out" | tail -n 8; fail "the package did not install"; }
  after="$(dpkg-query -W -f="\${Version}" nginx)"
  remove() { dpkg -r nginx-module-pagespeed-nginxorg >/dev/null; }
  INCL="/etc/nginx/modules-enabled/*.conf"
  MOD=/usr/lib/nginx/modules/ngx_pagespeed_module.so
else
  cat > /etc/yum.repos.d/nginx.repo <<R
[nginx-stable]
name=nginx stable
baseurl=https://nginx.org/packages/rhel/\$releasever/\$basearch/
gpgcheck=1
enabled=1
gpgkey=https://nginx.org/keys/nginx_signing.key
module_hotfixes=true
R
  out="$(dnf_retry install -y "nginx-2:${NGXVER}" --disablerepo=appstream 2>&1)" || { echo "$out" | tail -n 8; fail "nginx.org nginx ${NGXVER} did not install"; }
  before="$(rpm -q --qf "%{EPOCH}:%{VERSION}-%{RELEASE}" nginx)"
  rpm -qp --requires "/pkgs/$BASE" 2>/dev/null | grep -Eq "^nginx = 2:${NGXVER}$" || fail "the package does not pin nginx 2:${NGXVER}"
  rpm -qp --conflicts "/pkgs/$BASE" 2>/dev/null | grep -Eq "^nginx-module-pagespeed( |$)" || fail "the package does not conflict with the stock build"
  out="$(dnf -y install "/pkgs/$BASE" 2>&1)" || { echo "$out" | tail -n 8; fail "the package did not install"; }
  after="$(rpm -q --qf "%{EPOCH}:%{VERSION}-%{RELEASE}" nginx)"
  remove() { rpm -e nginx-module-pagespeed-nginxorg >/dev/null; }
  INCL="/usr/share/nginx/modules/*.conf"
  MOD=/usr/lib64/nginx/modules/ngx_pagespeed_module.so
  command -v curl >/dev/null || dnf -q -y install curl-minimal >/dev/null 2>&1
fi
[ "$before" = "$after" ] || fail "installing the module changed nginx from $before to $after"
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
nginx -t 2>&1 | tail -n 1
nginx
for _ in 1 2 3 4 5 6; do curl -s -o /dev/null http://localhost:8080/t.html || fail "nginx is not answering on 8080"; sleep 2; done
curl -sD- -o /dev/null http://localhost:8080/t.html | tr -d "\r" | grep -qiE "^x-page-speed: " || fail "no X-Page-Speed header"
body="$(curl -s http://localhost:8080/t.html)"
echo "$body" | grep -q "body{color:red;margin:0}" || { echo "$body" | head -c 300; fail "the stylesheet was not rewritten"; }
[ -f /var/log/nginx/error.log ] || fail "no error log"
if grep -E "\[(alert|emerg|crit)\]" /var/log/nginx/error.log; then fail "alerts in the log"; fi
nginx -s stop; sleep 1
rm -f /etc/nginx/conf.d/ps.conf
remove || fail "the package did not remove"
[ ! -e "$MOD" ] || fail "the module remained after removal"
nginx -t >/dev/null 2>&1 || fail "nginx -t after removing the package"
echo "nginx=$after, rewrite ok, removal clean"
echo PASS
'
