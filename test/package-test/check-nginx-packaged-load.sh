#!/usr/bin/env bash
#
# check-nginx-packaged-load.sh  —  the design record GATE 1 (packaged-nginx load-check)
#
# THE gate that makes the distro-stock matrix model (a′) safe.
#
# The M0 spike proved nginx dynamic modules are pinned to the EXACT nginx
# version they were compiled against; `--with-compat` does NOT relax this.
# So a mod_pagespeed nginx module built for noble's stock nginx 1.24.0 will
# REFUSE to load on any other nginx version with:
#
#     nginx: [emerg] module "…/ngx_pagespeed_module.so" version 1024000
#            instead of 1026000 in …
#   or
#     nginx: [emerg] module "…" is not binary compatible in …
#
# The EXISTING from-source smoke (test/package-test/Dockerfile.ubuntu-nginx)
# builds nginx from source to MATCH the module version, so it can NEVER catch
# a mismatch against the nginx the distro actually ships. THIS gate installs
# the distro-STOCK PACKAGED nginx (apt/dnf — never from source), then installs
# the freshly built package against it, and runs `nginx -t`. If the module
# version doesn't match the stock nginx, `nginx -t` emits one of the strings
# above and we FAIL the build.
#
# It also asserts the stock nginx is itself a `--with-compat` build, because
# the module is compiled `--with-compat`; loading a `--with-compat` module
# into a non-compat nginx is the "is not binary compatible" failure — we want
# to surface that distinctly rather than have the whole gate look "green
# because nginx happened to also be compat".
#
# NEGATIVE CONTROL (how this gate catches a real mismatch):
#   If Phase B ever (re)builds the module against the WRONG nginx — e.g. a
#   newer nginx than the distro ships, or the distro bumps stock nginx and the
#   pin in Depends/Requires is not refreshed — then `dpkg -i` / `dnf install`
#   either refuses on the dependency (Depends: nginx (= <ver>)) OR, if forced,
#   `nginx -t` prints `module "…" version <X> instead of <Y>` and this script
#   exits non-zero. Either way the release is BLOCKED before publish. You can
#   reproduce the failure locally by pointing this script at a package built
#   for a different nginx version than the container's stock nginx.
#
# This script is meant to run INSIDE a clean per-distro container:
#   ubuntu:24.04   (noble,  stock nginx 1.24.0)   — package = .deb
#   almalinux:9    (el9,    stock nginx 1.20.1)   — package = .rpm
# The CI job (release.yml: package-load-check-nginx) spawns those containers,
# bind-mounts the built package + this script, and runs it.
#
# Usage (inside the container):
#   check-nginx-packaged-load.sh <distro> <package-path>
#     <distro>       = noble | bullseye | bookworm | trixie | jammy | el9
#     <package-path> = path to the freshly built .deb (Debian/Ubuntu) / .rpm (el9)
#
# Exit codes:
#   0  module loads cleanly against distro-stock nginx
#   1  usage / environment error
#   2  load-check assertion failed (version mismatch / not-compat / nginx -t)
#      — THE blocking condition
#
set -euo pipefail

die() { echo "::error::$*" >&2; exit "${2:-1}"; }

[[ $# -eq 2 ]] || die "usage: $0 <noble|bullseye|bookworm|trixie|jammy|el9> <package-path>" 1
DISTRO="$1"
PKG="$2"
[[ -f "$PKG" ]] || die "package not found: $PKG" 1

# Version-mismatch / not-compatible signatures that `nginx -t` (or the load
# attempt) emits. Matching ANY of these is a hard FAIL.
MISMATCH_RE='version [0-9]+ instead of [0-9]+|is not binary compatible'

install_stock_nginx_apt() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  # IMPORTANT: install the distro-STOCK packaged nginx (NOT from source).
  # This is the whole point of the gate — exercise the real nginx the
  # customer's apt would pull, against which the module version is pinned.
  apt-get install -y --no-install-recommends nginx ca-certificates >/dev/null
}

install_stock_nginx_dnf() {
  # el9 ships nginx in AppStream. Stock packaged nginx (NOT from source).
  dnf install -y nginx >/dev/null
}

install_pkg_apt() {
  # `apt-get install ./pkg.deb` resolves the `Depends: nginx (= <ver>)` pin
  # against the already-installed stock nginx. If the pin doesn't match the
  # stock version, apt refuses here (first line of defence). We capture the
  # output so a dependency refusal is reported as a load-check failure too.
  local out
  if ! out="$(apt-get install -y "./$(basename "$PKG")" 2>&1)"; then
    echo "$out" >&2
    if echo "$out" | grep -qE 'nginx'; then
      die "package refused to install against stock nginx (Depends pin mismatch?):
$out" 2
    fi
    die "dpkg/apt install failed:
$out" 2
  fi
  echo "$out"
}

install_pkg_dnf() {
  local out
  if ! out="$(dnf install -y "$PKG" 2>&1)"; then
    echo "$out" >&2
    if echo "$out" | grep -qiE 'nginx'; then
      die "package refused to install against stock nginx (Requires pin mismatch?):
$out" 2
    fi
    die "dnf install failed:
$out" 2
  fi
  echo "$out"
}

echo "==> the design record GATE 1: packaged-nginx load-check (distro=$DISTRO)"
echo "    package: $PKG"

case "$DISTRO" in
  # the design record P3: all Debian/Ubuntu deb suites take the same apt install path —
  # stock packaged nginx auto-includes the .deb's modules-enabled load_module
  # snippet, so the gate logic below is identical across them.
  noble|bullseye|bookworm|trixie|jammy)
    install_stock_nginx_apt
    cp "$PKG" "./$(basename "$PKG")"
    install_pkg_apt
    ;;
  el9)
    install_stock_nginx_dnf
    install_pkg_dnf
    ;;
  *)
    die "unknown distro '$DISTRO' (want noble|bullseye|bookworm|trixie|jammy|el9)" 1
    ;;
esac

# ---------------------------------------------------------------------------
# Assert the stock nginx is a --with-compat build. The module is compiled
# --with-compat; a non-compat host nginx triggers "is not binary compatible".
# ---------------------------------------------------------------------------
echo "--- nginx -V ---"
NGINX_V="$(nginx -V 2>&1)"
echo "$NGINX_V"
STOCK_VER="$(echo "$NGINX_V" | sed -nE 's#^nginx version: nginx/([0-9.]+).*#\1#p' | head -1)"
echo "    stock nginx version: ${STOCK_VER:-<unknown>}"
if ! echo "$NGINX_V" | grep -- '--with-compat' >/dev/null 2>&1; then
  die "stock nginx is NOT a --with-compat build — the --with-compat module cannot load (would report 'is not binary compatible')" 2
fi
echo "OK: stock nginx is a --with-compat build"

# ---------------------------------------------------------------------------
# The load test. `nginx -t` parses the config, which includes the
# load_module snippet the package dropped (modules-enabled symlink on Debian,
# conf.d snippet on el9). A version mismatch surfaces HERE.
# ---------------------------------------------------------------------------
echo "--- nginx -t (loads the packaged module via the package's load_module snippet) ---"
NGINX_T="$(nginx -t 2>&1 || true)"
echo "$NGINX_T"

if echo "$NGINX_T" | grep -qE "$MISMATCH_RE"; then
  echo "::error::MODULE/NGINX VERSION MISMATCH — the packaged module does not match stock nginx $STOCK_VER:" >&2
  echo "$NGINX_T" | grep -E "$MISMATCH_RE" >&2
  die "nginx refused to load the module — version pin is wrong for $DISTRO (stock nginx $STOCK_VER)" 2
fi

# Belt and braces: `nginx -t` must actually succeed. A non-mismatch failure
# (missing dir, bad snippet) is also a blocking gate failure.
if ! nginx -t >/dev/null 2>&1; then
  die "nginx -t failed (non-version-mismatch):
$NGINX_T" 2
fi

# Confirm the module .so is actually referenced (guards against a no-op pass
# where the package installed but dropped no load_module directive).
if ! { nginx -T 2>/dev/null || true; } | grep -q 'ngx_pagespeed_module.so'; then
  echo "::warning::nginx -t passed but no load_module for ngx_pagespeed_module.so was found in the effective config — verify the package's load_module snippet is enabled"
fi

echo "==> GATE 1 PASSED: packaged module loads cleanly into distro-stock nginx ${STOCK_VER:-?} (--with-compat) on $DISTRO"
