#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Build the Apache module .so (//:libmod_pagespeed.so) NATIVELY on EL10 (glibc
# 2.39) so it loads on RHEL/Rocky/AlmaLinux/CloudLinux 10, then wrap it into the
# stock mod-pagespeed-<ver>.x86_64.rpm that the EA4 repackager spec
# (packaging/cpanel/ea-apache24-mod_pagespeed.spec) consumes AND that the plain
# dnf/yum el10 channel ships.
#
# WHY a native el10 build (the EL8 glibc lesson): the
# default Apache .so is built once in the pagespeed1.1-dev image (Ubuntu, glibc
# >= 2.34) and the el8 .so is floored at glibc 2.28 — neither is the right
# artifact for an el10 yum tree. We build the SAME bazel target inside
# almalinux:10 with gcc-toolset-15's libstdc++ (C++23-complete; matches Cyclone's
# std::expected need) and STATIC-link libstdc++ via .bazelrc BAZEL_LINKLIBS, so
# the .so carries no libstdc++.so.6 NEEDED and references the el10 glibc.
#
# This mirrors build_apache_el8_in_container.sh exactly, with el10 substitutions:
#   - gcc-toolset-15  (el8 used -13; el10 ships gt-15, base gcc is 14.x)
#   - system clang 21 (el10 AppStream; >= clang-20 abseil absl_nonnull need — no sideload)
#   - pcre2-devel     (el10 dropped pcre1)
#   - GLIBC_FLOOR 2.39 native (el8 floored at 2.28; el10's libstdc++ legitimately
#     references __isoc23_*@GLIBC_2.38, so the el8 "expect 0 __isoc23 / floor 2.28"
#     gate INVERTS here and is re-pointed)
#   - --config=clang-libstdcxx15 (the clang-libstdcxx13 paths /usr/include/c++/13
#     do not exist on el10; the gt-15 symlink dance + bazelrc block use /15)
#
# Designed to run INSIDE an almalinux:10 container with the source tree (incl. a
# populated vendor/ from tools/vendor-deps.sh) bind-mounted at /workspace. For a
# network-fetch test-compile (no vendor/), pass NO_VENDOR=1 to drop --config=vendored.
#
# Usage (inside container, cwd = source root):
#   install/rpm/build_apache_el10_in_container.sh [-o <outdir>]
set -euo pipefail

OUTDIR="/workspace/packages-el10"
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUTDIR="$2"; shift 2 ;;
    *) echo "ERROR: unknown arg '$1'" >&2; exit 1 ;;
  esac
done

SRCDIR="$(pwd)"
GCC15_PREFIX="${GCC15_PREFIX:-/opt/rh/gcc-toolset-15/root/usr}"
GLIBC_FLOOR="2.39"   # EL10 (RHEL/Rocky/AlmaLinux/CloudLinux 10) — native build

echo "============================================================"
echo "EL10 Apache module build (glibc floor ${GLIBC_FLOOR}, native)"
echo "  srcdir : ${SRCDIR}"
echo "  outdir : ${OUTDIR}"
echo "============================================================"

# ---------------------------------------------------------------------------
# 1. Toolchain: gcc-toolset-15 (GCC-15 libstdc++, C++23-complete for Cyclone) +
#    system clang 21 as the compiler. el10 base GCC is 14.x (C++23-capable) but
#    we use gt-15 to mirror the el8 "distro toolset" recipe and stay ahead of the
#    Cyclone std::expected / __cpp_concepts floor.
# ---------------------------------------------------------------------------
if ! rpm -q gcc-toolset-15 >/dev/null 2>&1; then
  echo "==> installing el10 toolchain (gcc-toolset-15 + clang)"
  # gperf / bison / flex and some -devel headers live in CRB on el10.
  dnf install -y --setopt=install_weak_deps=False dnf-plugins-core >/dev/null
  dnf config-manager --set-enabled crb 2>/dev/null \
    || dnf config-manager --enable crb 2>/dev/null || true
  dnf install -y --allowerasing --setopt=install_weak_deps=False \
    ca-certificates curl wget \
    gcc-toolset-15 \
    clang lld llvm \
    zlib-devel pcre2-devel openssl-devel \
    httpd-devel apr-devel apr-util-devel \
    python3 unzip zip gperf bison flex nasm \
    rpm-build tar gzip make which file \
    >/dev/null
fi
# The Apache module compiles against the system Apache 2.4 headers. bazel/BUILD
# hardcodes the Debian path -I/usr/include/apache2/; on RHEL httpd-devel installs
# them under /usr/include/httpd/. Symlink so the hardcoded include path resolves —
# el10's httpd 2.4.63 headers are ABI-equivalent to EA4's Apache (MMN 20120211).
[ -e /usr/include/apache2 ] || ln -sfn /usr/include/httpd /usr/include/apache2
[ -d "${GCC15_PREFIX}" ] || { echo "ERROR: gcc-toolset-15 not at ${GCC15_PREFIX}" >&2; exit 1; }

# bazelisk (pinned, checksum-verified) — same launcher + pin the el8 leg uses.
BAZELISK_VERSION="v1.29.0"
BAZELISK_SHA256_AMD64="5a408715e932c0250d28bd84555f12edbf70117de42f9181691c736eacc4a992"
if command -v bazel >/dev/null 2>&1; then
  BAZEL=bazel
elif command -v bazelisk >/dev/null 2>&1; then
  BAZEL=bazelisk
else
  echo "==> installing bazelisk ${BAZELISK_VERSION}"
  url="https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-amd64"
  wget -qO /usr/local/bin/bazelisk "${url}"
  echo "${BAZELISK_SHA256_AMD64}  /usr/local/bin/bazelisk" | sha256sum -c - >/dev/null \
    || { echo "ERROR: bazelisk checksum mismatch" >&2; rm -f /usr/local/bin/bazelisk; exit 1; }
  chmod +x /usr/local/bin/bazelisk
  BAZEL=bazelisk
fi

# ---------------------------------------------------------------------------
# 2. Symlink dance — map gcc-toolset-15's libstdc++ (headers + static .a) into
#    the Ubuntu multiarch paths that --config=clang-libstdcxx15 hardcodes, so
#    clang finds it as a builtin. Same dance as the el8 leg, gt-15 paths.
# ---------------------------------------------------------------------------
export PATH="${GCC15_PREFIX}/bin:${PATH}"
mkdir -p /usr/include/c++ /usr/lib/gcc/x86_64-linux-gnu /usr/include/x86_64-linux-gnu/c++
ln -sfn "${GCC15_PREFIX}/include/c++/15" /usr/include/c++/15
ln -sfn "${GCC15_PREFIX}/lib/gcc/x86_64-redhat-linux/15" /usr/lib/gcc/x86_64-linux-gnu/15
ln -sfn "${GCC15_PREFIX}/include/c++/15/x86_64-redhat-linux" /usr/include/x86_64-linux-gnu/c++/15

# --- foreign_cc lib64 fix (RHEL-only) -------------------------------------
# rules_foreign_cc CMake deps (libcurl/libevent/libmemcached) install to lib64/
# on a RHEL base unless /etc/debian_version exists (GNUInstallDirs skips its
# lib64 heuristic then), but bazel/BUILD pins out_static_libs to lib/. Create
# the marker (build-container only, ephemeral) so the .a lands in lib/. See the
# el8 leg for the full rationale.
[ -f /etc/debian_version ] || echo "el10-foreign-cc-libdir-shim" > /etc/debian_version

# Binding info-check: on el10 gt-15 libstdc++.a legitimately references
# __isoc23_*@GLIBC_2.38 (glibc 2.39) — UNLIKE el8 (where 0 was expected). This is
# informational only; the authoritative gate is the max-GLIBC floor below (2.39).
LIBA="${GCC15_PREFIX}/lib/gcc/x86_64-redhat-linux/15/libstdc++.a"
if [ -f "${LIBA}" ]; then
  ISOC23="$(objdump -T "${LIBA}" 2>/dev/null | grep -c '__isoc23' || true)"
  echo "    libstdc++.a __isoc23 refs: ${ISOC23} (nonzero EXPECTED on el10 glibc 2.39 — not a floor violation)"
fi

# ---------------------------------------------------------------------------
# 3. Build //:libmod_pagespeed.so. --config=clang-libstdcxx15 + (vendored unless
#    NO_VENDOR=1 for a live-fetch test-compile). NOT --config=ci (that pulls the
#    remote cache + clang-libstdcxx13). Feature disables match the el8/el9 legs.
# ---------------------------------------------------------------------------
BAZEL_STARTUP=( "--output_user_root=/tmp/bazel-out" )
# NO --disk_cache: a warm disk cache silently drops foreign_cc merged-archive
# members (libmemcached.a/libhashkit.a/libmemcachedutil.a) → "output not created"
# at link. Cold builds only.
BAZEL_FLAGS=(
  "--config=clang-libstdcxx15"
  "--features=-layering_check"
  "--features=-parse_headers"
  "--features=-use_header_modules"
  "-c" "opt" "--strip=never"
  "--verbose_failures"
)
if [ "${NO_VENDOR:-0}" != "1" ]; then
  BAZEL_FLAGS=( "--config=vendored" "${BAZEL_FLAGS[@]}" )
else
  echo "==> NO_VENDOR=1: live-fetch build (no vendor/ tarball; cyclone + http_archives over network)"
fi
echo "==> bazel build //:libmod_pagespeed.so (el10 glibc ${GLIBC_FLOOR} native target)"
"${BAZEL}" "${BAZEL_STARTUP[@]}" build "${BAZEL_FLAGS[@]}" -- //:libmod_pagespeed.so

BUILT="${SRCDIR}/bazel-bin/libmod_pagespeed.so"
[ -f "${BUILT}" ] || { echo "ERROR: bazel produced no libmod_pagespeed.so" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 4. The glibc-floor gate, re-pointed to el10's glibc 2.39. NEVER
#    publish on an ELF-pass alone — the live el10/cPanel dlopen smoke is the
#    authoritative check — but this catches a stray newer-glibc / dynamic-
#    libstdc++ leak cheaply.
# ---------------------------------------------------------------------------
floor_gate() {
  local so="$1"
  echo "=== glibc-floor gate (floor ${GLIBC_FLOOR}) on ${so} ==="
  # libstdc++ MUST be static (gt-15's is newer than el10's stock gcc-14 runtime —
  # a dynamic NEEDED would push the GLIBCXX floor past what an el10 host ships).
  # libgcc_s.so.1 stays DYNAMIC (ABI-stable, present on every el10 host; matches
  # the production el9 Apache .so). Contract: static libstdc++, dynamic libgcc_s,
  # glibc <= 2.39.
  local needed_cxx
  needed_cxx="$(readelf -d "${so}" 2>/dev/null | awk '/\(NEEDED\)/{gsub(/[][]/,"",$NF);print $NF}' | grep -iE '^libstdc\+\+' || true)"
  if [ -n "${needed_cxx}" ]; then
    echo "FAIL: libstdc++ dynamically linked (must be static — would raise the GLIBCXX floor above el10's stock runtime):" >&2
    echo "${needed_cxx}" | sed 's/^/   /' >&2
    return 1
  fi
  echo "OK: no dynamic libstdc++ in NEEDED (statically linked); libgcc_s.so.1 dynamic is expected (matches production)."
  local glibcxx
  glibcxx="$(objdump -T "${so}" 2>/dev/null | grep -oE 'GLIBCXX_[0-9.]+' | sort -V | tail -1 || true)"
  [ -n "${glibcxx}" ] && echo "    note: highest GLIBCXX ref ${glibcxx} (in statically-linked symbols — not a runtime dep)"
  local maxglibc highest
  maxglibc="$(objdump -T "${so}" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' | sed 's/GLIBC_//' | sort -V | tail -1)"
  echo "    max GLIBC symbol ref: ${maxglibc:-<none>} (floor ${GLIBC_FLOOR})"
  if [ -n "${maxglibc}" ]; then
    highest="$(printf '%s\n%s\n' "${maxglibc}" "${GLIBC_FLOOR}" | sort -V | tail -1)"
    if [ "${highest}" != "${GLIBC_FLOOR}" ]; then
      echo "FAIL: max GLIBC ref ${maxglibc} EXCEEDS the el10 floor ${GLIBC_FLOOR} — would not load on el10." >&2
      echo "    offending symbols:" >&2
      objdump -T "${so}" 2>/dev/null | grep -E "GLIBC_2\.(4[0-9]|[5-9][0-9])" | head -20 | sed 's/^/      /' >&2
      return 1
    fi
  fi
  echo "OK: max GLIBC ref ${maxglibc} <= floor ${GLIBC_FLOOR}."
  return 0
}

floor_gate "${BUILT}"

echo "============================================================"
echo "EL10 Apache .so build + floor gate PASSED"
ls -lh "${BUILT}"
echo "============================================================"

# ---------------------------------------------------------------------------
# 5. (optional) wrap into the stock RPM via the existing rpm build, and re-gate
#    the .so as laid inside the RPM.
# ---------------------------------------------------------------------------
if [ "${SKIP_RPM:-0}" != "1" ]; then
  mkdir -p "${SRCDIR}/debug" "${OUTDIR}"
  install -m 755 "${BUILT}" "${SRCDIR}/libmod_pagespeed.so"
  strip --strip-debug "${SRCDIR}/libmod_pagespeed.so"
  echo "==> install/rpm/build.sh -a x64 -o ${OUTDIR}"
  install/rpm/build.sh -a x64 -b . -o "${OUTDIR}/"
  echo "==> produced RPMs:"
  ls -lh "${OUTDIR}"/*.rpm || true
fi
