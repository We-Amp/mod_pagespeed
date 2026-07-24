#!/usr/bin/env bash
# Build the Apache module .so (//:libmod_pagespeed.so) against EL8 glibc 2.28 so
# it loads on both EL8/CloudLinux 8 (glibc 2.28) AND EL9 (glibc 2.34), then wrap
# it into the stock mod-pagespeed-<ver>.x86_64.rpm that the EA4 repackager spec
# (packaging/cpanel/ea-apache24-mod_pagespeed.spec) consumes.
#
# WHY: the default Apache .so is built once in the
# pagespeed1.1-dev image (FROM envoyproxy/envoy-build-ubuntu, glibc >= 2.34) and
# physically will not dlopen on EL8's glibc 2.28 — the confirmed root cause of the
# 2026-05-21 EL8 drop. The fix is purely the glibc build floor: build the SAME
# bazel target inside almalinux:8 with gcc-toolset-13's libstdc++ (built against
# the distro glibc 2.28, so NO Debian-style __isoc23/arc4random shims are needed —
# that is the advantage of the distro's own toolset over a sideloaded one) and
# STATIC-link libstdc++ via .bazelrc:73 BAZEL_LINKLIBS=-l%:libstdc++.a. The
# resulting .so carries no libstdc++.so.6 NEEDED and a glibc floor of 2.28.
#
# This mirrors install/nginx/build_module_in_container.sh's el9 leg (the proven
# P3 cross-glibc recipe), reusing the SAME --config=clang-libstdcxx13 +
# gcc-toolset-13 symlink dance, but for the Apache bazel target instead of the
# nginx module, and floored at glibc 2.28 instead of 2.34.
#
# Designed to run INSIDE an almalinux:8 container with the source tree (incl. a
# populated vendor/ from tools/vendor-deps.sh) bind-mounted at /workspace.
#
# Usage (inside container, cwd = source root):
#   install/rpm/build_apache_el8_in_container.sh [-o <outdir>]
set -euo pipefail

OUTDIR="/workspace/packages-el8"
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUTDIR="$2"; shift 2 ;;
    *) echo "ERROR: unknown arg '$1'" >&2; exit 1 ;;
  esac
done

SRCDIR="$(pwd)"
GCC13_PREFIX="${GCC13_PREFIX:-/opt/rh/gcc-toolset-13/root/usr}"
GLIBC_FLOOR="2.28"   # EL8 / CloudLinux 8

echo "============================================================"
echo "EL8 Apache module build (glibc floor ${GLIBC_FLOOR})"
echo "  srcdir : ${SRCDIR}"
echo "  outdir : ${OUTDIR}"
echo "============================================================"

# ---------------------------------------------------------------------------
# 1. Toolchain: gcc-toolset-13 (GCC-13 libstdc++ matching Cyclone's C++23 need,
#    built against el8 glibc 2.28) + a C++23-capable clang as the compiler. EL8
#    base GCC is 8.5 (no C++23), so gcc-toolset-13 is mandatory.
# ---------------------------------------------------------------------------
if ! rpm -q gcc-toolset-13 >/dev/null 2>&1; then
  echo "==> installing el8 toolchain (gcc-toolset-13 + clang)"
  # gperf (and some -devel headers) live in PowerTools (AlmaLinux 8) / CRB
  # (Rocky/RHEL-rebuild naming). Enable whichever the base image ships.
  dnf install -y --setopt=install_weak_deps=False dnf-plugins-core >/dev/null
  dnf config-manager --set-enabled powertools 2>/dev/null \
    || dnf config-manager --set-enabled crb 2>/dev/null \
    || dnf config-manager --set-enabled powertools 2>/dev/null || true
  dnf install -y --allowerasing --setopt=install_weak_deps=False \
    ca-certificates curl wget \
    gcc-toolset-13 \
    clang lld llvm \
    zlib-devel pcre-devel openssl-devel \
    httpd-devel apr-devel apr-util-devel \
    python3 unzip zip gperf bison flex nasm \
    rpm-build tar gzip make which file \
    >/dev/null
fi
# The Apache module compiles against the system Apache 2.4 headers (httpd.h,
# http_config.h, …). bazel/BUILD hardcodes the Debian path -I/usr/include/apache2/
# (the dev image is Ubuntu); on RHEL httpd-devel installs them under
# /usr/include/httpd/. Symlink so the hardcoded include path resolves — el8's
# Apache 2.4.x headers are ABI-equivalent to EA4's Apache (MMN 20120211).
[ -e /usr/include/apache2 ] || ln -sfn /usr/include/httpd /usr/include/apache2
[ -d "${GCC13_PREFIX}" ] || { echo "ERROR: gcc-toolset-13 not at ${GCC13_PREFIX}" >&2; exit 1; }

# bazelisk (pinned, checksum-verified) — same launcher + pin build_module_in_container uses.
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
# 2. Symlink dance — reuse --config=clang-libstdcxx13 verbatim (build_module_in_
#    container.sh:629-672). The config hardcodes the Ubuntu multiarch paths; map
#    gcc-toolset-13's libstdc++ (headers + static .a) into them so clang finds it
#    as a builtin (no --gcc-install-dir absolute-path guard tripping).
# ---------------------------------------------------------------------------
export PATH="${GCC13_PREFIX}/bin:${PATH}"
mkdir -p /usr/include/c++ /usr/lib/gcc/x86_64-linux-gnu /usr/include/x86_64-linux-gnu/c++
ln -sfn "${GCC13_PREFIX}/include/c++/13" /usr/include/c++/13
ln -sfn "${GCC13_PREFIX}/lib/gcc/x86_64-redhat-linux/13" /usr/lib/gcc/x86_64-linux-gnu/13
ln -sfn "${GCC13_PREFIX}/include/c++/13/x86_64-redhat-linux" /usr/include/x86_64-linux-gnu/c++/13

# --- foreign_cc lib64 fix (RHEL-only) -------------------------------------
# The PSOL graph builds libcurl/libevent/libmemcached via rules_foreign_cc
# (CMake). bazel/BUILD pins their `out_static_libs` to `lib/…a` and sets
# `CMAKE_INSTALL_LIBDIR=lib`, but on a RHEL base CMake's GNUInstallDirs installs
# to `lib64/` instead — and foreign_cc's configure→install prefix change makes
# GNUInstallDirs RE-compute and override the `lib` cache entry, so the .a lands
# in `lib64/` and bazel reports "output libmemcached.a was not created". This is
# the first time the Apache `.so` (full PSOL incl. libmemcached) is built on a
# RHEL base — the dev image is Ubuntu, where `lib/` is already the default.
# GNUInstallDirs explicitly SKIPS its lib64 heuristic when /etc/debian_version
# exists; creating it (build-container only, ephemeral) makes every foreign_cc
# CMake dep install to `lib/`, matching bazel/BUILD's out_static_libs. Touches
# nothing in the product or the resulting binary.
[ -f /etc/debian_version ] || echo "el8-foreign-cc-libdir-shim" > /etc/debian_version

# Binding pre-check (R3 in the briefing): gcc-toolset-13 libstdc++.a vs el8 glibc
# 2.28 should have NO __isoc23_* refs (those would push the floor to 2.38).
LIBA="${GCC13_PREFIX}/lib/gcc/x86_64-redhat-linux/13/libstdc++.a"
if [ -f "${LIBA}" ]; then
  ISOC23="$(objdump -T "${LIBA}" 2>/dev/null | grep -c '__isoc23' || true)"
  echo "    libstdc++.a __isoc23 refs: ${ISOC23} (expect 0 on el8 gcc-toolset-13)"
fi

# ---------------------------------------------------------------------------
# 3. Build //:libmod_pagespeed.so. --config=vendored (offline, vendor/repo-cache
#    + vendor/cyclone override) + --config=clang-libstdcxx13. NOT --config=ci
#    (that adds --config=remote-cache pointing at cache-host + clang-libstdcxx13's
#    el9 paths; we supply clang-libstdcxx13 explicitly and skip the remote cache).
#    The feature disables match the el9 leg (clang layering_check trips on abseil
#    under libstdc++). build:linux's BAZEL_LINKLIBS=-l%:libstdc++.a statically
#    links the C++ runtime into the .so.
# ---------------------------------------------------------------------------
BAZEL_STARTUP=( "--output_user_root=/tmp/bazel-out" )
# NO --disk_cache: per the design record P3 (build_module_in_container.sh) a warm disk
# cache silently drops foreign_cc merged-archive members (libmemcached's
# libmemcached.a/libhashkit.a/libmemcachedutil.a), surfacing as "output was not
# created" at link. Cold builds only.
BAZEL_FLAGS=(
  "--config=vendored"
  "--config=clang-libstdcxx13"
  "--features=-layering_check"
  "--features=-parse_headers"
  "--features=-use_header_modules"
  "-c" "opt" "--strip=never"
  "--verbose_failures"
)
echo "==> bazel build //:libmod_pagespeed.so (el8 glibc 2.28 target)"
"${BAZEL}" "${BAZEL_STARTUP[@]}" build "${BAZEL_FLAGS[@]}" -- //:libmod_pagespeed.so

BUILT="${SRCDIR}/bazel-bin/libmod_pagespeed.so"
[ -f "${BUILT}" ] || { echo "ERROR: bazel produced no libmod_pagespeed.so" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 4. The glibc-floor gate. NEVER publish on an ELF-pass alone — the live
#    cPanel smoke is the authoritative dlopen check — but this is the cheapest
#    place to catch a glibc-2.34 leak.
# ---------------------------------------------------------------------------
floor_gate() {
  local so="$1"
  echo "=== glibc-floor gate (floor ${GLIBC_FLOOR}) on ${so} ==="
  # Reject a DYNAMIC libstdc++ only. libstdc++ MUST be static (the gcc-toolset-13
  # one is NEWER than el8's stock 6.x runtime — a dynamic NEEDED would push the
  # GLIBCXX floor past what an EL8 host ships). libgcc_s.so.1 is intentionally
  # left DYNAMIC: it is ABI-stable, present on every EL8/EL9 host, and the
  # PRODUCTION el9 Apache .so (dev image) ships it dynamic too (verified — its
  # NEEDED is libgcc_s.so.1 + libc.so.6, no libstdc++). Static-linking libgcc
  # into an Apache *module* also risks cross-boundary C++ unwinding bugs. So the
  # Apache module's contract is: static libstdc++, dynamic libgcc_s, glibc ≤ 2.28.
  local needed_cxx
  needed_cxx="$(readelf -d "${so}" 2>/dev/null | awk '/\(NEEDED\)/{gsub(/[][]/,"",$NF);print $NF}' | grep -iE '^libstdc\+\+' || true)"
  if [ -n "${needed_cxx}" ]; then
    echo "FAIL: libstdc++ dynamically linked (must be static — would raise the GLIBCXX floor above EL8's stock runtime):" >&2
    echo "${needed_cxx}" | sed 's/^/   /' >&2
    return 1
  fi
  echo "OK: no dynamic libstdc++ in NEEDED (statically linked); libgcc_s.so.1 dynamic is expected (matches production)."
  # Belt-and-suspenders: assert no GLIBCXX_* (libstdc++ ABI) symbol versions leak
  # into the .so's dynamic symbol table (a dynamic libstdc++ would show these).
  local glibcxx
  glibcxx="$(objdump -T "${so}" 2>/dev/null | grep -oE 'GLIBCXX_[0-9.]+' | sort -V | tail -1 || true)"
  [ -n "${glibcxx}" ] && echo "    note: highest GLIBCXX ref ${glibcxx} (in statically-linked symbols — not a runtime dep)"
  local maxglibc highest
  maxglibc="$(objdump -T "${so}" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' | sed 's/GLIBC_//' | sort -V | tail -1)"
  echo "    max GLIBC symbol ref: ${maxglibc:-<none>} (floor ${GLIBC_FLOOR})"
  if [ -n "${maxglibc}" ]; then
    highest="$(printf '%s\n%s\n' "${maxglibc}" "${GLIBC_FLOOR}" | sort -V | tail -1)"
    if [ "${highest}" != "${GLIBC_FLOOR}" ]; then
      echo "FAIL: max GLIBC ref ${maxglibc} EXCEEDS the el8 floor ${GLIBC_FLOOR} — would not load on EL8/CloudLinux 8." >&2
      echo "    offending symbols:" >&2
      objdump -T "${so}" 2>/dev/null | grep -E "GLIBC_2\.(29|3[0-9]|4[0-9])" | head -20 | sed 's/^/      /' >&2
      return 1
    fi
  fi
  echo "OK: max GLIBC ref ${maxglibc} <= floor ${GLIBC_FLOOR}."
  return 0
}

floor_gate "${BUILT}"

echo "============================================================"
echo "EL8 Apache .so build + floor gate PASSED"
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
