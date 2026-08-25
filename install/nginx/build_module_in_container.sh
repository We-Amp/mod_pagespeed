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
#   BAZEL_CPUS          optional CPU cap for the Bazel build: adds
#                       --local_cpu_resources=N --jobs=N so peak RSS stays
#                       ~1.5 GB x N, letting several per-distro legs share one box
#                       (release.yml fan-out). Unset = use all host cores (prior
#                       behavior, uncapped).
#   MAKE_JOBS           parallelism for the nginx `make modules` link
#                       (default: BAZEL_CPUS if set, else nproc).

set -euo pipefail
if [ "${VERBOSE:-}" ]; then set -x; fi

DISTRO="${1:?usage: $0 <noble|el9|el10|bullseye|bookworm|trixie|jammy> [-o out.so]}"
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
  # focal is the design record SIDECAR portable path ONLY (not a distro-stock package
  # target). Its nginx is bumped to the 1.30 stable branch for GA: newer branch, smaller CVE backlog than 1.24.0 (2023).
  # sha256 captured from nginx.org/download/nginx-1.30.4.tar.gz 2026-07-23 (security bump 1.30.3 -> 1.30.4: CVE-2026-42533, CVE-2026-60005, CVE-2026-56434).
  focal) DEF_NGINX_VER="1.30.4"
         DEF_NGINX_SHA256="4261dc90e9e47c1c4041276e9aaa3d48ebe2e664f728e14fa95ae6c67d57a08b" ;;
  el9)   DEF_NGINX_VER="1.20.1"
         DEF_NGINX_SHA256="e462e11533d5c30baa05df7652160ff5979591d291736cfa5edb9fd2edb48c49" ;;
  # el10 (AlmaLinux/RHEL/Rocky/CloudLinux 10, the design record): distro-source mode like
  # el9 — this literal is only a pre-resolution fallback; the SRPM branch below
  # overwrites NGINX_SRC_VERSION from `dnf repoquery` (AlmaLinux 10 AppStream
  # ships non-modular nginx 1.26.3-1.el10 today). sha256 = the verified
  # nginx.org/download/nginx-1.26.3.tar.gz hash (same pin as trixie).
  el10)  DEF_NGINX_VER="1.26.3"
         DEF_NGINX_SHA256="69ee2b237744036e61d24b836668aad3040dda461fe6f570f1787eab570c75aa" ;;
  # the design record P3 — Debian/Ubuntu distro-stock prebuilt nginx-module-pagespeed
  # targets. Each pins the EXACT nginx version of that distro's stock `nginx`
  # package so the module's embedded module->version + NGX_MODULE_SIGNATURE match
  # what the host's apt-installed nginx loads (the ABI contract; M0-proven pins).
  # sha256s captured from nginx.org/download 2026-06-04.
  bullseye) DEF_NGINX_VER="1.18.0"
            DEF_NGINX_SHA256="4c373e7ab5bf91d34a4f11a0c9496561061ba5eee6020db272a17a7228d35f99" ;;
  bookworm) DEF_NGINX_VER="1.22.1"
            DEF_NGINX_SHA256="9ebb333a9e82b952acd3e2b4aeb1d4ff6406f72491bab6cd9fe69f0dea737f31" ;;
  trixie)   DEF_NGINX_VER="1.26.3"
            DEF_NGINX_SHA256="69ee2b237744036e61d24b836668aad3040dda461fe6f570f1787eab570c75aa" ;;
  jammy)    DEF_NGINX_VER="1.18.0"
            DEF_NGINX_SHA256="4c373e7ab5bf91d34a4f11a0c9496561061ba5eee6020db272a17a7228d35f99" ;;
  *) echo "ERROR: unknown distro '${DISTRO}' (want noble|focal|el9|el10|bullseye|bookworm|trixie|jammy)" >&2; exit 1 ;;
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

# Optional nginx HTTP modules compiled into BOTH the pre-configure (@nginx ABI
# source) and the module/binary build, kept identical so the matched pair stays
# ABI-consistent (D3). The sidecar's generated config is HTTP-only — it emits no
# ssl/http2 directives — so the PORTABLE focal build OMITS the ssl + http_v2
# modules. That drops the nginx binary's libssl/libcrypto NEEDED entries, so a
# single binary runs on both OpenSSL-1.1 hosts (Debian 11) and OpenSSL-3 hosts
# (Debian 12+) — the SONAME split that would otherwise block the glibc-2.31 reach
# goal. noble/el9 (distro-stock parity) keep them.
NGX_HTTP_MODULES=( --with-http_ssl_module --with-http_v2_module )
if [ "${DISTRO}" = "focal" ]; then NGX_HTTP_MODULES=(); fi

# ---------------------------------------------------------------------------
# Distro family classification.
#
# DEB_STATIC_STDCXX: the Debian/Ubuntu distro-stock targets (bullseye, bookworm,
# trixie, jammy) — plus the pre-existing portable focal — all build the module
# with the focal recipe: clang as the COMPILER, GCC-13's libstdc++ as the C++
# stdlib, and the module .so STATICALLY linking libstdc++/libgcc so it carries NO
# libstdc++.so.6 / GLIBCXX_* runtime dependency. That static link is essential on
# the older targets because the libstdc++-13 we build against is NEWER than the
# distro's stock libstdc++ (esp. bullseye glibc-2.31 / bookworm 2.36, where the
# GCC-13 headers+static archive are sideloaded from the trixie .deb — see
# install_deps_debian_common). With the static link the ONLY ABI floor that
# survives into the produced .so is glibc, and building inside the target
# container caps that at the target's own glibc.
#
# DEB_TRIPLET: the multiarch triplet (x86_64-linux-gnu | aarch64-linux-gnu),
# derived (NEVER hardcoded) so the same recipe builds amd64 AND arm64.
case "${DISTRO}" in
  bullseye|bookworm|trixie|jammy) DEB_STATIC_STDCXX=1 ;;
  *) DEB_STATIC_STDCXX=0 ;;
esac

# ---------------------------------------------------------------------------
# NGX_SRC_MODE — where the @nginx ABI source comes from.
#
# distro  : the DISTRO's OWN patched nginx tree, pinned to the exact stock
#           candidate the host runs (deb: `apt-get source nginx`; el9: AlmaLinux's
#           signed nginx SRPM via `dnf download --source` + `rpmbuild -bp`).
#           REQUIRED for the Ubuntu suites because a distro security update can
#           change the layout of core request structs WITHOUT bumping the upstream
#           version: Ubuntu shipped CVE-2026-49975 (HTTP/2 max_headers) as nginx
#           1.24.0-2ubuntu7.10 (noble) / 1.18.0-6ubuntu14.13 (jammy) on 2026-06-05,
#           inserting `ngx_uint_t count` mid-struct into ngx_http_headers_in_t —
#           which is embedded BY VALUE in ngx_http_request_t, so every downstream
#           field offset shifts 8 bytes. A module compiled against vanilla
#           nginx.org 1.24.0 then reads garbage request state and the worker SPINS
#           forever on the first optimizing request (a SILENT hang; `nginx -t`
#           still passes because the module->version + NGX_MODULE_SIGNATURE match,
#           and --with-compat does NOT pad ngx_http_headers_in_t). Building against
#           the distro's patched source makes the module's offsets match.
#           - noble/jammy (Ubuntu): the confirmed-drifting targets (above).
#           - el9 (AlmaLinux): NOT drifting today — stock nginx is the frozen
#             AppStream stream and does NOT backport the CVE-2026-49975 core-struct
#             change. el9 uses distro-source for deb PARITY + FUTURE-PROOFING: if
#             Alma ever backports a struct change, the distro-source build captures
#             it automatically; vanilla would silently drift. (Not an active-
#             incident fix.)
# vanilla : nginx.org release tarball (sha256-pinned). Kept for the targets that
#           are not distro-stock or are ABI-safe by the distro's design:
#           - bullseye/bookworm/trixie (Debian): Debian deliberately keeps the
#             HTTP/2 max_headers fix OUT of the core structs — it relocated the
#             counter into a separate ngx_http_header_count_module "to avoid
#             potential ABI breakage and keep all 3rd-party modules compatible
#             without recompilation" — so vanilla-built modules stay ABI-correct
#             and the proven-passing Debian builds are left untouched.
#           - focal: the design record sidecar PORTABLE build (a newer 1.30 branch,
#             not a distro-stock package).
#           (Debian suites validated unaffected 2026-06-08; see the design record P4/P5.)
#
# Canonical (unlike Debian) shipped CVE-2026-49975 by editing the core request
# structs in place, in BOTH noble (1.24.0-2ubuntu7.10) and jammy
# (1.18.0-6ubuntu14.13) on 2026-06-05; el9 joins distro-source for parity.
#
# NOTE (residual): a prebuilt dynamic module is only ABI-safe against the nginx
# revision it was built against. If a distro ships ANOTHER struct-changing
# security revision AFTER this build, a customer who upgrades nginx past it would
# hit the same drift. The backstop is the runtime ABI guard in ngx_pagespeed.cc
# (ps_preaccess_handler / ps_report_abi_mismatch, the design record P5): it cannot refuse
# to LOAD (nginx exposes neither its struct layout nor its distro revision at
# runtime), but it detects the drift on the first request and degrades the module
# to pass-through + one ALERT log instead of silently hanging the worker.
case "${DISTRO}" in
  noble|jammy|el9|el10) NGX_SRC_MODE="distro" ;;
  *)                    NGX_SRC_MODE="vanilla" ;;
esac
NGX_SRC_MODE="${NGX_SRC_MODE_OVERRIDE:-${NGX_SRC_MODE}}"

# Derive the Debian multiarch triplet (x86_64-linux-gnu | aarch64-linux-gnu)
# WITHOUT hardcoding the arch. Prefer dpkg-architecture (needs dpkg-dev), fall
# back to `gcc -dumpmachine`-style mapping from `dpkg --print-architecture` so it
# also works BEFORE the build deps are installed.
deb_triplet() {
  local t
  t="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)"
  if [ -z "${t}" ]; then
    case "$(dpkg --print-architecture 2>/dev/null || uname -m)" in
      amd64|x86_64)        t="x86_64-linux-gnu" ;;
      arm64|aarch64)       t="aarch64-linux-gnu" ;;
      *) echo "ERROR: cannot derive multiarch triplet for $(uname -m)" >&2; return 1 ;;
    esac
  fi
  printf '%s' "${t}"
}

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
# build toolchain. The launcher asset is arch-specific, so the same recipe can
# build the matched (nginx + module) pair on both linux-amd64 and linux-arm64
#. Update the version and BOTH per-arch checksums
# together when bumping.
BAZELISK_VERSION="v1.29.0"
BAZELISK_SHA256_AMD64="5a408715e932c0250d28bd84555f12edbf70117de42f9181691c736eacc4a992"
BAZELISK_SHA256_ARM64="e20e8b0f4f240091b7a55bf17b9398bd4f40ee70ae0208dff95dd4c445fb4010"

install_bazelisk() {
  if command -v "${BAZEL}" >/dev/null 2>&1; then
    return 0
  fi
  # Select the launcher asset + checksum for the build container's architecture.
  local arch asset sha
  arch="$(uname -m)"
  case "${arch}" in
    x86_64|amd64)  asset="bazelisk-linux-amd64"; sha="${BAZELISK_SHA256_AMD64}" ;;
    aarch64|arm64) asset="bazelisk-linux-arm64"; sha="${BAZELISK_SHA256_ARM64}" ;;
    *) echo "ERROR: unsupported architecture '${arch}' for bazelisk ${BAZELISK_VERSION}" >&2; exit 1 ;;
  esac
  local url="https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/${asset}"
  wget -qO /usr/local/bin/bazelisk "${url}"
  if ! echo "${sha}  /usr/local/bin/bazelisk" | sha256sum -c - >/dev/null 2>&1; then
    echo "ERROR: bazelisk ${BAZELISK_VERSION} (${asset}) checksum mismatch; refusing to use it" >&2
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
    python3 unzip zip gperf bison flex nasm \
    dpkg-dev fakeroot >/dev/null
  # bazelisk (pinned + checksum-verified) if bazel/bazelisk not already provided.
  install_bazelisk
}

install_deps_focal() {
  # Ubuntu 20.04 (glibc 2.31) build base for the PORTABLE sidecar pair.
  # Building here gives the module a glibc-2.31 floor — focal's glibc headers do
  # NOT emit the __isoc23_* GLIBC_2.38 redirects that the noble (glibc 2.39) base
  # bakes in — and combined with -static-libstdc++/-static-libgcc on the module
  # link (step 6, focal-only LD_OPT) the .so carries NO libstdc++ runtime dep, so
  # it loads on Debian 11/12, Ubuntu 20.04+, RHEL/Alma 8/9, Amazon Linux 2023.
  # focal's stock toolchain is too old for the C++20/23 PSOL/Cyclone graph, so add
  # the LLVM apt repo (clang) + the toolchain PPA (g++-13 -> the libstdc++-13
  # headers/libs the clang-libstdcxx13 bazel config expects at /usr/{include/c++,
  # lib/gcc/<triple>}/13).
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y --no-install-recommends \
    ca-certificates curl wget gnupg lsb-release software-properties-common \
    build-essential \
    zlib1g-dev libpcre3-dev libssl-dev \
    python3 unzip zip gperf bison flex nasm \
    dpkg-dev fakeroot >/dev/null
  # g++-13 / libstdc++-13 from the Ubuntu toolchain PPA (focal ships g++-9).
  add-apt-repository -y ppa:ubuntu-toolchain-r/test >/dev/null
  apt-get update -qq
  apt-get install -y --no-install-recommends g++-13 gcc-13 libstdc++-13-dev >/dev/null
  # clang/lld/llvm from apt.llvm.org (focal's stock clang predates C++20/23 support).
  local CLANG_VER=18
  wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
  chmod +x /tmp/llvm.sh
  /tmp/llvm.sh ${CLANG_VER} >/dev/null 2>&1
  apt-get install -y --no-install-recommends \
    clang-${CLANG_VER} lld-${CLANG_VER} llvm-${CLANG_VER} >/dev/null
  # The clang-libstdcxx13 bazel config + nginx make invoke clang/clang++/ld.lld by
  # bare name; apt.llvm.org installs versioned binaries only.
  update-alternatives --install /usr/bin/clang   clang   /usr/bin/clang-${CLANG_VER}   100
  update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-${CLANG_VER} 100
  ln -sf "/usr/bin/ld.lld-${CLANG_VER}" /usr/bin/ld.lld
  # bazelisk (pinned + checksum-verified) if bazel/bazelisk not already provided.
  install_bazelisk
}

install_deps_el9() {
  # --allowerasing: almalinux:9 ships curl-minimal, which conflicts with the full
  # `curl` package; allow dnf to swap it rather than abort on "conflicting requests".
  # perl: libaom's cmake configure hard-requires it (RTCD/asm generation). On el9
  # it arrives transitively anyway; pin it explicitly so a slimmer future base
  # image can't regress the aom build (el10's base did exactly that).
  dnf install -y --allowerasing --setopt=install_weak_deps=False \
    ca-certificates curl wget \
    gcc-toolset-13 \
    clang lld llvm \
    zlib-devel pcre-devel openssl-devel \
    python3 perl unzip zip gperf bison flex \
    rpm-build dnf-plugins-core tar gzip make >/dev/null
  # libaom (AVIF, r20) hard-requires nasm for its x86 SIMD (ENABLE_NASM=1 in
  # bazel/libaom.bzl). On el9 nasm lives in CRB (verified: base repos have no
  # match), and EL ships it for x86_64 only — aarch64 aom uses GNU as — so
  # enable CRB and gate by arch.
  if [ "$(uname -m)" = "x86_64" ]; then
    dnf config-manager --set-enabled crb 2>/dev/null \
      || dnf config-manager --enable crb 2>/dev/null || true
    dnf install -y --setopt=install_weak_deps=False nasm >/dev/null
  fi
  # bazelisk (pinned + checksum-verified) if bazel/bazelisk not already provided.
  install_bazelisk
}

install_deps_el10() {
  # el10 (almalinux:10, the design record): gcc-toolset-15 (gt-15 libstdc++, C++23-complete
  # for Cyclone; base gcc is 14.x) + system clang-21 (AppStream — already >= the
  # clang-20 abseil absl_nonnull floor, so NO sideload) + pcre2-devel (el10 dropped
  # pcre1). gperf/bison/flex + some -devel live in CRB on el10, so enable it first.
  # Mirrors install_deps_el9 with the -13->-15 / pcre->pcre2 / CRB substitutions
  # proven by build_apache_el10_in_container.sh.
  dnf install -y --setopt=install_weak_deps=False dnf-plugins-core >/dev/null
  dnf config-manager --set-enabled crb 2>/dev/null \
    || dnf config-manager --enable crb 2>/dev/null || true
  # gnupg2 is REQUIRED on el10 (NOT optional): the AlmaLinux nginx.spec %prep runs
  # %{gpgverify}, which shells out to gpg2 to verify the upstream nginx tarball
  # signature. The almalinux:10 BASE image does NOT ship gpg2 (almalinux:9 does, as
  # a transitive dep — which is why install_deps_el9 never needed it), so without
  # this the el10 distro-source `rpmbuild -bp` fails at %prep with
  # "gpgverify: gpg2: command not found" and the el10 nginx module silently never
  # builds (the leg is soft). Verified in a real almalinux:10 container.
  # perl: libaom's cmake configure hard-requires it ("Perl is required to build
  # libaom", aom_configure.cmake) — the almalinux:10 base image, unlike el9,
  # ships NO perl even transitively; r20 attempt 4d el10 leg failed exactly
  # there. Verified installable in a real almalinux:10 container (perl 5.40).
  dnf install -y --allowerasing --setopt=install_weak_deps=False \
    ca-certificates curl wget gnupg2 \
    gcc-toolset-15 \
    clang lld llvm \
    zlib-devel pcre2-devel openssl-devel \
    python3 perl unzip zip gperf bison flex \
    rpm-build dnf-plugins-core tar gzip make which file >/dev/null
  # libaom (AVIF, r20) hard-requires nasm for its x86 SIMD (ENABLE_NASM=1 in
  # bazel/libaom.bzl); EL packages nasm for x86_64 only, and aarch64 aom uses
  # GNU as, so gate by arch.
  if [ "$(uname -m)" = "x86_64" ]; then
    dnf install -y --setopt=install_weak_deps=False nasm >/dev/null
  fi
  # bazelisk (pinned + checksum-verified) if bazel/bazelisk not already provided.
  install_bazelisk
}

# --- the design record P3 Debian/Ubuntu distro-stock targets -------------------------
# Shared apt baseline: ca-certs/build tools/nginx build-deps. NO g++/clang here;
# the per-distro function adds the C++23 toolchain (sources differ per distro).
install_deps_debian_common() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y --no-install-recommends \
    ca-certificates curl wget gnupg lsb-release \
    build-essential make \
    zlib1g-dev libssl-dev \
    python3 unzip zip gperf bison flex nasm \
    dpkg-dev fakeroot >/dev/null
  # PCRE dev headers for nginx. Older distros (bullseye/bookworm/jammy) ship the
  # legacy libpcre3-dev (PCRE 8.x); trixie DROPPED it and ships only libpcre2-dev
  # (nginx >= 1.21.5 prefers PCRE2 anyway). Install whichever the distro provides.
  if apt-get install -y --no-install-recommends libpcre3-dev >/dev/null 2>&1; then
    echo "    PCRE: libpcre3-dev"
  elif apt-get install -y --no-install-recommends libpcre2-dev >/dev/null 2>&1; then
    echo "    PCRE: libpcre2-dev"
  else
    echo "ERROR: neither libpcre3-dev nor libpcre2-dev is installable on ${DISTRO}" >&2
    exit 1
  fi
}

# Enable deb-src across BOTH apt source formats so `apt-get source nginx` works:
#   - deb822 (.sources): ubuntu:24.04 ubuntu.sources, debian:12/13 debian.sources
#   - classic one-line:  ubuntu:22.04, debian:11 /etc/apt/sources.list
# apt verifies the fetched source against the repo's GPG-signed Release (.dsc +
# per-file checksums), so this carries the same supply-chain trust as the
# vanilla path's hardcoded tarball sha256.
enable_deb_src() {
  local f
  for f in /etc/apt/sources.list.d/*.sources; do
    [ -e "${f}" ] || continue
    sed -i -E 's/^(Types:[[:space:]]+deb)[[:space:]]*$/\1 deb-src/' "${f}"
  done
  if [ -f /etc/apt/sources.list ] && grep -qE '^[[:space:]]*deb[[:space:]]' /etc/apt/sources.list \
     && ! grep -qE '^[[:space:]]*deb-src[[:space:]]' /etc/apt/sources.list; then
    sed -nE 's/^[[:space:]]*deb[[:space:]]+(.*)$/deb-src \1/p' /etc/apt/sources.list \
      >> /etc/apt/sources.list
  fi
  apt-get update -qq
}

# Install a modern clang (C++23-capable) from apt.llvm.org and alias the bare
# clang/clang++/ld.lld names the clang-libstdcxx13 config + nginx make expect.
# The clang BINARY links against the container's own glibc, so it runs on the
# target distro (this is why clang — not g++-13 — is the compiler on the
# old-glibc targets where a native g++-13 either doesn't exist [bullseye] or
# would drag a newer libc6 [bookworm]).
install_llvm_clang() {
  local CLANG_VER="$1"
  apt-get install -y --no-install-recommends software-properties-common >/dev/null
  wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
  chmod +x /tmp/llvm.sh
  /tmp/llvm.sh "${CLANG_VER}" >/dev/null 2>&1
  apt-get install -y --no-install-recommends \
    clang-${CLANG_VER} lld-${CLANG_VER} llvm-${CLANG_VER} >/dev/null
  update-alternatives --install /usr/bin/clang   clang   /usr/bin/clang-${CLANG_VER}   100
  update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-${CLANG_VER} 100
  ln -sf "/usr/bin/ld.lld-${CLANG_VER}" /usr/bin/ld.lld
}

# Sideload GCC-13's libstdc++ (headers + the static libstdc++.a) WITHOUT
# installing it through apt — apt would pull the donor's libc6-dev and upgrade
# the whole container's glibc, destroying the old-glibc target's floor (verified:
# trixie libstdc++-13-dev Depends: libc6-dev>=2.41 -> would bump bullseye 2.31 /
# bookworm 2.36 to 2.41). We only need the C++ stdlib headers + the static
# archive, which `dpkg-deb -x /` lays down at exactly the paths
# --config=clang-libstdcxx13 hardcodes (/usr/include/c++/13,
# /usr/include/<triplet>/c++/13, /usr/lib/gcc/<triplet>/13). clang supplies its
# own builtin C headers, so no gcc-13 binary is needed at all.
#
# WHICH gcc-13 libstdc++ (the binding constraint, the design record P3 / the EA4 lesson):
# the CURRENT trixie gcc-13 (13.3.0) was compiled against glibc >= 2.38, so its
# libstdc++.a's eh_alloc.o/debug.o reference __isoc23_strtoul@GLIBC_2.38. Static-
# linking THAT into the module would (a) HARD-FAIL the link on bullseye (glibc
# 2.31 has no __isoc23_strtoul — surfaces first as the libmemcached `memcapable`
# foreign_cc exe link error) and (b) push the .so's glibc floor to 2.38, breaking
# the bullseye 2.31 + bookworm 2.36 targets. So we pin the LAST gcc-13 libstdc++
# uploaded BEFORE the glibc-2.38 __isoc23 cutover — Debian snapshot 13.2.0-11
# (built vs glibc 2.37) — which references plain strtoul and whose worst-case
# (no-gc-sections) glibc floor is 2.14. Verified 2026-06-04: 13.2.0-11 has 0
# __isoc23 refs; the very next upload 13.3.0-1 has 2. snapshot.debian.org by-hash
# URLs are permanent + content-addressed; we ALSO sha256-verify the .deb.
LIBSTDCXX13_SNAPSHOT_VER="13.2.0-11"
LIBSTDCXX13_SNAP_HASH_amd64="70b041d0254e9becb2c5e72048344e90d3e81caf"
LIBSTDCXX13_SNAP_HASH_arm64="bcef351fec8d9fab7aef66359f9b42cb8af0536a"
LIBSTDCXX13_SNAP_SHA256_amd64="df64df8c345c06edad7f0e7a4291341539c8d2eb6a5d75f6c9d9fb83b27e6a50"
LIBSTDCXX13_SNAP_SHA256_arm64="a07a0db3fbb2a72594349e6e37f8ed11ea622a7f229a1bc7286fc87cf9c7bf97"
sideload_libstdcxx13_pinned() {
  local triplet debarch snaphash snapsha url deb
  triplet="$(deb_triplet)"
  debarch="$(dpkg --print-architecture)"
  case "${debarch}" in
    amd64) snaphash="${LIBSTDCXX13_SNAP_HASH_amd64}"; snapsha="${LIBSTDCXX13_SNAP_SHA256_amd64}" ;;
    arm64) snaphash="${LIBSTDCXX13_SNAP_HASH_arm64}"; snapsha="${LIBSTDCXX13_SNAP_SHA256_arm64}" ;;
    *) echo "ERROR: no pinned libstdc++-13 snapshot for arch '${debarch}'" >&2; exit 1 ;;
  esac
  url="https://snapshot.debian.org/file/${snaphash}"
  deb="/tmp/libstdc++-13-dev.deb"
  echo "    sideloading GCC-13 libstdc++ ${LIBSTDCXX13_SNAPSHOT_VER} (pre-isoc23, glibc-safe) from ${url}"
  wget -qO "${deb}" "${url}"
  echo "${snapsha}  ${deb}" | sha256sum -c - \
    || { echo "ERROR: pinned libstdc++-13 .deb sha256 mismatch (got $(sha256sum "${deb}" | awk '{print $1}'), expected ${snapsha})" >&2; exit 1; }
  # Extract into / — lands /usr/include/c++/13, /usr/include/<triplet>/c++/13,
  # /usr/lib/gcc/<triplet>/13/libstdc++.a at the standard paths. No glibc touched.
  dpkg-deb -x "${deb}" /
  [ -f "/usr/lib/gcc/${triplet}/13/libstdc++.a" ] \
    || { echo "ERROR: sideloaded libstdc++.a missing at /usr/lib/gcc/${triplet}/13" >&2; exit 1; }
  [ -f "/usr/include/${triplet}/c++/13/bits/c++config.h" ] \
    || { echo "ERROR: sideloaded c++config.h missing at /usr/include/${triplet}/c++/13" >&2; exit 1; }
}

# bullseye-only: the GCC-13 libstdc++ we sideload was built against glibc >= 2.37
# and references two libc symbols that bullseye's glibc 2.31 does NOT provide, so
# a static link of it on bullseye leaves them undefined -> dlopen() fails on stock
# nginx (and/or the link itself fails). Both are libstdc++-internal:
#   1. __libc_single_threaded  (glibc 2.32; ios.o/ios_init.o single-thread fast
#      path in std::ios_base). Shim = a weak `char ... = 0;` ("assume
#      multi-threaded" -> libstdc++ always takes the locked path; correct, only
#      loses the single-thread optimisation).
#   2. arc4random              (glibc 2.36; random.o, std::random_device's default
#      entropy source). Shim = a real implementation over getrandom(2) (present on
#      bullseye glibc 2.31), so std::random_device keeps yielding real CSPRNG
#      entropy. arc4random_buf/_uniform provided too for completeness.
# We ar BOTH shim objects INTO the sideloaded libstdc++.a, so every link that
# pulls libstdc++ resolves them locally and NO >2.31 libc symbol leaks into the
# .so — the module's floor stays at bullseye's glibc 2.31. (bookworm/jammy/trixie
# all provide these natively, so no shim there.) The shim symbols are weak, so
# they're harmless no-ops if ever linked where libc already provides them.
embed_bullseye_libc_compat_shims() {
  local triplet liba
  triplet="$(deb_triplet)"
  liba="/usr/lib/gcc/${triplet}/13/libstdc++.a"
  [ -f "${liba}" ] || { echo "ERROR: libstdc++.a not present for shim at ${liba}" >&2; exit 1; }
  cat > /tmp/bullseye_libc_compat_shim.c <<'CSHIM'
/* glibc-2.31 (bullseye) compatibility shims for the GCC-13 libstdc++ we sideload
   (built vs glibc >= 2.37). All weak: a no-op override if libc provides them. */
#include <stddef.h>
#include <stdint.h>
#include <sys/random.h>

/* glibc 2.32 std::ios_base single-thread fast path. 0 = assume multi-threaded. */
__attribute__((weak)) char __libc_single_threaded = 0;

/* glibc 2.36 arc4random family (std::random_device entropy). Implement over
   getrandom(2) [glibc 2.25+, present on bullseye]; block until satisfied. */
static void ps_getrandom_full(void *buf, size_t n) {
    unsigned char *p = (unsigned char *)buf;
    while (n > 0) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0) { /* fall back deterministically rather than spin forever */
            for (size_t i = 0; i < n; i++) p[i] = (unsigned char)i;
            return;
        }
        p += r; n -= (size_t)r;
    }
}
__attribute__((weak)) uint32_t arc4random(void) {
    uint32_t v; ps_getrandom_full(&v, sizeof v); return v;
}
__attribute__((weak)) void arc4random_buf(void *buf, size_t n) {
    ps_getrandom_full(buf, n);
}
__attribute__((weak)) uint32_t arc4random_uniform(uint32_t upper) {
    if (upper < 2) return 0;
    uint32_t min = -upper % upper, r;   /* rejection sampling, unbiased */
    do { r = arc4random(); } while (r < min);
    return r % upper;
}
CSHIM
  clang -c -fPIC -O2 -o /tmp/bullseye_libc_compat_shim.o /tmp/bullseye_libc_compat_shim.c
  ar r "${liba}" /tmp/bullseye_libc_compat_shim.o
  ranlib "${liba}"
  echo "    embedded bullseye glibc-2.31 compat shims (__libc_single_threaded + arc4random*) into ${liba}"
}

install_deps_trixie() {
  # debian:13 — stock g++-13 (13.3.0) + stock clang-19 are both C++23-capable and
  # already lay libstdc++-13 down at the standard paths. The simplest target.
  install_deps_debian_common
  apt-get install -y --no-install-recommends \
    g++-13 gcc-13 libstdc++-13-dev \
    clang lld llvm >/dev/null
  install_bazelisk
}

install_deps_bookworm() {
  # debian:12 (glibc 2.36) — stock g++ is 12; there is no native g++-13 (not in
  # bookworm-backports either) and the trixie g++-13 would upgrade libc6 to 2.41.
  # Compiler = clang-18 from apt.llvm.org (runs on bookworm's 2.36); C++ stdlib =
  # the PINNED pre-isoc23 GCC-13 libstdc++ snapshot 13.2.0-11 (headers + static
  # archive only, no glibc bump; its glibc floor is 2.14 << 2.36). The module link
  # static-links libstdc++ (DEB_STATIC_STDCXX), so the .so floor stays bookworm's
  # glibc 2.36 — verified by the post-build GLIBC-floor check.
  install_deps_debian_common
  install_llvm_clang 18
  sideload_libstdcxx13_pinned
  install_bazelisk
}

install_deps_bullseye() {
  # debian:11 (glibc 2.31) — the hardest target: stock g++ is 10, there is NO
  # g++-13 anywhere in bullseye/bullseye-backports (backports tops out below 13),
  # and stock clang is 11 (too old for C++23). Compiler = clang-18 from
  # apt.llvm.org (its binary links bullseye's glibc 2.31, so it runs); C++ stdlib
  # = the PINNED pre-isoc23 GCC-13 libstdc++ snapshot 13.2.0-11 (headers + static
  # archive only — a CURRENT trixie 13.3 libstdc++.a references
  # __isoc23_strtoul@GLIBC_2.38 which neither links nor loads on glibc 2.31; the
  # 13.2.0-11 archive references plain strtoul, worst-case floor 2.14). Static
  # libstdc++ link keeps the produced .so at bullseye's glibc 2.31.
  install_deps_debian_common
  install_llvm_clang 18
  sideload_libstdcxx13_pinned
  embed_bullseye_libc_compat_shims
  install_bazelisk
}

install_deps_jammy() {
  # ubuntu:22.04 (glibc 2.35) — stock g++ is 11. g++-13 IS available for jammy
  # from the Ubuntu toolchain PPA (built FOR jammy, so its libstdc++ targets glibc
  # 2.35, not a newer libc), so install it natively (no trixie sideload needed).
  # Stock clang (14) is borderline for C++23, so use clang-18 from apt.llvm.org.
  install_deps_debian_common
  # add-apt-repository lives in software-properties-common (not in the common
  # baseline); install it before adding the toolchain PPA.
  apt-get install -y --no-install-recommends software-properties-common >/dev/null
  add-apt-repository -y ppa:ubuntu-toolchain-r/test >/dev/null
  apt-get update -qq
  apt-get install -y --no-install-recommends g++-13 gcc-13 libstdc++-13-dev >/dev/null
  install_llvm_clang 18
  install_bazelisk
}

if [ "${SKIP_DEPS:-}" != "1" ]; then
  echo "==> installing build deps (${DISTRO})"
  case "${DISTRO}" in
    noble)    install_deps_noble ;;
    focal)    install_deps_focal ;;
    el9)      install_deps_el9 ;;
    el10)     install_deps_el10 ;;
    bullseye) install_deps_bullseye ;;
    bookworm) install_deps_bookworm ;;
    trixie)   install_deps_trixie ;;
    jammy)    install_deps_jammy ;;
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
# protobuf well-known types, ...) are absent and the archive silently loses them
# -> undefined symbols / wrong behavior on the optimizing path. A no-disk_cache
# build forces every CppCompile to execute and write its .pic.o loose, so all
# 2900+ members
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
echo "==> fetch + pre-configure target nginx (mode=${NGX_SRC_MODE}) for @nginx ABI match"
rm -rf "${NGX_BUILD_DIR}"
mkdir -p "${NGX_BUILD_DIR}"
if [ "${NGX_SRC_MODE}" = "distro" ] && { [ "${DISTRO}" = "el9" ] || [ "${DISTRO}" = "el10" ]; }; then
  # the design record P4 ABI-drift fix, el9 + el10 — the rpm/dnf analog of the deb
  # apt-get-source path. Build against AlmaLinux's OWN signed, patched nginx
  # SOURCE rpm so the module's struct offsets match the stock nginx it dlopen's.
  # el9/el10 stock nginx is the frozen AppStream stream and does NOT backport the
  # CVE-2026-49975 core-struct change today, so this is deb parity + future-
  # proofing (if Alma ever backports a struct change the distro-source build
  # captures it; vanilla would silently drift). The fetched SRPM is GPG-signed by
  # the AlmaLinux release key — trust equal-or-stronger than the vanilla tarball
  # sha256 pin; we fail-closed on `rpm -K ... signatures OK`. The key path is
  # versioned per distro: RPM-GPG-KEY-AlmaLinux-9 (el9) vs -10 (el10).
  rpm --import "/etc/pki/rpm-gpg/RPM-GPG-KEY-AlmaLinux-${DISTRO#el}" 2>/dev/null || true
  # Derive the host RPM arch (x86_64 | aarch64) so elN-arm64 resolves its OWN stock
  # nginx candidate. AlmaLinux 9/10 aarch64 ships the same stock nginx epoch:version as
  # x86_64, so the build_nginx_rpm.sh pin holds. Pin the EXACT stock candidate the
  # smoke (and customers) run. repoquery %{version} is already epoch-free; NVR selects.
  NGX_RPM_ARCH="$(rpm --eval '%{_arch}')"
  NGX_PKG_VER="$(dnf repoquery --latest-limit 1 --qf '%{version}-%{release}' "nginx.${NGX_RPM_ARCH}" 2>/dev/null | head -1)"
  NGINX_SRC_VERSION="$(dnf repoquery --latest-limit 1 --qf '%{version}' "nginx.${NGX_RPM_ARCH}" 2>/dev/null | head -1)"
  [ -n "${NGX_PKG_VER}" ] && [ -n "${NGINX_SRC_VERSION}" ] || { echo "ERROR: cannot determine stock nginx candidate version on ${DISTRO}" >&2; exit 1; }
  echo "==> dnf download --source nginx-${NGX_PKG_VER} (upstream ${NGINX_SRC_VERSION}) on ${DISTRO}"
  mkdir -p "${NGX_BUILD_DIR}/srpm"
  dnf download --source "nginx-${NGX_PKG_VER}" --destdir "${NGX_BUILD_DIR}/srpm"
  NGX_SRPM="$(ls "${NGX_BUILD_DIR}/srpm/"*.src.rpm 2>/dev/null | head -1)"
  [ -n "${NGX_SRPM}" ] || { echo "ERROR: dnf download --source produced no .src.rpm under ${NGX_BUILD_DIR}/srpm" >&2; exit 1; }
  # Supply-chain gate: the SRPM must carry a verified GPG signature, not just a
  # digest. `rpm -K` prints 'digests signatures OK' only when the Alma key is
  # trusted; 'digests OK' alone (NOKEY) is a hard fail.
  echo "==> verifying AlmaLinux nginx SRPM signature (rpm -K)"
  rpm -K "${NGX_SRPM}" | grep -q 'signatures OK' \
    || { echo "ERROR: AlmaLinux nginx SRPM ${NGX_SRPM} is not GPG-signed/trusted: $(rpm -K "${NGX_SRPM}")" >&2; exit 1; }
  # Install the SRPM + run %prep (rpmbuild -bp) to apply AlmaLinux's full patch
  # series. --nodeps: %prep needs none of nginx's BuildRequires (perl/gd/geoip).
  # Keep rpm's _topdir CONTAINER-LOCAL: on Docker Desktop's bind mounts
  # (macOS/virtiofs) rpm's transient write-only cpio files become unopenable
  # ("cpio: open failed - Permission denied" — the rc.2 arm64 el9 leg proved
  # it). A mktemp topdir sidesteps the mount's permission semantics entirely.
  SRPM_TOPDIR=$(mktemp -d -t nginx-srpm-topdir.XXXXXX)
  rpm -i --define "_topdir ${SRPM_TOPDIR}" "${NGX_SRPM}"
  rpmbuild -bp --nodeps --define "_topdir ${SRPM_TOPDIR}" "${SRPM_TOPDIR}/SPECS/nginx.spec"
  # The prepped tree lands under the topdir's BUILD, NOT NGX_BUILD_DIR. EXACT-name
  # match (nginx-${NGINX_SRC_VERSION}) — never a glob — because %prep leaves a
  # nested FULL copy named nginx-<EVR>-src INSIDE the top-level tree.
  NGX_SRC="$(find "${SRPM_TOPDIR}/BUILD" -maxdepth 2 -type d -name "nginx-${NGINX_SRC_VERSION}" 2>/dev/null | sort | head -1)"
  [ -n "${NGX_SRC}" ] && [ -d "${NGX_SRC}/src" ] && [ -x "${NGX_SRC}/configure" ] \
    || { echo "ERROR: rpmbuild -bp did not produce a prepped nginx-${NGINX_SRC_VERSION} tree under ${SRPM_TOPDIR}/BUILD" >&2; exit 1; }
  echo "    distro nginx source: ${NGX_SRC} (Alma-signed SRPM; NGINX_VERSION $(awk -F'"' '/define NGINX_VERSION/{print $2}' "${NGX_SRC}/src/core/nginx.h"))"
elif [ "${NGX_SRC_MODE}" = "distro" ]; then
  # the design record P4 ABI-drift fix — build against the DISTRO's OWN patched nginx
  # source so the module's struct offsets match the stock nginx it dlopen's
  # (see the NGX_SRC_MODE block near the top for the CVE-2026-49975 root cause).
  enable_deb_src
  # Pin to the EXACT stock candidate the smoke (and customers) run, so the
  # build<->runtime ABI pairing is deterministic within this release.
  NGX_PKG_VER="$(apt-cache policy nginx-core 2>/dev/null | awk '/Candidate:/{print $2}')"
  [ -n "${NGX_PKG_VER}" ] || NGX_PKG_VER="$(apt-cache policy nginx 2>/dev/null | awk '/Candidate:/{print $2}')"
  [ -n "${NGX_PKG_VER}" ] || { echo "ERROR: cannot determine stock nginx candidate version on ${DISTRO}" >&2; exit 1; }
  # Upstream version = strip the Debian revision (after first '-') and any epoch.
  NGINX_SRC_VERSION="${NGX_PKG_VER%%-*}"; NGINX_SRC_VERSION="${NGINX_SRC_VERSION#*:}"
  NGX_SRC="${NGX_BUILD_DIR}/nginx-${NGINX_SRC_VERSION}"
  echo "==> apt-get source nginx=${NGX_PKG_VER} (upstream ${NGINX_SRC_VERSION}) on ${DISTRO}"
  # dpkg-source (format 3.0 quilt) applies the distro patch series during extract.
  ( cd "${NGX_BUILD_DIR}" && { apt-get source "nginx=${NGX_PKG_VER}" || apt-get source nginx; } )
  if [ ! -d "${NGX_SRC}/src" ]; then
    # Fall back to whatever nginx-* tree dpkg-source actually unpacked.
    NGX_SRC="$(find "${NGX_BUILD_DIR}" -maxdepth 1 -type d -name 'nginx-*' 2>/dev/null | sort | head -1)"
  fi
  [ -d "${NGX_SRC}/src" ] || { echo "ERROR: apt-get source did not unpack an nginx source tree under ${NGX_BUILD_DIR}" >&2; exit 1; }
  echo "    distro nginx source: ${NGX_SRC} (apt-verified .dsc; NGINX_VERSION $(awk -F'"' '/define NGINX_VERSION/{print $2}' "${NGX_SRC}/src/core/nginx.h"))"
else
  NGX_TARBALL="${NGX_BUILD_DIR}/nginx-${NGINX_SRC_VERSION}.tar.gz"
  wget -qO "${NGX_TARBALL}" "https://nginx.org/download/nginx-${NGINX_SRC_VERSION}.tar.gz"
  # Supply-chain pin: ALWAYS verify the downloaded tarball against the
  # resolved sha256 (per-distro default or explicit NGINX_SRC_SHA256). Hard-fail
  # on mismatch — a tampered/corrupt download must never reach the build.
  echo "==> verifying nginx tarball sha256 (${NGINX_SRC_SHA256})"
  echo "${NGINX_SRC_SHA256}  ${NGX_TARBALL}" | sha256sum -c - \
    || { echo "ERROR: nginx tarball sha256 mismatch (got $(sha256sum "${NGX_TARBALL}" | awk '{print $1}'), expected ${NGINX_SRC_SHA256})" >&2; exit 1; }
  tar -xzf "${NGX_TARBALL}" -C "${NGX_BUILD_DIR}"
fi
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
elif [ "${DISTRO}" = "el10" ]; then
  # el10 install_deps_el10 installs gcc-toolset-15 (not base gcc as cc); prepend
  # the toolset bin so nginx ./configure finds a C compiler.
  NGX_PRECFG_PATH="${GCC15_PREFIX:-/opt/rh/gcc-toolset-15/root/usr}/bin:${PATH}"
fi
(
  cd "${NGX_SRC}"
  PATH="${NGX_PRECFG_PATH}" ./configure \
    --with-compat \
    "${NGX_HTTP_MODULES[@]}" \
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
  # SYMLINKING gcc-toolset-13's libstdc++ into the STANDARD Linux multiarch paths
  # that config hardcodes (/usr/include/c++/13 + /usr/lib/gcc/<triplet>/13).
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
  # ARCH: derive both triplets so el9-arm64 wires the aarch64 paths. deb_triplet()
  # falls back to uname -m on el9 (no dpkg) -> x86_64-linux-gnu | aarch64-linux-gnu;
  # gcc-toolset-13 names its target subdir <arch>-redhat-linux.
  EL9_DEB_TRIPLET="$(deb_triplet)"
  EL9_RH_TRIPLET="$(rpm --eval '%{_arch}')-redhat-linux"
  mkdir -p /usr/include/c++ "/usr/lib/gcc/${EL9_DEB_TRIPLET}" "/usr/include/${EL9_DEB_TRIPLET}/c++"
  ln -sfn "${GCC13_PREFIX}/include/c++/13" /usr/include/c++/13
  # Symlink the WHOLE toolset gcc/13 dir to the standard path; this exposes
  # libstdc++.a/.so + the c++config target headers in one shot. (Do NOT also
  # symlink individual libs INTO this dir afterwards — they'd resolve through the
  # dir symlink back onto themselves and `ln` would abort with "same file" under
  # set -e.)
  ln -sfn "${GCC13_PREFIX}/lib/gcc/${EL9_RH_TRIPLET}/13" "/usr/lib/gcc/${EL9_DEB_TRIPLET}/13"
  # Cyclone's per_file_copt (clang-libstdcxx13) uses -nostdinc++ and hardcodes the
  # multiarch target-config dir -I/usr/include/<triplet>/c++/13 (where
  # <bits/c++config.h> lives). gcc-toolset-13 names its target subdir
  # <arch>-redhat-linux INSIDE c++/13, so point the multiarch path at it; without
  # this cyclone fails "'bits/c++config.h' file not found".
  ln -sfn "${GCC13_PREFIX}/include/c++/13/${EL9_RH_TRIPLET}" "/usr/include/${EL9_DEB_TRIPLET}/c++/13"
  # clang-21's default layering_check/header-modules trip on abseil's deps under
  # libstdc++ ("module ... does not depend on a module exporting <cstddef>"); these
  # are header-hygiene lints, not codegen — disable them.
  BAZEL_FLAGS+=(
    "--config=clang-libstdcxx13"
    "--features=-layering_check"
    "--features=-parse_headers"
    "--features=-use_header_modules"
  )
elif [ "${DISTRO}" = "el10" ]; then
  # el10: a /15 clone of the el9 arm. el10 ships gcc-toolset-15 (base
  # gcc is 14.x), so the -13 paths (/usr/include/c++/13, /usr/lib/gcc/.../13) do
  # NOT exist — el10 MUST NOT fall through to the el9 -13 arm or the `else`/-13
  # default (that would fail "bits/c++config.h not found" at cyclone's
  # per_file_copt). It gets its OWN gcc-toolset-15 symlink dance +
  # --config=clang-libstdcxx15. The three symlink targets + the bazelrc
  # clang-libstdcxx15 config exactly match the proven
  # build_apache_el10_in_container.sh dance (its lines 103-107).
  GCC15_PREFIX="${GCC15_PREFIX:-/opt/rh/gcc-toolset-15/root/usr}"
  if [ ! -d "${GCC15_PREFIX}" ]; then
    echo "ERROR: gcc-toolset-15 not found at ${GCC15_PREFIX}" >&2
    exit 1
  fi
  # Process-global PATH export (NOT a local NGX_PRECFG_PATH): step-6 `make
  # modules` runs in a subshell that does not re-export PATH, so toolset-15 must
  # be on PATH globally or make picks base gcc-14 (wrong toolchain / ABI drift).
  export PATH="${GCC15_PREFIX}/bin:${PATH}"
  mkdir -p /usr/include/c++ /usr/lib/gcc/x86_64-linux-gnu /usr/include/x86_64-linux-gnu/c++
  ln -sfn "${GCC15_PREFIX}/include/c++/15" /usr/include/c++/15
  ln -sfn "${GCC15_PREFIX}/lib/gcc/x86_64-redhat-linux/15" /usr/lib/gcc/x86_64-linux-gnu/15
  ln -sfn "${GCC15_PREFIX}/include/c++/15/x86_64-redhat-linux" /usr/include/x86_64-linux-gnu/c++/15
  BAZEL_FLAGS+=(
    "--config=clang-libstdcxx15"
    "--features=-layering_check"
    "--features=-parse_headers"
    "--features=-use_header_modules"
  )
  # KNOWN INTRODUCTION-PHASE GAP (the design record D8): like el9 (gt-13), el10
  # builds the module against gt-15's libstdc++ but DEB_STATIC_STDCXX=0, so the
  # module .so links libstdc++ DYNAMICALLY. The sibling Apache el10 build instead
  # force-statics libstdc++ + hard floor-gates it (build_apache_el10_in_container.sh
  # floor_gate), because gt-15's libstdc++ is newer than el10 stock gcc-14's runtime
  # — a dynamic GLIBCXX NEEDED could exceed what a stock el10 host ships. The el9
  # analog (gt-13 dynamic) loads fine on stock el9, so this is LIKELY fine on el10,
  # but it is NOT statically proven here and there is NO blocking gate: the DETECTION
  # is the (currently soft/continue-on-error) el10 dlopen load-check + the D8 real-
  # host smoke. DO NOT promote the el10 nginx channel to prod, nor harden the el10
  # nginx legs to blocking, until D8 confirms the module dlopens on stock AlmaLinux
  # 10. If D8 shows a GLIBCXX gap, static-link libstdc++ here like the focal/
  # DEB_STATIC_STDCXX path (and add a floor-gate mirroring the Apache build).
elif [ "${DEB_STATIC_STDCXX}" = "1" ]; then
  # the design record P3 Debian/Ubuntu distro-stock targets. GCC-13 libstdc++ already sits
  # at the standard paths --config=clang-libstdcxx13 hardcodes (native on
  # trixie/jammy, sideloaded from the trixie .deb on bullseye/bookworm). The
  # modern apt.llvm.org clang (18) enforces the same layering_check/header-modules
  # lints el9's clang-21 trips on under abseil+libstdc++, so disable them too.
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

# Optional CPU cap (release.yml fan-out): BAZEL_CPUS limits Bazel's local CPU
# resources + concurrent actions so peak RSS stays ~1.5 GB x BAZEL_CPUS, letting
# several legs share one box. Unset = use all host cores (prior behavior).
if [ -n "${BAZEL_CPUS:-}" ]; then
  # --jobs caps concurrent actions (the hard RSS limiter); --local_resources=cpu
  # is the non-deprecated successor to --local_cpu_resources (which warns on
  # current Bazel). Both bound the build to BAZEL_CPUS parallel compiles.
  BAZEL_FLAGS+=( "--local_resources=cpu=${BAZEL_CPUS}" "--jobs=${BAZEL_CPUS}" )
fi

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

# Append EVERY static-lib (.a) CppLink input via ar's MRI script mode
# (ADDLIB): it copies archive members verbatim, so member names never collide
# with each other or with the loose objects, and duplicate names INSIDE one
# archive survive too — libavif.a legitimately carries two scale.c.o members,
# which any ar-x extraction strategy silently clobbers (the linker itself
# resolves members by symbol index, not name, so duplicates are fine).
# Generic on purpose: a hardcoded lib list silently drops any NEW archive that
# joins the link — r20's foreign-cc image codecs (libaom.a + libavif.a) fell
# through exactly that way, and GATE 1 caught the packaged module failing
# dlopen with "undefined symbol: avifDecoderCreate". The REQUIRED set asserts
# the known members never vanish from the link either (a lib disappearing from
# CppLink inputs is a build regression, not a reason to ship a thinner
# archive).
REQUIRED_LIBS="libcurl.a libhashkit.a libmemcached.a libmemcachedutil.a libaom.a libavif.a"
grep -E '\.a$' "${WORK}/all_inputs.txt" | sort -u > "${WORK}/psol_libs.txt" || true
for req in ${REQUIRED_LIBS}; do
  grep -qE "/${req}$" "${WORK}/psol_libs.txt" \
    || { echo "ERROR: required static lib ${req} not found in CppLink inputs" >&2; exit 1; }
done
NLIBS="$(wc -l < "${WORK}/psol_libs.txt" | tr -d ' ')"
echo "==> appending ${NLIBS} static libs from CppLink inputs"
# Validate every lib first (so a missing path fails before any MRI mutation),
# then merge in a single MRI run.
: > "${WORK}/merge.mri"
echo "OPEN ${MERGED}" >> "${WORK}/merge.mri"
LIB_MEMBERS=0
while IFS= read -r libpath; do
  libname="$(basename "${libpath}")"
  abslib="${EXECROOT}/${libpath}"
  [ -f "${abslib}" ] || abslib="${libpath}"  # absolute fallback
  [ -f "${abslib}" ] || { echo "ERROR: static lib not found: ${libpath}" >&2; exit 1; }
  NMEM="$(ar t "${abslib}" | wc -l | tr -d ' ')"
  NDUP="$(ar t "${abslib}" | sort | uniq -d | wc -l | tr -d ' ')"
  LIB_MEMBERS=$((LIB_MEMBERS + NMEM))
  echo "    + ${libname} (${NMEM} members, ${NDUP} duplicate member name(s))"
  echo "ADDLIB ${abslib}" >> "${WORK}/merge.mri"
done < "${WORK}/psol_libs.txt"
printf 'SAVE\nEND\n' >> "${WORK}/merge.mri"
ar -M < "${WORK}/merge.mri" \
  || { echo "ERROR: ar MRI merge of static libs failed" >&2; exit 1; }
ar s "${MERGED}"
# Lossless-merge assertion: every loose object AND every lib member must be in
# the final archive — a count short of the sum means something was dropped.
EXPECTED=$((NOBJS + LIB_MEMBERS))
ACTUAL="$(ar t "${MERGED}" | wc -l | tr -d ' ')"
[ "${ACTUAL}" -eq "${EXPECTED}" ] || {
  echo "ERROR: merged archive has ${ACTUAL} members, expected ${EXPECTED} (${NOBJS} objects + ${LIB_MEMBERS} lib members)" >&2
  exit 1
}
echo "    merged archive members: ${ACTUAL} (lossless)"
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
CC_OPT="${CC_OPT} -I${SRCDIR} -I${SRCDIR}/third_party/css_parser/src"
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
  CONFIGURE_ARGS=(
    --add-dynamic-module="${SRCDIR}/pagespeed/nginx"
    --with-compat
    "${NGX_HTTP_MODULES[@]}"
    --with-threads
    --with-cc-opt="${CC_OPT}"
  )
  if [ "${DISTRO}" = "focal" ]; then
    # Link the module with gcc-13 (matches the bazel objects' libstdc++-13 ABI;
    # focal's default cc=gcc-9 would pull libstdc++-9 and mis-resolve the C++23
    # std symbols) and static-link the C++ runtime so the produced .so carries NO
    # libstdc++.so.6 / libgcc_s.so.1 NEEDED — it then loads on hosts with an older
    # libstdc++ (Debian 11 etc.). The glibc floor is set by focal's glibc 2.31.
    export CC=gcc-13
    CONFIGURE_ARGS+=( --with-ld-opt="-static-libstdc++ -static-libgcc" )
  elif [ "${DEB_STATIC_STDCXX}" = "1" ]; then
    # the design record P3 distro-stock targets. We do NOT override CC here: nginx's
    # configure pollutes the global CFLAGS with the module config's `-std=c++17`
    # and then runs C probes (int-size etc.) with it. The stock gcc CC (from
    # build-essential) merely WARNS on -std=c++17 for a .c probe (like focal's
    # gcc-13 and noble's gcc do), whereas clang as CC hard-ERRORS ("can not detect
    # int size"). So the C probes + the glue (ngx_pagespeed.cc, a .cc -> compiled
    # as C++17) use stock gcc; that is link-compatible with the clang-built archive
    # (same Itanium C++ ABI + the static libstdc++-13 satisfies both). The static
    # C++ runtime + clang++ link compiler are injected ONLY on the module .so link
    # rule via the post-configure sed below (NOT via --with-ld-opt, whose configure
    # probe links a C test and would reject the C++-only -static-libstdc++).
    :
  fi
  ./configure "${CONFIGURE_ARGS[@]}"
  if [ "${DISTRO}" = "focal" ]; then
    # The engine's module config (pagespeed/nginx/config) appends an explicit
    # `-lstdc++` to the module link, which forces a DYNAMIC libstdc++.so.6
    # (GLIBCXX_3.4.32) dependency and DEFEATS --with-ld-opt -static-libstdc++ (the
    # gcc driver honors -static-libstdc++ only for its IMPLICIT libstdc++, never an
    # explicit -l). Re-point ONLY the module .so link rule at g++-13 (which links
    # libstdc++ implicitly) and drop the explicit -lstdc++, so -static-libstdc++ /
    # -static-libgcc statically link the PIC libstdc++/libgcc -> the produced .so
    # carries NO libstdc++/libgcc_s NEEDED and no GLIBCXX_* version requirement.
    # The nginx C binary link is untouched (it has no -lstdc++). (Done post-configure
    # so the shared engine config stays intact for the distro noble/el9 builds.)
    sed -i \
      -e 's#[$][(]LINK[)] -o objs/ngx_pagespeed[.]so#g++-13 -o objs/ngx_pagespeed.so#' \
      -e 's# -lstdc++##g' \
      objs/Makefile
  elif [ "${DEB_STATIC_STDCXX}" = "1" ]; then
    # Link the module .so with clang++ (matches the clang-compiled object graph;
    # the only C++23 driver available on the old-glibc targets). We must STATIC-
    # link the C++ runtime so the .so carries NO libstdc++.so.6/GLIBCXX_* NEEDED.
    #
    # The naive `-static-libstdc++` does NOT work here: this is a `-shared` link,
    # where the linker is happy to LEAVE C++ runtime symbols undefined (resolved
    # at load by the host's libstdc++) rather than pull them from the implicit
    # libstdc++.a. The result is a .so with undefined _ZSt28__throw_bad_array_
    # new_lengthv / basic_string::_M_replace_cold etc. that fails dlopen() in
    # stock nginx (whose system libstdc++.so.6 is OLDER and lacks them). Instead
    # we (a) link with clang++, (b) DROP the engine config's dynamic `-lstdc++`,
    # and (c) inject the EXPLICIT static libstdc++.a + libsupc++.a (wrapped in a
    # --start-group with -Bstatic so the linker re-scans and PULLS every needed
    # member, even for a -shared output) plus -static-libgcc. The version script
    # then hides all those pulled symbols -> the .so is self-contained, exports
    # only ngx_module*, and its floor is pure glibc (the target distro's).
    # C++-runtime link strategy (verified empirically, this is the crux of P3):
    #  - `-nostdlib++` suppresses clang's IMPLICIT libstdc++ entirely. Without it,
    #    clang appends its own `-lstdc++`, which (a) on a -shared link stays DYNAMIC
    #    -> a libstdc++.so.6 NEEDED + GLIBCXX_* floor, and (b) is auto-detected from
    #    the NEWEST FULL gcc install = stock g++-10/12 (the WRONG, older libstdc++).
    #  - We then provide the C++ runtime EXPLICITLY as the GCC-13 static archives
    #    wrapped in -Bstatic --start-group libstdc++.a libsupc++.a --end-group
    #    -Bdynamic. The group FORCE-PULLS every member the -shared link would
    #    otherwise leave undefined (e.g. __throw_bad_array_new_length in functexcept.o
    #    and basic_string::_M_replace_cold), which is exactly what makes the .so
    #    dlopen-clean on stock nginx whose system libstdc++.so.6 is older.
    #  - GCCDIR is pinned to the GCC-13 dir (native on trixie/jammy, sideloaded on
    #    bullseye/bookworm) — NOT clang's -print-file-name, which returns the stock
    #    g++-10/12 path (the sideloaded gcc-13 is a libstdc++-only tree with no
    #    crt*.o, so clang does not treat it as a gcc toolchain and ignores it).
    # The engine config's dynamic `-lstdc++` is replaced by this group in-place.
    GCCDIR="/usr/lib/gcc/$(deb_triplet)/13"
    [ -f "${GCCDIR}/libstdc++.a" ] || { echo "ERROR: GCC-13 static libstdc++.a not found at ${GCCDIR}" >&2; exit 1; }
    sed -i \
      -e "s#[\$][(]LINK[)] -o objs/ngx_pagespeed[.]so#clang++ -nostdlib++ -static-libgcc -o objs/ngx_pagespeed.so#" \
      -e "s# -lstdc++# -Wl,-Bstatic,--start-group,${GCCDIR}/libstdc++.a,${GCCDIR}/libsupc++.a,--end-group,-Bdynamic#g" \
      objs/Makefile
  fi
  make -j"${MAKE_JOBS:-${BAZEL_CPUS:-$(nproc)}}" modules
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

# ---------------------------------------------------------------------------
# 7b. glibc-floor check. For the Debian/Ubuntu
#     distro-stock targets, assert (a) NO libstdc++.so.6 / libgcc_s NEEDED (the
#     static link took) and (b) the highest GLIBC_x.y symbol the .so references
#     is <= the target distro's glibc floor, so it loads on a STOCK host. Builds
#     INSIDE the target container already cap glibc at the target's version; this
#     verifies the static libstdc++ archive didn't sneak a newer GLIBC ref in.
# ---------------------------------------------------------------------------
if [ "${DEB_STATIC_STDCXX}" = "1" ]; then
  case "${DISTRO}" in
    bullseye) GLIBC_FLOOR="2.31" ;;
    jammy)    GLIBC_FLOOR="2.35" ;;
    bookworm) GLIBC_FLOOR="2.36" ;;
    trixie)   GLIBC_FLOOR="2.41" ;;
    *)        GLIBC_FLOOR="" ;;
  esac
  echo "=== the design record P3 glibc-floor check (${DISTRO}, floor ${GLIBC_FLOOR}) ==="
  NEEDED_CXX="$(readelf -d "${OUT_SO}" 2>/dev/null | awk '/\(NEEDED\)/{gsub(/[][]/,"",$NF);print $NF}' | grep -iE '^lib(stdc\+\+|gcc_s)' || true)"
  if [ -n "${NEEDED_CXX}" ]; then
    echo "FAIL: module has a C++ runtime NEEDED (static link did not take):" >&2
    echo "${NEEDED_CXX}" | sed 's/^/   /' >&2
    exit 1
  fi
  echo "OK: no libstdc++/libgcc_s in NEEDED (statically linked)."
  MAXGLIBC="$(objdump -T "${OUT_SO}" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' | sed 's/GLIBC_//' | sort -V | tail -1)"
  echo "    max GLIBC symbol ref: ${MAXGLIBC:-<none>} (floor ${GLIBC_FLOOR})"
  if [ -n "${MAXGLIBC}" ] && [ -n "${GLIBC_FLOOR}" ]; then
    HIGHEST="$(printf '%s\n%s\n' "${MAXGLIBC}" "${GLIBC_FLOOR}" | sort -V | tail -1)"
    if [ "${HIGHEST}" != "${GLIBC_FLOOR}" ]; then
      echo "FAIL: max GLIBC ref ${MAXGLIBC} EXCEEDS the ${DISTRO} floor ${GLIBC_FLOOR} — would not load on a stock host." >&2
      exit 1
    fi
    echo "OK: max GLIBC ref ${MAXGLIBC} <= floor ${GLIBC_FLOOR}."
  fi
fi

echo "============================================================"
echo "Built ABI-correct module for ${DISTRO}: ${OUT_SO}"
ls -lh "${OUT_SO}"
echo "============================================================"
