#!/bin/bash
#
# build_module_in_container.sh — build the ABI-correct
# ngx_pagespeed_module.so for ONE target distro, INSIDE that distro's container.
#
# WHY THIS SCRIPT EXISTS (the glibc/ABI problem, EA4 lesson, journal Phase B):
# An nginx dynamic module compiled on a modern Ubuntu dev base demands
# GLIBC_2.38 / GLIBCXX_3.4.32. AlmaLinux 9 ships glibc 2.34 / GLIBCXX 3.4.29, so
# such a .so will NOT load on stock el9. The reference is baked in at compile
# time (PSOL C objects reference __isoc23_* GLIBC_2.38 symbols, etc.) and is
# un-relinkable. The fix is to build the WHOLE module — the merged PSOL archive
# AND the nginx `make modules` link — inside the target distro's own container so
# its libc/libstdc++ floor matches the target:
#   el9   -> almalinux:9      (glibc 2.34, gcc-toolset-13)
#   noble -> ubuntu:24.04     (glibc 2.39, g++-13 + clang)
#
# It reproduces the proven Phase A/B recipe (journal "Phase A CLOSED" + "Phase B
# CLOSED"): bazel build of the .so target to force the full PSOL object graph,
# aquery the CppLink action's Inputs to assemble a single merged PSOL archive,
# then nginx's OWN `./configure --add-dynamic-module ... --with-compat && make
# modules` against the TARGET nginx source (so module->version == that nginx).
#
# This script is the per-distro `.so`-producing front half; install/nginx/
# build_nginx_{deb,rpm}.sh is the back half that wraps the produced .so into a
# package. release.yml runs THIS first, then the packager, in the same container.
#
# Usage (inside the target-distro container, repo CWD = mod_pagespeed source root
# with vendor/ present):
#   install/nginx/build_module_in_container.sh <noble|el9> [-o <out.so>]
#
# Output: the built ngx_pagespeed_module.so at -o (default
#   bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so — overwritten with the
#   ABI-correct module so the packager's default -s path also works).
#
# Env overrides (CI / debugging):
#   NGINX_SRC_VERSION   exact nginx version to build the glue against
#                       (default: noble->1.24.0, el9->1.20.1)
#   NGINX_SRC_SHA256    sha256 of the nginx-<ver>.tar.gz. The download is ALWAYS
#                       verified (supply-chain pin): if unset, the per-distro
#                       default hash is used; required if NGINX_SRC_VERSION is
#                       overridden to a non-default version.
#   BAZEL               bazel/bazelisk binary (default: bazel, else bazelisk)
#   GCC13_PREFIX        el9 gcc-toolset-13 prefix (default /opt/rh/gcc-toolset-13/root/usr)
#   SKIP_DEPS=1         skip the apt/dnf build-dep install (deps already present)

set -euo pipefail
if [ "${VERBOSE:-}" ]; then set -x; fi

DISTRO="${1:?usage: $0 <noble|el9> [-o out.so]}"
shift || true

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(cd "${SCRIPTDIR}/../.." && pwd)"
OUT_SO="${SRCDIR}/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so"

while getopts ":o:h" opt; do
  case $opt in
    o) OUT_SO="$OPTARG" ;;
    h) echo "usage: $0 <noble|el9> [-o out.so]"; exit 0 ;;
    *) echo "invalid option: -$OPTARG" >&2; exit 1 ;;
  esac
done

# Per-distro target nginx version AND its verified upstream sha256. The sha256s were captured by downloading the tarballs from
# nginx.org and `sha256sum` (2026-05-27); they match nginx.org's
# published release hashes. The wget below ALWAYS verifies against these (the
# check is no longer opt-in), so a tampered/corrupt mirror download hard-fails.
case "${DISTRO}" in
  noble) DEF_NGINX_VER="1.24.0"
         DEF_NGINX_SHA256="77a2541637b92a621e3ee76776c8b7b40cf6d707e69ba53a940283e30ff2f55d" ;;
  el9)   DEF_NGINX_VER="1.20.1"
         DEF_NGINX_SHA256="e462e11533d5c30baa05df7652160ff5979591d291736cfa5edb9fd2edb48c49" ;;
  *) echo "ERROR: unknown distro '${DISTRO}' (want noble|el9)" >&2; exit 1 ;;
esac
NGINX_SRC_VERSION="${NGINX_SRC_VERSION:-${DEF_NGINX_VER}}"
# Resolve the sha256 to verify against. If the caller pinned NGINX_SRC_SHA256
# explicitly, honor it. Otherwise use the per-distro default ONLY when the
# version still matches the default; if NGINX_SRC_VERSION was overridden to a
# version we did not pin a hash for, refuse to download an unverified tarball.
if [ -z "${NGINX_SRC_SHA256:-}" ]; then
  if [ "${NGINX_SRC_VERSION}" = "${DEF_NGINX_VER}" ]; then
    NGINX_SRC_SHA256="${DEF_NGINX_SHA256}"
  else
    echo "ERROR: NGINX_SRC_VERSION overridden to ${NGINX_SRC_VERSION} but no NGINX_SRC_SHA256 set;" >&2
    echo "       refusing to download an unverified nginx tarball (supply-chain pin, the design record)." >&2
    exit 1
  fi
fi

BAZEL="${BAZEL:-}"
if [ -z "${BAZEL}" ]; then
  if command -v bazelisk >/dev/null 2>&1; then BAZEL=bazelisk; else BAZEL=bazel; fi
fi

echo "============================================================"
echo "the design record in-container module build"
echo "  distro          : ${DISTRO}"
echo "  target nginx    : ${NGINX_SRC_VERSION}"
echo "  source root     : ${SRCDIR}"
echo "  output .so      : ${OUT_SO}"
echo "  bazel           : ${BAZEL}"
echo "============================================================"

# ---------------------------------------------------------------------------
# 1. Install build deps for the target distro.
#    el9 uses gcc-toolset-13 (GCC 13 libstdc++ matching the Cyclone C++23 need);
#    clang is wrapped with --gcc-install-dir so Bazel's sandbox allowlists the
#    GCC-13 libstdc++ headers as builtin (a bare -nostdinc++ -I<path> is rejected
#    "outside the execution root"). noble uses apt clang + g++-13, for which the
#    clang-libstdcxx13 bazelrc hardcoded paths (/usr/include/c++/13,
#    /usr/lib/gcc/x86_64-linux-gnu/13) already match.
# ---------------------------------------------------------------------------

# Pinned bazelisk launcher used when no bazel/bazelisk is already present.
# Pin the exact version and verify its sha256 (fail-closed) rather than
# fetching the mutable "latest" URL and exec'ing it unverified as the release
# build toolchain. Update both values together when bumping.
BAZELISK_VERSION="v1.29.0"
BAZELISK_SHA256="5a408715e932c0250d28bd84555f12edbf70117de42f9181691c736eacc4a992"

install_bazelisk() {
  if command -v "${BAZEL}" >/dev/null 2>&1; then
    return 0
  fi
  local url="https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-amd64"
  wget -qO /usr/local/bin/bazelisk "${url}"
  if ! echo "${BAZELISK_SHA256}  /usr/local/bin/bazelisk" | sha256sum -c - >/dev/null 2>&1; then
    echo "ERROR: bazelisk ${BAZELISK_VERSION} checksum mismatch; refusing to use it" >&2
    rm -f /usr/local/bin/bazelisk
    exit 1
  fi
  chmod +x /usr/local/bin/bazelisk
  BAZEL=bazelisk
}

install_deps_noble() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y --no-install-recommends \
    ca-certificates curl wget gnupg lsb-release \
    build-essential g++-13 gcc-13 \
    clang lld llvm \
    zlib1g-dev libpcre3-dev libssl-dev \
    python3 unzip zip gperf bison flex \
    dpkg-dev fakeroot >/dev/null
  # bazelisk (pinned + checksum-verified) if bazel/bazelisk not already provided.
  install_bazelisk
}

install_deps_el9() {
  # --allowerasing: almalinux:9 ships curl-minimal, which conflicts with the full
  # `curl` package; allow dnf to swap it rather than abort on "conflicting requests".
  dnf install -y --allowerasing --setopt=install_weak_deps=False \
    ca-certificates curl wget \
    gcc-toolset-13 \
    clang lld llvm \
    zlib-devel pcre-devel openssl-devel \
    python3 unzip zip gperf bison flex \
    rpm-build tar gzip make >/dev/null
  # bazelisk (pinned + checksum-verified) if bazel/bazelisk not already provided.
  install_bazelisk
}

if [ "${SKIP_DEPS:-}" != "1" ]; then
  echo "==> installing build deps (${DISTRO})"
  case "${DISTRO}" in
    noble) install_deps_noble ;;
    el9)   install_deps_el9 ;;
  esac
fi

# ---------------------------------------------------------------------------
# 2. Per-distro Bazel toolchain flags.
#    Common: --config=vendored (offline, repo-cache + cyclone
#    override) + -c opt.
#    noble: --config=clang-libstdcxx13 works as-is (its hardcoded GCC-13 paths
#           are noble's apt paths).
#    el9:   gcc-toolset-13 lives under ${GCC13_PREFIX}; clang must be told to use
#           it via --gcc-install-dir, and the Cyclone per_file_copt's -nostdinc++
#           +include dirs and the libstdc++ -L must be redirected to the toolset.
# ---------------------------------------------------------------------------
cd "${SRCDIR}"

# Base flags. This mirrors the proven `--config=ci` recipe the rest of the
# release build uses (pagespeed.bazelrc: ci = vendored + clang-libstdcxx13 +
# remote-cache + c++20 host/cxxopt), MINUS clang-libstdcxx13 (whose hardcoded
# Ubuntu libstdc++ paths break on el9 — handled per-distro below) and MINUS
# remote-cache (we keep the build self-contained; the disk cache below still
# de-dups against the same-config Apache/nginx objects). The c++20 host/cxxopt
# IS carried because the PSOL graph is compiled with it in CI; dropping it
# would diverge the object ABI from the proven build.
BAZEL_FLAGS=(
  "--config=vendored" "-c" "opt"
  "--cxxopt=-std=c++20" "--host_cxxopt=-std=c++20"
  "--define=go_build=disabled"
)
# NOTE: we deliberately do NOT add --disk_cache here, even when BAZEL_DISK_CACHE
# is set. The merged PSOL archive is
# assembled (step 4) from the LOOSE .pic.o link-input files in the execroot. When
# a warm --disk_cache satisfies the .so link as a cache hit, Bazel does NOT stage
# the intermediate compile outputs loose into bazel-out (it never needs them as
# files), so ~dozens of objects (abseil base/crc, css_parser utf, jpeg_reader,
# grpc alts, ...) are absent and the archive silently loses them -> undefined
# symbols / wrong behavior on the optimizing path. A no-disk_cache build forces
# every CppCompile to execute and write its .pic.o loose, so all 2900+ members
# are present (verified: 2986 loose .pic.o vs 0 under a warm cache). The
# --config=vendored repository_cache still avoids re-fetching external repos, so
# only compilation is cold. (Honor an explicit opt-in override if a caller really
# wants the cache and accepts the materialization risk.)
if [ -n "${BAZEL_DISK_CACHE:-}" ] && [ "${ALLOW_DISK_CACHE:-0}" = "1" ]; then
  echo "::warning::ALLOW_DISK_CACHE=1 set — using --disk_cache; verify all PSOL objects materialized (see step 4 guard)"
  BAZEL_FLAGS+=( "--disk_cache=${BAZEL_DISK_CACHE}" )
elif [ -n "${BAZEL_DISK_CACHE:-}" ]; then
  echo "==> BAZEL_DISK_CACHE is set but intentionally NOT used (materialization correctness); set ALLOW_DISK_CACHE=1 to override"
fi

# ---------------------------------------------------------------------------
# 2a. Fetch + ./configure the TARGET nginx BEFORE the Bazel build, and point the
#     Bazel @nginx repo at it via NGINX_PATH.
#
#     ROOT CAUSE THIS FIXES: the module ABI is split across two compilers. The
#     nginx-make half (ngx_pagespeed.cc, via --add-dynamic-module) compiles
#     against the TARGET nginx headers fetched in step 6 (noble->1.24.0,
#     el9->1.20.1). But the Bazel-archive half (ngx_server_context.cc et al.,
#     in libpagespeed_psol_full.a) compiles against @nginx, which bazel/nginx.bzl
#     resolves to the third_party/nginx SUBMODULE (1.26.3). nginx grew the
#     `quic` field in ngx_connection_s in 1.25, so 1.26.3 puts `local_sockaddr`
#     at offset 0xa8 while 1.20.1/1.24.0 put it at 0xa0 (8 bytes lower, no quic).
#     NgxServerContext::NewRequestContext (archive half) then reads
#     r->connection->local_sockaddr from 0xa8 on a stock-nginx struct -> garbage
#     ptr -> SIGSEGV on the first optimizing request. `nginx -t` PASSES because
#     the module->version + NGX_MODULE_SIGNATURE live in the nginx-make half
#     (built vs the target -> correct), so the binary-compat check never sees the
#     archive half's stale layout. Pass-through never touches local_sockaddr, so
#     only the LICENSED optimizing path crashes.
#
#     FIX: make the @nginx repo use the SAME target nginx version as the make
#     half. nginx_repository (bazel/nginx.bzl) prefers $NGINX_PATH over the
#     submodule, and re-evaluates because it is local=True + environ=[NGINX_PATH].
#     We do a first ./configure (no module) so objs/ngx_auto_config.h exists with
#     --with-compat; the repo rule symlinks src/ + objs/ from it. The SAME tree is
#     re-./configure'd with --add-dynamic-module in step 6 for `make modules`, so
#     both halves see one identical, --with-compat, target-versioned nginx ABI.
# ---------------------------------------------------------------------------
NGX_BUILD_DIR="${SRCDIR}/.ngx-module-build/${DISTRO}"
NGX_SRC="${NGX_BUILD_DIR}/nginx-${NGINX_SRC_VERSION}"
echo "==> fetch + pre-configure target nginx ${NGINX_SRC_VERSION} (for @nginx ABI match)"
rm -rf "${NGX_BUILD_DIR}"
mkdir -p "${NGX_BUILD_DIR}"
NGX_TARBALL="${NGX_BUILD_DIR}/nginx-${NGINX_SRC_VERSION}.tar.gz"
wget -qO "${NGX_TARBALL}" "https://nginx.org/download/nginx-${NGINX_SRC_VERSION}.tar.gz"
# Supply-chain pin: ALWAYS verify the downloaded tarball against the
# resolved sha256 (per-distro default or explicit NGINX_SRC_SHA256). Hard-fail
# on mismatch — a tampered/corrupt download must never reach the build.
echo "==> verifying nginx tarball sha256 (${NGINX_SRC_SHA256})"
echo "${NGINX_SRC_SHA256}  ${NGX_TARBALL}" | sha256sum -c - \
  || { echo "ERROR: nginx tarball sha256 mismatch (got $(sha256sum "${NGX_TARBALL}" | awk '{print $1}'), expected ${NGINX_SRC_SHA256})" >&2; exit 1; }
tar -xzf "${NGX_TARBALL}" -C "${NGX_BUILD_DIR}"
# First configure WITHOUT the module: generates objs/ngx_auto_config.h with the
# SAME feature flags the runtime stock nginx uses (mirrors docker/Dockerfile +
# the step-6 make-modules configure). The module add is deferred to step 6
# because --add-dynamic-module needs the merged archive built first.
#
# nginx's configure needs a C compiler on PATH. noble has it via build-essential;
# el9's install_deps_el9 installs gcc-toolset-13 (not base gcc), so prepend the
# toolset bin for el9 (the same toolchain step 6's make modules uses).
NGX_PRECFG_PATH="${PATH}"
if [ "${DISTRO}" = "el9" ]; then
  NGX_PRECFG_PATH="${GCC13_PREFIX:-/opt/rh/gcc-toolset-13/root/usr}/bin:${PATH}"
fi
(
  cd "${NGX_SRC}"
  PATH="${NGX_PRECFG_PATH}" ./configure \
    --with-compat \
    --with-http_ssl_module \
    --with-http_v2_module \
    --with-threads
)
[ -f "${NGX_SRC}/objs/ngx_auto_config.h" ] || {
  echo "ERROR: target nginx pre-configure did not produce objs/ngx_auto_config.h" >&2
  exit 1
}
# Point the Bazel @nginx repository at this exact target nginx tree, so the
# archive's ngx_server_context.cc compiles against the matching ngx_connection_t.
export NGINX_PATH="${NGX_SRC}"
echo "    NGINX_PATH=${NGINX_PATH} (nginx_version $(awk -F'"' '/define NGINX_VERSION/{print $2}' "${NGX_SRC}/src/core/nginx.h"))"

if [ "${DISTRO}" = "el9" ]; then
  GCC13_PREFIX="${GCC13_PREFIX:-/opt/rh/gcc-toolset-13/root/usr}"
  if [ ! -d "${GCC13_PREFIX}" ]; then
    echo "ERROR: gcc-toolset-13 not found at ${GCC13_PREFIX}" >&2
    exit 1
  fi
  # Put gcc-toolset-13 on PATH so its tools/libs are found.
  export PATH="${GCC13_PREFIX}/bin:${PATH}"
  # Make el9 build with the EXACT SAME --config=clang-libstdcxx13 as noble/CI, by
  # SYMLINKING gcc-toolset-13's libstdc++ into the STANDARD Linux x86_64 paths that
  # config hardcodes (/usr/include/c++/13 + /usr/lib/gcc/x86_64-linux-gnu/13).
  # WHY: gcc-toolset-13 lives at a non-standard prefix, so the obvious
  # alternative (--config=clang + clang --gcc-install-dir=<toolset>) makes clang-21
  # report the toolset's libstdc++ headers via ABSOLUTE paths NOT in the toolchain's
  # builtin set -> Bazel's "absolute path inclusion(s) found" guard fails the build.
  # The native-gcc alternative (--config=gcc) compiles the whole graph but the
  # libmemcached foreign_cc CMake fails linking its `memcapable` C++ utility under
  # the toolset g++ (missing libstdc++ at exe-link). Symlinking the toolset into the
  # standard paths sidesteps BOTH: clang finds libstdc++ as a builtin (no
  # --gcc-install-dir, no absolute-path guard), and the proven clang-libstdcxx13
  # path is reused verbatim — giving the glibc-2.34/libstdc++-13 floor el9 needs.
  mkdir -p /usr/include/c++ /usr/lib/gcc/x86_64-linux-gnu /usr/include/x86_64-linux-gnu/c++
  ln -sfn "${GCC13_PREFIX}/include/c++/13" /usr/include/c++/13
  # Symlink the WHOLE toolset gcc/13 dir to the standard path; this exposes
  # libstdc++.a/.so + the c++config target headers in one shot. (Do NOT also
  # symlink individual libs INTO this dir afterwards — they'd resolve through the
  # dir symlink back onto themselves and `ln` would abort with "same file" under
  # set -e.)
  ln -sfn "${GCC13_PREFIX}/lib/gcc/x86_64-redhat-linux/13" /usr/lib/gcc/x86_64-linux-gnu/13
  # Cyclone's per_file_copt (clang-libstdcxx13) uses -nostdinc++ and hardcodes the
  # Ubuntu multiarch target-config dir -I/usr/include/x86_64-linux-gnu/c++/13 (where
  # <bits/c++config.h> lives). gcc-toolset-13 names its target subdir
  # x86_64-redhat-linux INSIDE c++/13, so point the Ubuntu path at it; without this
  # cyclone fails "'bits/c++config.h' file not found".
  ln -sfn "${GCC13_PREFIX}/include/c++/13/x86_64-redhat-linux" /usr/include/x86_64-linux-gnu/c++/13
  # clang-21's default layering_check/header-modules trip on abseil's deps under
  # libstdc++ ("module ... does not depend on a module exporting <cstddef>"); these
  # are header-hygiene lints, not codegen — disable them.
  BAZEL_FLAGS+=(
    "--config=clang-libstdcxx13"
    "--features=-layering_check"
    "--features=-parse_headers"
    "--features=-use_header_modules"
  )
else
  BAZEL_FLAGS+=( "--config=clang-libstdcxx13" )
fi

# ---------------------------------------------------------------------------
# 3. Build the module .so target. We build the .so (NOT just the archive) to
#    FORCE Bazel to compile the entire PSOL object graph and emit every .pic.o
#    we need for the merged archive. The .so Bazel emits is the dev-nginx one
#    (wrong version) — we discard it; we only want its inputs. Then we build the
#    thin archive target too so the support objects are present.
# ---------------------------------------------------------------------------
# --output_user_root is a STARTUP flag (must precede the command) and pins the
# bazel server. It MUST be identical across build/info/aquery or each lands on a
# different server with a different execution_root (the "cannot resolve
# execution_root" failure). Apply it uniformly via BAZEL_STARTUP.
BAZEL_STARTUP=( "--output_user_root=/tmp/bazel-out" )

echo "==> bazel build ngx_pagespeed_module.so (forces full PSOL graph)"
"${BAZEL}" "${BAZEL_STARTUP[@]}" build "${BAZEL_FLAGS[@]}" \
  -- //pagespeed/nginx:ngx_pagespeed_module.so //pagespeed/nginx:ngx_pagespeed_archive

EXECROOT="$("${BAZEL}" "${BAZEL_STARTUP[@]}" info "${BAZEL_FLAGS[@]}" execution_root 2>/dev/null)"
[ -n "${EXECROOT}" ] && [ -d "${EXECROOT}" ] || { echo "ERROR: cannot resolve execution_root" >&2; exit 1; }
echo "    execution_root: ${EXECROOT}"

# ---------------------------------------------------------------------------
# 4. Assemble the merged PSOL archive from the CppLink action's Inputs.
#    aquery emits "Inputs: [a, b, c, ...]" as ONE comma-separated bracketed
#    line (journal: split on ',[]', NOT $-anchored grep). We keep .o/.pic.o
#    inputs, MINUS the two module-entry objects (compiled separately by nginx
#    via --add-dynamic-module), and `ar qcsP` them (P preserves member paths so
#    same-basename members don't clobber). The 4 vendored static libs
#    (libcurl + libmemcached x3) are .a not loose .o, so each is extracted into
#    its OWN dir then appended (else same-named members clobber -> late
#    "undefined symbol: hashkit_string_c_str" at load).
# ---------------------------------------------------------------------------
WORK="$(mktemp -d -t ngxmod.XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
MERGED="${WORK}/libpagespeed_psol_full.a"

echo "==> aquery CppLink inputs for the merged archive"
"${BAZEL}" "${BAZEL_STARTUP[@]}" aquery "${BAZEL_FLAGS[@]}" \
  "mnemonic(CppLink, //pagespeed/nginx:ngx_pagespeed_module.so)" 2>/dev/null \
  > "${WORK}/link_aquery.txt"

# Extract the bracketed Inputs list -> one path per line.
INPUTS_LINE="$(grep -m1 -oE 'Inputs: \[.*\]' "${WORK}/link_aquery.txt" || true)"
[ -n "${INPUTS_LINE}" ] || { echo "ERROR: no 'Inputs: [...]' in aquery output" >&2; exit 1; }
# Strip the "Inputs: [" prefix and trailing "]", split on commas.
echo "${INPUTS_LINE}" \
  | sed -E 's/^Inputs: \[//; s/\]$//' \
  | tr ',' '\n' \
  | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//' \
  | grep -v '^$' > "${WORK}/all_inputs.txt"

# The two module-entry objects nginx compiles itself (drop from the archive).
DROP_RE='(/ngx_pagespeed_module\.so/ngx_pagespeed_module\.pic\.o|/nginx_pagespeed/ngx_pagespeed\.pic\.o)$'

# Object inputs (.o/.pic.o), minus the two module-entry objs.
grep -E '\.(o|pic\.o)$' "${WORK}/all_inputs.txt" \
  | grep -vE "${DROP_RE}" > "${WORK}/psol_objs.txt" || true
NOBJS="$(wc -l < "${WORK}/psol_objs.txt" | tr -d ' ')"
echo "    PSOL object members: ${NOBJS}"
[ "${NOBJS}" -gt 100 ] || { echo "ERROR: implausibly few PSOL objects (${NOBJS})" >&2; exit 1; }

# Materialization guard: every link-input .pic.o MUST be present loose
# in the execroot or `ar` would silently drop it -> undefined symbols on the
# optimizing path. A no-disk_cache build (the default above) materializes all of
# them; this guard HARD-FAILS if any are absent (e.g. someone forced
# ALLOW_DISK_CACHE=1 and hit the cache-staging gap).
MISSING=0
while IFS= read -r o; do [ -e "${EXECROOT}/${o}" ] || { echo "    MISSING in execroot: ${o}" >&2; MISSING=$((MISSING+1)); }; done < "${WORK}/psol_objs.txt"
[ "${MISSING}" -eq 0 ] || {
  echo "ERROR: ${MISSING}/${NOBJS} PSOL link-input object(s) not materialized in execroot." >&2
  echo "       The merged archive would be incomplete (undefined symbols at module load)." >&2
  echo "       This happens with a warm --disk_cache; build without it (unset ALLOW_DISK_CACHE)." >&2
  exit 1
}
echo "    all ${NOBJS} PSOL objects materialized in execroot"

# Build the archive. Paths are execroot-relative; run ar from execroot, P-mode.
# Fail loudly if ar drops any member (was previously silent under a broken pipe).
( cd "${EXECROOT}" && tr '\n' ' ' < "${WORK}/psol_objs.txt" \
    | xargs ar qcsP "${MERGED}" ) || { echo "ERROR: ar failed assembling merged archive" >&2; exit 1; }

# Append the 4 vendored static libs, each extracted into its OWN dir.
echo "==> appending vendored static libs (curl + libmemcached x3)"
for libname in libcurl.a libhashkit.a libmemcached.a libmemcachedutil.a; do
  libpath="$(grep -E "/${libname}$" "${WORK}/all_inputs.txt" | head -1 || true)"
  if [ -z "${libpath}" ]; then
    echo "ERROR: vendored ${libname} not found in CppLink inputs" >&2
    exit 1
  fi
  abslib="${EXECROOT}/${libpath}"
  [ -f "${abslib}" ] || abslib="${libpath}"  # absolute fallback
  exd="${WORK}/extract/${libname%.a}"
  mkdir -p "${exd}"
  ( cd "${exd}" && ar x "${abslib}" )
  ( cd "${exd}" && find . -name '*.o' -print0 | xargs -0 ar qPS "${MERGED}" )
done
ar s "${MERGED}"
echo "    merged archive: $(du -h "${MERGED}" | cut -f1)"

# ---------------------------------------------------------------------------
# 5. Harvest the PSOL include dirs from the archive's CppCompile actions and
#    pass them to nginx's configure via --with-cc-opt. (Journal: the config's
#    in-configure harvest silently yields empty under vendored builds, so the
#    build script must do it where bazel is reliable.) Drop external/nginx/* so
#    the glue compiles against the TARGET nginx headers, not Bazel's pinned set.
# ---------------------------------------------------------------------------
echo "==> harvest PSOL include dirs"
"${BAZEL}" "${BAZEL_STARTUP[@]}" aquery "${BAZEL_FLAGS[@]}" \
  "mnemonic(CppCompile, //pagespeed/nginx:ngx_pagespeed_archive)" 2>/dev/null \
  | sed 's/\\$//' | tr '\n' ' ' \
  | grep -oE '\-I[^ ]+|-iquote +[^ ]+|-isystem +[^ ]+' \
  | sed -E 's/^-I//; s/^-(iquote|isystem) +//' \
  | grep -vE '(^|/)external/nginx(/|$)' \
  | sort -u > "${WORK}/incdirs_raw.txt"

CC_OPT=""
while IFS= read -r d; do
  [ -n "${d}" ] || continue
  case "${d}" in
    .)  abs="${EXECROOT}" ;;
    /*) abs="${d}" ;;
    *)  abs="${EXECROOT}/${d}" ;;
  esac
  [ -d "${abs}" ] && CC_OPT="${CC_OPT} -I${abs}"
done < "${WORK}/incdirs_raw.txt"
# Source-tree includes the config also adds.
CC_OPT="${CC_OPT} -I${SRCDIR} -I${SRCDIR}/third_party/css_parser/src -I${SRCDIR}/third_party/domain_registry_provider/src"
# nginx's module Makefile uses -Werror; the glue trips override/unused-set
# warnings the Bazel build silenced. -Wno-error keeps make from failing on them.
CC_OPT="${CC_OPT} -Wno-error -std=c++17"

# ---------------------------------------------------------------------------
# 6. Build the module against the SAME target nginx tree we pre-configured in
#    step 2a (and that the Bazel @nginx archive half compiled against). Re-run
#    ./configure adding --add-dynamic-module now that the merged archive exists.
#    Reusing the identical tree guarantees both halves share one nginx ABI.
# ---------------------------------------------------------------------------
[ -d "${NGX_SRC}/src" ] || { echo "ERROR: pre-configured nginx tree missing at ${NGX_SRC}" >&2; exit 1; }

echo "==> configure nginx --add-dynamic-module + make modules"
# The config file resolves the archive via PAGESPEED_ARCHIVE / PAGESPEED_PSOL_ARCHIVE
# env (its own cquery/bazel-bin resolution returns empty from inside nginx's
# configure under vendored builds). Point both at the merged archive.
export MOD_PAGESPEED_DIR="${SRCDIR}"
export PAGESPEED_ARCHIVE="${MERGED}"
export PAGESPEED_PSOL_ARCHIVE="${MERGED}"

(
  cd "${NGX_SRC}"
  ./configure \
    --add-dynamic-module="${SRCDIR}/pagespeed/nginx" \
    --with-compat \
    --with-http_ssl_module \
    --with-http_v2_module \
    --with-threads \
    --with-cc-opt="${CC_OPT}"
  make modules
)

BUILT_SO="${NGX_SRC}/objs/ngx_pagespeed.so"
[ -f "${BUILT_SO}" ] || { echo "ERROR: make modules produced no ngx_pagespeed.so" >&2; exit 1; }

mkdir -p "$(dirname "${OUT_SO}")"
install -m 644 "${BUILT_SO}" "${OUT_SO}"

# ---------------------------------------------------------------------------
# 7. Sanity: embedded module->version must match the target nginx; D4 hygiene.
# ---------------------------------------------------------------------------
EXPECT_VER="$(echo "${NGINX_SRC_VERSION}" | awk -F. '{printf "%d%03d%03d", $1,$2,$3}')"
if command -v strings >/dev/null 2>&1; then
  if strings "${OUT_SO}" | grep -q "${EXPECT_VER}"; then
    echo "OK: module embeds nginx version ${EXPECT_VER} (${NGINX_SRC_VERSION})"
  else
    echo "::warning::could not confirm embedded nginx version ${EXPECT_VER} by string scan (GATE 1 load-check is authoritative)"
  fi
fi
"${SCRIPTDIR}/assert_symbol_hygiene.sh" "${OUT_SO}"

echo "============================================================"
echo "Built ABI-correct module for ${DISTRO}: ${OUT_SO}"
ls -lh "${OUT_SO}"
echo "============================================================"
