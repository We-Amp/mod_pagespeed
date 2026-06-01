#!/bin/bash
#
# Copyright (c) 2009 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -e
if [ "$VERBOSE" ]; then
  set -x
fi
set -u

gen_spec() {
  rm -f "${SPEC}"
  process_template "${SCRIPTDIR}/mod-pagespeed.spec.template" "${SPEC}"
}

# Setup the installation directory hierachy in the package staging area.
prep_staging_rpm() {
  prep_staging_common
  install -m 755 -d "${STAGEDIR}/usr/bin"
}

# Put the package contents in the staging area.
stage_install_rpm() {
  prep_staging_rpm
  stage_install_common
  echo "Staging RPM install files in '${STAGEDIR}'..."
  # For CentOS, the load and conf files are combined into a single
  # 'conf' file. So we install the load template as the conf file, and
  # then concatenate the actual conf file.
  process_template "${BUILDDIR}/install/common/pagespeed.load.template" \
    "${STAGEDIR}${APACHE_CONFDIR}/${PAGESPEED_CONF_PREFIX}pagespeed.conf"
  process_template "${BUILDDIR}/install/common/pagespeed.conf.template" \
    "${BUILDDIR}/install/common/pagespeed.conf"
  cat "${BUILDDIR}/install/common/pagespeed.conf" >> \
    "${STAGEDIR}${APACHE_CONFDIR}/${PAGESPEED_CONF_PREFIX}pagespeed.conf"
  if [ "$CPANEL" = true ]; then
    cat "${BUILDDIR}/install/rpm/pagespeed.cpanel.conf" >> \
      "${STAGEDIR}${APACHE_CONFDIR}/${PAGESPEED_CONF_PREFIX}pagespeed.conf"
  fi
  chmod 644 "${STAGEDIR}${APACHE_CONFDIR}/${PAGESPEED_CONF_PREFIX}pagespeed.conf"
  # Install pagespeed_libraries.conf if available
  # Try Bazel output path first, then legacy GYP path
  local LIBRARIES_CONF="${BUILDDIR}/net/instaweb/genfiles/conf/pagespeed_libraries.conf"
  if [ ! -f "${LIBRARIES_CONF}" ]; then
    LIBRARIES_CONF="${BUILDDIR}/../../net/instaweb/genfiles/conf/pagespeed_libraries.conf"
  fi
  if [ -f "${LIBRARIES_CONF}" ]; then
    install -m 644 "${LIBRARIES_CONF}" \
      "${STAGEDIR}${APACHE_CONFDIR}/${PAGESPEED_CONF_PREFIX}pagespeed_libraries.conf"
  fi
}

# Actually generate the package file.
do_package() {
  echo "Packaging ${HOST_ARCH}..."
  PROVIDES="${PACKAGE}"
  REPLACES=""

  # If we specify a dependecy of foo.so below, we would depend on both the
  # 32 and 64-bit versions on a 64-bit machine. The current version of RPM
  # we use is too old and doesn't provide %{_isa}, so we do this manually.
  if [ "$HOST_ARCH" = "x86_64" ] || [ "$HOST_ARCH" = "aarch64" ] ; then
    local EMPTY_VERSION="()"
    local PKG_ARCH="(64bit)"
  fi

  DEPENDS="httpd >= 2.4"
  if [ "$CPANEL" = true ]; then
    DEPENDS="ea-apache24 >= 2.4, \
    ea-apache24-mod_version >= 2.4"
  fi
  DEPENDS="$DEPENDS, \
  libstdc++ >= 4.1.2"
  gen_spec

  # Create temporary rpmbuild dirs.
  RPMBUILD_DIR=$(mktemp -d -t rpmbuild.XXXXXX) || exit 1
  mkdir -p "$RPMBUILD_DIR/BUILD"
  mkdir -p "$RPMBUILD_DIR/RPMS"

  rpmbuild --buildroot="$RPMBUILD_DIR/BUILD" -bb \
    --target="$HOST_ARCH" --rmspec \
    --define "_topdir $RPMBUILD_DIR" \
    --define "_binary_payload w9.bzdio" \
    "${SPEC}"
  PKGNAME="${PACKAGE}-${VERSION}-${REVISION}"
  mv "$RPMBUILD_DIR/RPMS/$HOST_ARCH/${PKGNAME}.${HOST_ARCH}.rpm" "${OUTPUTDIR}"
  # Make sure the package is world-readable, otherwise it causes problems when
  # copied to share drive.
  chmod a+r "${OUTPUTDIR}/${PKGNAME}.$HOST_ARCH.rpm"
  rm -rf "$RPMBUILD_DIR"
}

# Remove temporary files and unwanted packaging output.
cleanup() {
  rm -rf "${STAGEDIR}"
  rm -rf "${TMPFILEDIR}"
}

usage() {
  echo "usage: $(basename $0) [-a target_arch] [-o 'dir'] [-b 'dir'] [-p]"
  echo "-a arch    package architecture (x64 or arm64)"
  echo "-o dir     package output directory [${OUTPUTDIR}]"
  echo "-b dir     build input directory    [${BUILDDIR}]"
  echo "-p         cPanel EasyApache 4 build"
  echo "-c channel (ignored, kept for backward compatibility)"
  echo "-h         this help message"
}

process_opts() {
  while getopts ":o:b:c:a:ph" OPTNAME
  do
    case $OPTNAME in
      o )
        OUTPUTDIR="$OPTARG"
        mkdir -p "${OUTPUTDIR}"
        ;;
      b )
        BUILDDIR=$(readlink -f "${OPTARG}")
        ;;
      c )
        CHANNEL="$OPTARG"
        ;;
      a )
        TARGETARCH="$OPTARG"
        ;;
      p )
        CPANEL=true
        ;;
      h )
        usage
        exit 0
        ;;
      \: )
        echo "'-$OPTARG' needs an argument."
        usage
        exit 1
        ;;
      * )
        echo "invalid command-line option: $OPTARG"
        usage
        exit 1
        ;;
    esac
  done
}

#=========
# MAIN
#=========

SCRIPTDIR=$(readlink -f "$(dirname "$0")")
OUTPUTDIR="${PWD}"
STAGEDIR=$(mktemp -d -t rpm.build.XXXXXX) || exit 1
TMPFILEDIR=$(mktemp -d -t rpm.tmp.XXXXXX) || exit 1
CHANNEL="beta"
# Default target architecture to same as build host.
case "$(uname -m)" in
  x86_64)  TARGETARCH="x64" ;;
  aarch64) TARGETARCH="arm64" ;;
  *)
    echo "ERROR: Unsupported host architecture '$(uname -m)'." >&2
    exit 1
    ;;
esac
SPEC="${TMPFILEDIR}/mod-pagespeed.spec"
CPANEL=false

# call cleanup() on exit
trap cleanup 0
process_opts "$@"
if [ ! "${BUILDDIR:-}" ]; then
  # Default: source tree root (Bazel build)
  BUILDDIR=$(readlink -f "${SCRIPTDIR}/../..")
fi

source "${BUILDDIR}/install/common/installer.include"

get_version_info

source "${BUILDDIR}/install/common/mod-pagespeed/mod-pagespeed.info"
eval $(sed -e "s/^\([^=]\+\)=\(.*\)$/export \1='\2'/" \
  "${BUILDDIR}/install/common/BRANDING")

REPOCONFIG=""

APACHE_CONFDIR="/etc/httpd/conf.d"
MOD_PAGESPEED_CACHE="/var/cache/mod_pagespeed"
MOD_PAGESPEED_LOG="/var/log/pagespeed"
APACHE_USER="apache"
COMMENT_OUT_DEFLATE=
SSL_CERT_DIR="/etc/pki/tls/certs"
SSL_CERT_FILE_COMMAND="ModPagespeedSslCertFile /etc/pki/tls/cert.pem"
APACHE_MODULEDIR_X64="/usr/lib64/httpd/modules"
PAGESPEED_CONF_PREFIX=""

if [ "$CPANEL" = true ]; then
  APACHE_CONFDIR="/etc/apache2/conf.modules.d"
  APACHE_USER="nobody"
  PACKAGE="ea-apache24-$(echo $PACKAGE | tr - _)"
  APACHE_MODULEDIR_X64="/usr/lib64/apache2/modules"
  PAGESPEED_CONF_PREFIX="456_"
  COMMENT_OUT_CRON="\# "
fi

# Make everything happen in the OUTPUTDIR.
cd "${OUTPUTDIR}"

case "$TARGETARCH" in
  x64 )
    export APACHE_MODULEDIR=$APACHE_MODULEDIR_X64
    export HOST_ARCH="x86_64"
    stage_install_rpm
    ;;
  arm64 )
    export APACHE_MODULEDIR=$APACHE_MODULEDIR_X64
    export HOST_ARCH="aarch64"
    stage_install_rpm
    ;;
  * )
    echo
    echo "ERROR: Don't know how to build RPMs for '$TARGETARCH'."
    echo
    exit 1
    ;;
esac

do_package "$HOST_ARCH"
