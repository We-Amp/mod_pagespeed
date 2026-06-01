#!/bin/bash
#
# build_nginx_rpm.sh — package the 1.1 ngx_pagespeed dynamic module as
# nginx-module-pagespeed-<ver>-<N>.el9.x86_64.rpm for AlmaLinux/RHEL/Rocky 9.
#
# Model (a') scoped distro-stock matrix: the .rpm hard-pins
# `Requires: nginx = <el9 stock upstream version>` because nginx refuses to load
# a dynamic module whose embedded nginx_version != the running nginx (M0,
# 2026-05-27). The module .so MUST be built/linked INSIDE an almalinux:9
# container against el9's stock nginx 1.20.1 source so its glibc/libstdc++ ABI
# matches the target (EA4 glibc lesson). This script wraps that already-built
# .so; it does not build it. Run it inside almalinux:9 (or any host with rpmbuild
# + the el9-targeted .so).
#
# Usage:
#   ./install/nginx/build_nginx_rpm.sh [-o output_dir] [-s module.so] [-a x86_64]
#
# Inputs:
#   -s  prebuilt ngx_pagespeed_module.so (el9-targeted).
#       Default: bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so (legacy/fallback).
#   -o  output directory for the .rpm (default: cwd).
#   -a  rpm architecture (default: x86_64).

set -e
if [ "${VERBOSE:-}" ]; then set -x; fi
set -u

SCRIPTDIR=$(readlink -f "$(dirname "$0")")
SRCDIR=$(readlink -f "${SCRIPTDIR}/../..")
OUTPUTDIR="${PWD}"
SO_PATH=""
HOST_ARCH="x86_64"

usage() {
  echo "usage: $(basename "$0") [-o output_dir] [-s module.so] [-a x86_64]"
  echo "  -o dir     .rpm output directory       [${OUTPUTDIR}]"
  echo "  -s file    prebuilt ngx_pagespeed_module.so (el9-targeted)"
  echo "  -a arch    rpm architecture            [${HOST_ARCH}]"
  echo "  -h         this help"
}

while getopts ":o:s:a:h" opt; do
  case $opt in
    o) OUTPUTDIR=$(readlink -f "$OPTARG"); mkdir -p "${OUTPUTDIR}" ;;
    s) SO_PATH=$(readlink -f "$OPTARG") ;;
    a) HOST_ARCH="$OPTARG" ;;
    h) usage; exit 0 ;;
    :) echo "'-$OPTARG' needs an argument."; usage; exit 1 ;;
    *) echo "invalid option: -$OPTARG"; usage; exit 1 ;;
  esac
done

if [ -z "${SO_PATH}" ]; then
  SO_PATH="${SRCDIR}/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so"
fi
if [ ! -f "${SO_PATH}" ]; then
  echo "ERROR: ngx_pagespeed_module.so not found at ${SO_PATH}" >&2
  echo "Build it inside almalinux:9 against el9's stock nginx 1.20.1 first." >&2
  exit 1
fi

# --- version stamping (shared machinery with the Apache .rpm) ---------------
BUILDDIR="${SRCDIR}"
source "${SRCDIR}/install/common/installer.include"
get_version_info

PACKAGE="nginx-module-pagespeed"   # D2: SEO-parity name
COMPANY_FULLNAME="We-Amp"
MAINTNAME="mod_pagespeed developers"
MAINTMAIL="info@we-amp.com"
PRODUCTURL="https://github.com/we-amp/mod_pagespeed/"
SHORTDESC="nginx module to optimize web content (mod_pagespeed 1.15)."
FULLDESC="ngx_pagespeed is the nginx port of mod_pagespeed: it rewrites web pages and their resources (CSS, JavaScript, images) to apply web performance best practices automatically. Free to install; optimization activates with a valid license token (BYOL). Pinned to the exact stock nginx version it was built against."

# --- exact-version nginx pin (model (a')) -----------------------------------
# Confirm the el9 stock nginx upstream version + EPOCH and pin to it exactly.
# CRITICAL: AlmaLinux/RHEL nginx ships with EPOCH 2 (provides `nginx = 2:1.20.1`),
# so `Requires: nginx = 1.20.1` (implicit epoch 0) does NOT match and the package
# refuses to install ("nothing provides nginx = 1.20.1"). The Requires MUST carry
# the epoch. Resolve both at build time from dnf metadata; fall back to the
# GA-confirmed epoch:version 2:1.20.1.
NGINX_UPSTREAM_VERSION="${NGINX_UPSTREAM_VERSION:-}"
NGINX_EPOCH="${NGINX_EPOCH:-}"
if command -v dnf >/dev/null 2>&1; then
  [ -z "${NGINX_UPSTREAM_VERSION}" ] && NGINX_UPSTREAM_VERSION=$(dnf -q info nginx 2>/dev/null | awk '/^Version/{print $3; exit}')
  [ -z "${NGINX_EPOCH}" ] && NGINX_EPOCH=$(dnf -q info nginx 2>/dev/null | awk '/^Epoch/{print $3; exit}')
fi
NGINX_UPSTREAM_VERSION="${NGINX_UPSTREAM_VERSION:-1.20.1}"
NGINX_EPOCH="${NGINX_EPOCH:-2}"
# Pin to <epoch>:<upstream-version> exactly. Any el9 packaging revision of that
# epoch:version satisfies it (routine security re-revs); a minor rebase (1.22/1.24)
# does NOT — caught by the build-time load-check, then a +rN reship.
NGINX_DEP="nginx = ${NGINX_EPOCH}:${NGINX_UPSTREAM_VERSION}"
DIST=".el9"

echo "Packaging ${PACKAGE} ${VERSION}-${REVISION}${DIST} (${HOST_ARCH}); pin: Requires ${NGINX_DEP}"

# --- el9 module-load layout -------------------------------------------------
# Stock el9 nginx.conf includes /usr/share/nginx/modules/*.conf from the main
# context AND /etc/nginx/conf.d/*.conf from http{}. load_module must be in the
# MAIN context (before http{}), so we drop the snippet under
# /usr/share/nginx/modules/ — exactly where nginx.org's el9 module RPMs put it
# (e.g. nginx-module-njs ships /usr/share/nginx/modules/njs.conf).
NGINX_MODULEDIR="/usr/lib64/nginx/modules"
NGINX_LOADDIR="/usr/share/nginx/modules"
NGINX_DOCDIR="/usr/share/doc/${PACKAGE}"

# --- staging ----------------------------------------------------------------
STAGEDIR=$(mktemp -d -t ngxrpm.build.XXXXXX)
TMPFILEDIR=$(mktemp -d -t ngxrpm.tmp.XXXXXX)
trap 'rm -rf "${STAGEDIR}" "${TMPFILEDIR}"' EXIT

install -m 755 -d \
  "${STAGEDIR}${NGINX_MODULEDIR}" \
  "${STAGEDIR}${NGINX_LOADDIR}" \
  "${STAGEDIR}${NGINX_DOCDIR}"

install -m 644 "${SO_PATH}" "${STAGEDIR}${NGINX_MODULEDIR}/ngx_pagespeed_module.so"

# Use the ABSOLUTE module path — stock el9 nginx resolves a relative
# `load_module modules/...` against the snippet's own dir (/usr/share/nginx/
# modules/), NOT the --modules-path (/usr/lib64/nginx/modules/), so a relative
# path dlopen-fails. nginx.org's own el9 module RPMs use the absolute path too.
cat > "${STAGEDIR}${NGINX_LOADDIR}/mod-pagespeed.conf" <<SNIP
# ngx_pagespeed dynamic module (We-Amp mod_pagespeed 1.15).
# Auto-included by stock el9 nginx via the main-context
# \`include /usr/share/nginx/modules/*.conf;\` (loads before http{}).
load_module "${NGINX_MODULEDIR}/ngx_pagespeed_module.so";
SNIP
chmod 644 "${STAGEDIR}${NGINX_LOADDIR}/mod-pagespeed.conf"

"${SCRIPTDIR}/nginx_package_docs.sh" "${STAGEDIR}${NGINX_DOCDIR}" "${VERSION}" "rpm" \
  "${NGINX_UPSTREAM_VERSION}"

# --- D4 symbol-leak assertion (deterministic gate, also wired in CI) --------
"${SCRIPTDIR}/assert_symbol_hygiene.sh" \
  "${STAGEDIR}${NGINX_MODULEDIR}/ngx_pagespeed_module.so"

# --- spec -------------------------------------------------------------------
export PACKAGE COMPANY_FULLNAME VERSION REVISION DIST PRODUCTURL MAINTNAME \
  MAINTMAIL SHORTDESC FULLDESC NGINX_DEP NGINX_MODULEDIR NGINX_LOADDIR NGINX_DOCDIR \
  STAGEDIR
# Reuse installer.include's process_template (@@VAR@@ substitution) with the
# nginx-specific variables. process_template references many Apache vars; set
# the unused ones empty so set+u inside process_template doesn't matter.
: "${PRERELEASE:=}"
SPEC="${TMPFILEDIR}/nginx-module-pagespeed.spec"

# process_template only knows the Apache variable names; the nginx spec template
# uses @@NGINX_DEP@@/@@NGINX_MODULEDIR@@/@@NGINX_LOADDIR@@/@@NGINX_DOCDIR@@/@@DIST@@
# which it does not substitute. Do a focused sed instead of the Apache helper.
sed \
  -e "s#@@PACKAGE@@#${PACKAGE}#g" \
  -e "s#@@COMPANY_FULLNAME@@#${COMPANY_FULLNAME}#g" \
  -e "s#@@VERSION@@#${VERSION}#g" \
  -e "s#@@REVISION@@#${REVISION}#g" \
  -e "s#@@DIST@@#${DIST}#g" \
  -e "s#@@PRODUCTURL@@#${PRODUCTURL}#g" \
  -e "s#@@MAINTNAME@@#${MAINTNAME}#g" \
  -e "s#@@MAINTMAIL@@#${MAINTMAIL}#g" \
  -e "s#@@SHORTDESC@@#${SHORTDESC}#g" \
  -e "s#@@FULLDESC@@#${FULLDESC}#g" \
  -e "s#@@NGINX_DEP@@#${NGINX_DEP}#g" \
  -e "s#@@NGINX_MODULEDIR@@#${NGINX_MODULEDIR}#g" \
  -e "s#@@NGINX_LOADDIR@@#${NGINX_LOADDIR}#g" \
  -e "s#@@NGINX_DOCDIR@@#${NGINX_DOCDIR}#g" \
  -e "s#@@STAGEDIR@@#${STAGEDIR}#g" \
  "${SCRIPTDIR}/nginx-module-pagespeed.spec.template" > "${SPEC}"
if grep -q "@@" "${SPEC}"; then
  echo "ERROR: spec contains unsubstituted @@-variables:" >&2
  grep "@@" "${SPEC}" >&2
  exit 1
fi

# --- build the .rpm ---------------------------------------------------------
RPMBUILD_DIR=$(mktemp -d -t rpmbuild.XXXXXX)
mkdir -p "${RPMBUILD_DIR}/BUILD" "${RPMBUILD_DIR}/RPMS"
rpmbuild --buildroot="${RPMBUILD_DIR}/BUILD" -bb \
  --target="${HOST_ARCH}" --rmspec \
  --define "_topdir ${RPMBUILD_DIR}" \
  --define "_binary_payload w9.bzdio" \
  "${SPEC}"
PKGNAME="${PACKAGE}-${VERSION}-${REVISION}${DIST}"
RPM_OUT="${OUTPUTDIR}/${PKGNAME}.${HOST_ARCH}.rpm"
mv "${RPMBUILD_DIR}/RPMS/${HOST_ARCH}/${PKGNAME}.${HOST_ARCH}.rpm" "${RPM_OUT}"
chmod a+r "${RPM_OUT}"
rm -rf "${RPMBUILD_DIR}"
echo "Package created: ${RPM_OUT}"
