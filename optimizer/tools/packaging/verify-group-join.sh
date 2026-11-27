#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Verifies that installing the optimizer package leaves the web-server user in
# group `pagespeed` whatever order the serving module and the optimizer are
# installed in (the module's own scriptlet only joins the group if it already
# exists, and the two packages have no install-order dependency).
#
# Host mode (needs docker and network access to packages.modpagespeed.com):
#   verify-group-join.sh --distro ubuntu:24.04|debian:12|almalinux:9 \
#       --package PATH.deb|PATH.rpm [--published]
#   Runs every scenario below in its own throwaway container.  --published
#   installs the released optimizer instead of --package (negative control:
#   the worker-after-module scenarios are expected to FAIL there).
#
# Container mode (as root, inside a clean image):
#   verify-group-join.sh --scenario worker-first|module-first|together|upgrade \
#       --package PATH [--published]
#
#   worker-first  nginx, then the optimizer, then the module
#   module-first  nginx, then the module, then the optimizer
#   together      nginx, then optimizer and module in ONE transaction
#   upgrade       module, then the released optimizer (member list stays
#                 empty), then --package over it
set -euo pipefail

DISTRO="" PKG="" SCENARIO="" PUBLISHED=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --distro) DISTRO="$2"; shift 2 ;;
    --package) PKG="$2"; shift 2 ;;
    --scenario) SCENARIO="$2"; shift 2 ;;
    --published) PUBLISHED=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -n "$DISTRO" ]]; then
  [[ -f "$PKG" ]] || { echo "error: --package PATH required" >&2; exit 2; }
  me="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
  rc=0
  for s in worker-first module-first together upgrade; do
    name="groupjoin-$$-$s"
    if docker run --rm --name "$name" -v "$me:/verify.sh:ro" \
         -v "$(cd "$(dirname "$PKG")" && pwd)/$(basename "$PKG"):/pkg/$(basename "$PKG"):ro" \
         "$DISTRO" bash /verify.sh --scenario "$s" --package "/pkg/$(basename "$PKG")" \
         $([[ $PUBLISHED = 1 ]] && echo --published); then
      echo "RESULT $DISTRO $s: PASS"
    else
      echo "RESULT $DISTRO $s: FAIL"; rc=1
    fi
  done
  exit $rc
fi

[[ -n "$SCENARIO" && -n "$PKG" ]] || { echo "usage: see header" >&2; exit 2; }

if command -v apt-get >/dev/null 2>&1; then
  FAMILY=deb; WEBUSER=www-data
  export DEBIAN_FRONTEND=noninteractive
  apt-get -qq update >/dev/null
  # systemd supplies systemd-sysusers, which creates the group on a real host
  apt-get -qq -y install curl ca-certificates gnupg systemd >/dev/null
  curl -fsSL https://packages.modpagespeed.com/install.sh | sh >/dev/null
  apt-get -qq update >/dev/null
  inst() { apt-get -qq -y install "$@" >/dev/null; }
  inst nginx
  MODULE=nginx-module-pagespeed
  WORKER_PUB=pagespeed-optimizer
else
  FAMILY=rpm; WEBUSER=nginx
  dnf -q -y install ca-certificates >/dev/null  # curl-minimal ships in the image
  curl -fsSL https://packages.modpagespeed.com/install.sh | sh >/dev/null
  inst() { dnf -q -y install "$@" >/dev/null; }
  inst nginx
  MODULE=nginx-module-pagespeed
  WORKER_PUB=pagespeed-optimizer
fi

WORKER_NEW="$PKG"; [[ $PUBLISHED = 1 ]] && WORKER_NEW="$WORKER_PUB"

member() { getent group pagespeed | cut -d: -f4 | tr ',' '\n' | grep -qx "$WEBUSER"; }
assert_member() {
  if member; then echo "ok: $1: $WEBUSER is in group pagespeed ($(getent group pagespeed))"
  else echo "FAIL: $1: $WEBUSER missing from group pagespeed ($(getent group pagespeed || echo 'no group'))" >&2; exit 1; fi
}

case "$SCENARIO" in
  worker-first)
    inst "$WORKER_NEW"; inst "$MODULE"; assert_member "worker then module" ;;
  module-first)
    inst "$MODULE"
    if member; then echo "note: module alone already joined (group pre-exists?)"; fi
    inst "$WORKER_NEW"; assert_member "module then worker" ;;
  together)
    inst "$WORKER_NEW" "$MODULE"; assert_member "one transaction" ;;
  upgrade)
    inst "$MODULE"; inst "$WORKER_PUB"
    if member; then echo "note: released worker already left $WEBUSER in the group"
    else echo "ok: precondition: released worker after module leaves $WEBUSER out (the bug)"; fi
    if [[ $FAMILY = deb ]]; then apt-get -qq -y install "$PKG" >/dev/null; else dnf -q -y upgrade "$PKG" >/dev/null; fi
    assert_member "upgrade over the released worker"
    # idempotent: a second install must not duplicate or fail
    if [[ $FAMILY = deb ]]; then apt-get -qq -y --reinstall install "$PKG" >/dev/null; else dnf -q -y reinstall "$PKG" >/dev/null; fi
    n="$(getent group pagespeed | cut -d: -f4 | tr ',' '\n' | grep -cx "$WEBUSER")"
    [[ "$n" = 1 ]] || { echo "FAIL: $WEBUSER listed $n times after reinstall" >&2; exit 1; }
    echo "ok: reinstall is idempotent" ;;
  *) echo "unknown scenario $SCENARIO" >&2; exit 2 ;;
esac
