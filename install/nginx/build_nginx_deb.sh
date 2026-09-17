#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# build_nginx_deb.sh — package the 1.1 ngx_pagespeed dynamic module as
# nginx-module-pagespeed_<ver>-r<N>_amd64.deb for Ubuntu 24.04 (noble).
#
# Model (a') scoped distro-stock matrix: nginx dynamic modules carry an exact
# nginx_version signature that --with-compat does NOT relax (M0, 2026-05-27), so
# the .deb hard-pins `Depends: nginx (= <noble upstream version>)`. The module
# .so MUST be built/linked INSIDE an ubuntu:24.04 container against noble's stock
# nginx 1.24.0 source so its glibc/libstdc++ ABI matches the target (EA4 glibc
# lesson). This script wraps that already-built .so; it does not build it.
#
# Usage:
#   ./install/nginx/build_nginx_deb.sh [-o output_dir] [-s module.so] [-a amd64]
#
# Inputs:
#   -s  path to the prebuilt ngx_pagespeed_module.so (noble-targeted).
#       Default: bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so (legacy/fallback).
#   -o  output directory for the .deb (default: cwd).
#   -a  dpkg architecture (default: amd64).
#
# Version/revision come from net/instaweb/public/VERSION + the +rN tag
# convention via install/common/installer.include get_version_info (shared with
# the Apache .deb so the two stay in lockstep).

set -e
if [ "${VERBOSE:-}" ]; then set -x; fi
set -u

SCRIPTDIR=$(readlink -f "$(dirname "$0")")
SRCDIR=$(readlink -f "${SCRIPTDIR}/../..")
OUTPUTDIR="${PWD}"
SO_PATH=""
TARGETARCH="amd64"

usage() {
  echo "usage: $(basename "$0") [-o output_dir] [-s module.so] [-a amd64]"
  echo "  -o dir     .deb output directory       [${OUTPUTDIR}]"
  echo "  -s file    prebuilt ngx_pagespeed_module.so (noble-targeted)"
  echo "  -a arch    dpkg architecture            [${TARGETARCH}]"
  echo "  -h         this help"
}

while getopts ":o:s:a:h" opt; do
  case $opt in
    o) OUTPUTDIR=$(readlink -f "$OPTARG"); mkdir -p "${OUTPUTDIR}" ;;
    s) SO_PATH=$(readlink -f "$OPTARG") ;;
    a) TARGETARCH="$OPTARG" ;;
    h) usage; exit 0 ;;
    :) echo "'-$OPTARG' needs an argument."; usage; exit 1 ;;
    *) echo "invalid option: -$OPTARG"; usage; exit 1 ;;
  esac
done

# Default .so location (Bazel convenience symlink) — but the GA path passes -s
# to the in-container ubuntu:24.04 build output.
if [ -z "${SO_PATH}" ]; then
  SO_PATH="${SRCDIR}/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so"
fi
if [ ! -f "${SO_PATH}" ]; then
  echo "ERROR: ngx_pagespeed_module.so not found at ${SO_PATH}" >&2
  echo "Build it inside ubuntu:24.04 against noble's stock nginx 1.24.0 first." >&2
  exit 1
fi

# --- version stamping (shared machinery with the Apache .deb) ---------------
BUILDDIR="${SRCDIR}"
source "${SRCDIR}/install/common/installer.include"
get_version_info

# Per-suite version suffix. Each distro ships a DISTINCT .so (exact
# stock-nginx ABI + glibc floor), so the .deb version MUST encode the suite:
# apt/aptly identify a package by name+version+arch, and two suites sharing those
# three with different .so bytes would collide in the aptly pool and serve the
# wrong module. Derive the codename from the build container's /etc/os-release
# (this script runs INSIDE the target-distro container, after
# build_module_in_container.sh) and append ~<codename>. The corp publish
# classifier (classify_deb_suite) routes each .deb to dists/<codename> by this
# suffix. ~<codename> is the conventional Debian "sorts-lower" separator, so a
# routine +rN reship (e.g. 1.15.0-r2~bookworm > 1.15.0-r1~bookworm) upgrades
# cleanly within a suite.
CODENAME="${CODENAME:-}"
if [ -z "${CODENAME}" ] && [ -r /etc/os-release ]; then
  CODENAME="$(. /etc/os-release 2>/dev/null && echo "${VERSION_CODENAME:-}")"
fi
[ -n "${CODENAME}" ] || { echo "ERROR: cannot determine distro codename (set CODENAME or run inside the target-distro container)" >&2; exit 1; }
VERSIONFULL="${VERSION}-r${REVISION}~${CODENAME}"

PACKAGE="nginx-module-pagespeed"   # D2: SEO-parity name (NOT mod-pagespeed-nginx)
MAINTNAME="mod_pagespeed developers"
MAINTMAIL="info@we-amp.com"
PRODUCTURL="https://github.com/we-amp/mod_pagespeed/"

# --- exact-version nginx pin (model (a')) -----------------------------------
# Confirm the noble stock nginx upstream version and pin to its upstream-version
# component (the bit before the Debian revision), so routine packaging-revision
# security updates (1.24.0-2ubuntu7.8 -> ...7.9) do NOT break the dep, but a
# minor rebase (1.24.0 -> 1.26.x) does — caught by the build-time load-check.
# We resolve it at build time from the running container's apt metadata when
# available, falling back to the GA-confirmed 1.24.0.
NGINX_UPSTREAM_VERSION="${NGINX_UPSTREAM_VERSION:-}"
if [ -z "${NGINX_UPSTREAM_VERSION}" ] && command -v apt-cache >/dev/null 2>&1; then
  _cand=$(apt-cache policy nginx-common 2>/dev/null | awk '/Candidate:/{print $2}')
  [ -z "${_cand}" ] && _cand=$(apt-cache policy nginx 2>/dev/null | awk '/Candidate:/{print $2}')
  # Strip epoch and the Debian revision -> upstream version (e.g. 1.24.0).
  _cand="${_cand#*:}"
  NGINX_UPSTREAM_VERSION="${_cand%%-*}"
fi
NGINX_UPSTREAM_VERSION="${NGINX_UPSTREAM_VERSION:-1.24.0}"
# Pin to the EXACT upstream version while allowing any Debian packaging
# revision. dpkg has no '=' wildcard, so express the upstream-version pin as a
# half-open range: >= <ver>  &&  << <ver+1-patch>. Any 1.24.0-<anything>
# satisfies it (routine security re-revs are fine); a minor rebase to 1.26.x
# does NOT (caught by the build-time load-check, then a +rN reship). Derive the
# upper bound by bumping the LAST numeric component of the upstream version.
_uv="${NGINX_UPSTREAM_VERSION}"
_last="${_uv##*.}"; _prefix="${_uv%.*}"
NGINX_UPPER="${_prefix}.$((_last + 1))"
NGINX_DEP="nginx (>= ${_uv}), nginx (<< ${NGINX_UPPER})"

echo "Packaging ${PACKAGE} ${VERSIONFULL} (${TARGETARCH}); pin: ${NGINX_DEP}"

# --- staging ----------------------------------------------------------------
STAGEDIR=$(mktemp -d -t ngxdeb.build.XXXXXX)
trap 'rm -rf "${STAGEDIR}"' EXIT

NGINX_MODULEDIR="/usr/lib/nginx/modules"
NGINX_MODAVAIL="/usr/share/nginx/modules-available"
NGINX_MODENABLED="/etc/nginx/modules-enabled"
NGINX_DOCDIR="/usr/share/doc/${PACKAGE}"

install -m 755 -d \
  "${STAGEDIR}/DEBIAN" \
  "${STAGEDIR}${NGINX_MODULEDIR}" \
  "${STAGEDIR}${NGINX_MODAVAIL}" \
  "${STAGEDIR}${NGINX_MODENABLED}" \
  "${STAGEDIR}${NGINX_DOCDIR}"

# The module .so.
install -m 644 "${SO_PATH}" "${STAGEDIR}${NGINX_MODULEDIR}/ngx_pagespeed_module.so"

# nginx.org module-package layout: ship the load_module snippet in
# modules-available/ and symlink it into modules-enabled/, which stock noble's
# nginx.conf auto-includes (`include /etc/nginx/modules-enabled/*.conf;`).
# A high-numbered prefix keeps load ordering predictable relative to other
# nginx-module-* packages (PageSpeed is an aux filter; load late).
cat > "${STAGEDIR}${NGINX_MODAVAIL}/mod-pagespeed.conf" <<'SNIP'
# ngx_pagespeed dynamic module (We-Amp mod_pagespeed 1.15).
# Auto-included by stock noble nginx via /etc/nginx/modules-enabled/.
load_module modules/ngx_pagespeed_module.so;
SNIP
chmod 644 "${STAGEDIR}${NGINX_MODAVAIL}/mod-pagespeed.conf"
# Symlink /etc/nginx/modules-enabled/50-mod-pagespeed.conf ->
# /usr/share/nginx/modules-available/mod-pagespeed.conf. From modules-enabled/,
# `../../../` reaches `/`, so the relative target is
# ../../../usr/share/nginx/modules-available/... (matches nginx.org's layout).
ln -s "../../../usr/share/nginx/modules-available/mod-pagespeed.conf" \
  "${STAGEDIR}${NGINX_MODENABLED}/50-mod-pagespeed.conf"

# Sample config + admin-restrict snippet + README (authored below; verbatim from
# build_nginx_package.sh with the corrected compat string + the cache note).
"${SCRIPTDIR}/nginx_package_docs.sh" "${STAGEDIR}${NGINX_DOCDIR}" "${VERSION}" "deb" \
  "${NGINX_UPSTREAM_VERSION}"

# --- DEBIAN control ---------------------------------------------------------
INSTSIZE=$(du -k -s "${STAGEDIR}" | cut -f1)
cat > "${STAGEDIR}/DEBIAN/control" <<EOF
Package: ${PACKAGE}
Version: ${VERSIONFULL}
Section: httpd
Priority: optional
Architecture: ${TARGETARCH}
Depends: ${NGINX_DEP}
Maintainer: ${MAINTNAME} <${MAINTMAIL}>
Installed-Size: ${INSTSIZE}
Homepage: ${PRODUCTURL}
Description: nginx module to optimize web content (mod_pagespeed 1.15)
 ngx_pagespeed is the nginx port of mod_pagespeed: it rewrites web pages and
 their resources (CSS, JavaScript, images) to apply web performance best
 practices automatically. Pinned to the exact stock nginx version it was built
 against — nginx refuses to load a dynamic module built for a different
 version.
EOF
chmod 644 "${STAGEDIR}/DEBIAN/control"

# conffiles: the load snippet is operator-editable config.
cat > "${STAGEDIR}/DEBIAN/conffiles" <<EOF
${NGINX_MODAVAIL}/mod-pagespeed.conf
EOF

# postinst/postrm: keep nginx -t clean across install/remove, and join the
# web-server user to the optimizer daemon's group.
cat > "${STAGEDIR}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
if [ "$1" = "configure" ]; then
  # Join the web-server user to the `pagespeed` group. After the optimizer
  # daemon's privilege drop (2.1) its cache volume, notify socket and shared
  # config are group-rw (0660/0640 pagespeed:pagespeed), and group membership
  # is what lets the module reach them. Stock Debian/Ubuntu nginx runs as
  # www-data. Guarded and idempotent; a no-op when the optimizer package
  # (which creates the group) is not installed, and a usermod failure must not
  # fail the install: the module logs the degraded state loudly at startup
  # instead.
  if getent group pagespeed >/dev/null 2>&1 && id www-data >/dev/null 2>&1; then
    usermod -a -G pagespeed www-data || true
  fi
fi
exit 0
EOF
chmod 755 "${STAGEDIR}/DEBIAN/postinst"

cat > "${STAGEDIR}/DEBIAN/postrm" <<'EOF'
#!/bin/sh
# Cache/log dirs are preserved on purpose (standard Unix convention).
exit 0
EOF
chmod 755 "${STAGEDIR}/DEBIAN/postrm"

cat > "${STAGEDIR}/DEBIAN/prerm" <<EOF
#!/bin/sh
# On removal, neutralize the load_module snippet so a dangling .so reference
# cannot wedge \`nginx -t\` between file removal and the symlink going away.
if [ "\$1" = "remove" ] && [ -L ${NGINX_MODENABLED}/50-mod-pagespeed.conf ]; then
  rm -f ${NGINX_MODENABLED}/50-mod-pagespeed.conf || true
fi
exit 0
EOF
chmod 755 "${STAGEDIR}/DEBIAN/prerm"

# --- D4 symbol-leak assertion (the deterministic gate Phase C reuses) -------
"${SCRIPTDIR}/assert_symbol_hygiene.sh" \
  "${STAGEDIR}${NGINX_MODULEDIR}/ngx_pagespeed_module.so"

# --- build the .deb ---------------------------------------------------------
DEB_OUT="${OUTPUTDIR}/${PACKAGE}_${VERSIONFULL}_${TARGETARCH}.deb"
# fakeroot keeps root:root ownership without needing real root.
if command -v fakeroot >/dev/null 2>&1; then
  fakeroot dpkg-deb -Zgzip --build "${STAGEDIR}" "${DEB_OUT}"
else
  dpkg-deb -Zgzip --build "${STAGEDIR}" "${DEB_OUT}"
fi
chmod a+r "${DEB_OUT}"
echo "Package created: ${DEB_OUT}"
