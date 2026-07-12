load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load(":hiredis.bzl", "hiredis_build_rule")
load(":jsoncpp.bzl", "jsoncpp_build_rule")
load(":libpng.bzl", "libpng_build_rule")
load(":libwebp.bzl", "libwebp_build_rule")
load(":google_sparsehash.bzl", "google_sparsehash_build_rule")
load(":libpsl.bzl", "libpsl_build_rule")
load(":giflib.bzl", "giflib_build_rule")
load(":optipng.bzl", "optipng_build_rule")
load(":apr.bzl", "apr_build_rule")
load(":aprutil.bzl", "aprutil_build_rule")
load(":cyclone.bzl", "cyclone_build_rule")
load(":ed25519.bzl", "ed25519_build_rule")
load(":zlib_compat.bzl", "zlib_ng_alias_repository")

# the design record / CVE matcher: every vendored C/C++ dep below carries a CPE +
# release_date annotation in tools/dependency/cpe-map.yaml, scanned daily
# against NVD by tools/dependency/cve_scan.py. tools/dependency/validate-deps.py
# fails CI on any dep here that lacks an entry (or an explicit cpe: "N/A" +
# justification) — a new C/C++ dep cannot land unscanned. When you add or bump a
# dep, update its cpe-map.yaml release_date.

ENVOY_COMMIT = "f97695a50e11f5ff6719e129a466bf9204b64a7f"  # v1.37.5 - 2026-06-26 CVE batch (defense-in-depth; mpp's compiled extension set was already unaffected — see tools/dependency/cve-ignore.yaml). ABI-compat pins (gRPC/BoringSSL/zlib-ng below) unchanged v1.37.2->v1.37.5.
ENVOY_SHA = "b517189c09755bcf24a0e04376f4f329df83aad03ed014857a506c74ce9c103f"

# Standalone zlib-ng — replaces @envoy//bazel:zlib for non-Envoy builds.
ZLIB_NG_VERSION = "2.3.2"
ZLIB_NG_SHA = "6a0561b50b8f5f6434a6a9e667a67026f2b2064a1ffa959c6b2dae320161c2a8"

# Standalone BoringSSL — previously only an Envoy transitive dep.
# Bumped May 2026: ~12 months of upstream drift (post-quantum + hardening rolls).
BORINGSSL_VERSION = "0.20260508.0"
BORINGSSL_SHA = "de3371d3fe085afd34778a4c988fb7840b9c92cb21504e674f33ebefd98edc00"

# Phase 4: Standalone gRPC — pinned for ABI compatibility with Envoy v1.37.2.
# Bumped May 2026 from 1.76.0 (~2 minor releases of upstream drift; the 1.78
# series picked up the `<string>` / `<limits>` / `<algorithm>` include cleanups
# we previously carried in bazel/grpc.patch — 3 hunks dropped on re-port).
GRPC_VERSION = "1.78.1"
GRPC_SHA = "961a44a2a5a50670e58f5e887c17fe70529253da23802245326d681f6d8d1ba6"

# Standalone googletest — previously only an Envoy transitive dep.
GOOGLETEST_VERSION = "1.17.0"
GOOGLETEST_SHA = "65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c"

# Standalone fmt + spdlog — previously only Envoy transitive deps.
# Both are header-only libraries used by base/log_shim.
FMT_VERSION = "12.1.0"
FMT_SHA = "695fd197fa5aff8fc67b5f2bbc110490a875cdf7a41686ac8512fb480fa8ada7"
SPDLOG_VERSION = "1.17.0"
SPDLOG_SHA = "d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744"

HIREDIS_COMMIT = "1.3.0"  # Updated Jan 2026 - major version upgrade
HIREDIS_SHA = "25cee4500f359cf5cad3b51ed62059aadfc0939b05150c1f19c7e2829123631c"
JSONCPP_COMMIT = "1.9.6"  # Updated Jan 2026
JSONCPP_SHA = "f93b6dd7ce796b13d02c108bc9f79812245a82e577581c4c9aabe57075c90ea2"
LIBPNG_COMMIT = "1.6.58"  # Updated May 2026 - 4 point releases of parser hardening
LIBPNG_SHA = "a9d4df463d36a6e5f9c29bd6f4967312d17e996c1854f3511f833924eb1993cf"
LIBWEBP_COMMIT = "1.5.0"  # Updated Mar 2026 - CVE-2023-4863 fix (heap buffer overflow)
LIBWEBP_SHA = "668c9aba45565e24c27e17f7aaf7060a399f7f31dba6c97a044e1feacb930f37"
GOOGLE_SPARSEHASH_COMMIT = "6ff8809259d2408cb48ae4fa694e80b15b151af3"
GOOGLE_SPARSEHASH_SHA = "4ae105acb6b53f957b6005fa103a9fd342c39dbc7c87673663e782325b8296b3"
GFLAGS_COMMIT = "de1b8d3daa40b5b07208ec9e82f223d430e2ecc1"  # v2.3.0 - Updated Jan 2026
GFLAGS_SHA = "b563851a60342abc35281fa9c684de7e9604a2bf1982c85035180b9ce3afa774"
# libpsl — maintained Public Suffix List library. Replaces the dead
# Apache-incubator domain_registry_provider ("drp"); same Mozilla PSL dataset,
# built builtin-only with no IDNA runtime. Release dist tarball (ships a
# pre-generated include/libpsl.h; we still generate suffixes_dafsa.h + config.h).
LIBPSL_VERSION = "0.21.5"
LIBPSL_SHA = "1dcc9ceae8b128f3c0b3f654decd0e1e891afc6ff81098f227ef260449dae208"
GIFLIB_COMMIT = "5.2.2"  # Updated Jan 2026
GIFLIB_SHA = "be7ffbd057cadebe2aa144542fd90c6838c6a083b5e8a9048b8ee3b66b29d5fb"
OPTIPNG_COMMIT = "0.7.8"  # Updated Jan 2026 - security fix for GIF decoder buffer overflow (CVE)
OPTIPNG_SHA = "25a3bd68481f21502ccaa0f4c13f84dcf6b20338e4c4e8c51f2cefbd8513398c"
# Upstream libjpeg-turbo tag — May 2026 bump from Chromium-fork v3.1.0 to upstream v3.1.4.1.
# Native Bazel BUILD file at bazel/libjpeg_turbo.BUILD emulates configure_file() for
# src/jconfig.h, src/jconfigint.h, src/jversion.h via sed-based genrules.
LIBJPEG_TURBO_VERSION = "3.1.4.1"
LIBJPEG_TURBO_SHA = "a7da42b640377c2a9a9665e2c4b0ea60cd5599afb48c2521e6df0c9dc9d15a25"
# APR 1.7.x branch head - Updated Feb 2026
APR_COMMIT = "d7a4f5be56969ebb5d2f9d093e17eb39dd016693"
APR_SHA = "5c56af0a8ad7dee32dc381620496f6ebbaa9bf64a470a4ad4fdb0eed84e87fc7"
# APR-util 1.6.x branch head - Updated Feb 2026
APRUTIL_COMMIT = "efbe77e09f0f3e872f2a1126d88d791ab4361b23"
APRUTIL_SHA = "4ce5fead950705f6b33dcac5b7fae45f4295b80cb75a6a1378baaec896fd4fc1"

# Cyclone Cache - high-performance disk cache with scan-resistant CLFUS algorithm
# Requires C++23 - wrapper provides C ABI for C++20 consumers
# Not pinned during development; pinned to a specific commit at release time.

# Cyclone Cache - pinned at release time. This commit carries the
# fork-safe Cache::stop() fix that stops Apache children
# and the nginx master hanging in Cyclone teardown on graceful recycle/reload,
# the small-object tier API: CacheConfig::small_tier_percent,
# tier-routed sync ops, and Cache::small_tier_active(), plus lease-based
# region pinning: borrowed mmap read views are
# protected against circular-buffer wraps for the lease window — the safety
# prerequisite for the zero-copy serving path (CycloneZeroCopy).  This bump
# makes cache teardown join every background thread
# unconditionally: fixes a thread-leak/use-after-free (the Apache
# graceful-restart symptom where a lingering flush thread kept children
# alive -> MPM scoreboard exhaustion) plus a teardown deadlock and an
# in-flight-operation race surfaced by that fix.  This bump adds
# the epoch-checked renew_lease() and force-wrap deadline (ns_until_forced_wrap)
# that the zero-copy serve copy-out path renews against, plus a Volume-lifetime
# use-after-free fix on the renew path.  This bump makes the
# directory insert/remove election verify the full stored key before electing
# update-in-place: previously a different key colliding on the (stripe, bucket,
# 12-bit tag) triple silently destroyed the victim's directory slot on every
# write (deterministic; the victim reads back as a clean NotFound), and adds
# the tag_collision_evictions stat for the full-bucket eviction fallback.
# This bump fixes Volume::open() so non-creators wait for the
# creator to finish initialization: closes a multi-process cache-open race
# where a second opener could observe a half-initialized volume and corrupt
# it or spuriously fail initialization (the lock is kernel-dropped on
# process death, so crash recovery is automatic); it also brings a Cyclone change,
# which removes the dead WriteAggregator and reclaims ~4 MB of committed RSS
# per cache stripe with no API impact.
# This bump makes wrap gating borrow-scoped:
# the per-stripe read lease defers wraps only while read handles are actually
# outstanding (refcounted, released on handle close; ceiling-forced wraps
# reset leaked state), so cache writes no longer starve at capacity under
# steady reads. On-disk format unchanged (v1); adds the borrows_outstanding
# stats gauge (append-only C-struct extension, rebuilt against the vendored
# header by this bump). renew_lease() semantics for open handles unchanged.
CYCLONE_COMMIT = "4e34d7bb634d2310aea52aec22ea09eacd65aaa4"

# Libevent - cross-platform event notification library
# Used by LibeventDispatcher for standalone event loop (Apache deployments)
LIBEVENT_VERSION = "2.1.12-stable"
LIBEVENT_SHA = "92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb"

# libcurl - HTTP client library (built from source)
LIBCURL_VERSION = "8.21.0"
LIBCURL_SHA = "ec753aa6f408a3ca9f0d6d5f7a77417aecd1544db13c03ae5d443612bf367364"

# libmemcached - memcached client library (built from source)
# Using awesomized/libmemcached fork which is actively maintained
LIBMEMCACHED_VERSION = "1.1.4"
LIBMEMCACHED_SHA = "c477e1f6510e1dc698e84f3717ce690a8f65b94c616ecaa62306cce0f5e3116a"

# Build file content for source archives used by rules_foreign_cc
_ALL_SRCS_BUILD_FILE = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
"""

_ABSEIL_VERSION = "20260107.1"
_ABSEIL_SHA = "4314e2a7cbac89cac25a2f2322870f343d81579756ceff7f431803c2c9090195"
_ABSEIL_PATCHES = [
    "//bazel:abseil.patch",
    "//bazel:abseil_nullability.patch",
]

def mod_pagespeed_dependencies():
    # Declare abseil under both names used in the dependency graph:
    # - "com_google_absl": PageSpeed BUILD files, grpc_deps()
    # - "abseil-cpp": gRPC internal refs, protobuf transitive deps
    # Both must exist with identical content to satisfy Bazel's strict
    # include checking on Windows (headers from either external/ dir
    # must be matched by a declared dep).
    for abseil_name in ["com_google_absl", "abseil-cpp"]:
        http_archive(
            name = abseil_name,
            urls = ["https://github.com/abseil/abseil-cpp/archive/%s.tar.gz" % _ABSEIL_VERSION],
            sha256 = _ABSEIL_SHA,
            strip_prefix = "abseil-cpp-%s" % _ABSEIL_VERSION,
            patches = _ABSEIL_PATCHES,
            patch_args = ["-p1"],
        )

    # Phase 0: Standalone zlib-ng
    http_archive(
        name = "zlib_ng",
        strip_prefix = "zlib-ng-%s" % ZLIB_NG_VERSION,
        url = "https://github.com/zlib-ng/zlib-ng/archive/%s.tar.gz" % ZLIB_NG_VERSION,
        sha256 = ZLIB_NG_SHA,
        build_file = "//bazel:zlib_ng.BUILD",
    )

    # Collapse the duplicate deflate: alias @zlib -> @zlib_ng so protobuf's
    # gzip_stream and grpc link the SAME single zlib-ng as libpng/kernel/util,
    # instead of dragging in a second (stock madler) zlib. Declared here, before
    # grpc_deps()/protobuf_deps() in WORKSPACE, so their maybe()-guarded madler
    # @zlib is skipped. Fixes the ODR/UB two-deflate hazard (was the PngOptimizer
    # golden "flake"). See bazel/zlib_compat.bzl.
    zlib_ng_alias_repository(name = "zlib")

    # Phase 1: Standalone BoringSSL
    http_archive(
        name = "boringssl",
        strip_prefix = "boringssl-%s" % BORINGSSL_VERSION,
        url = "https://github.com/google/boringssl/archive/%s.tar.gz" % BORINGSSL_VERSION,
        sha256 = BORINGSSL_SHA,
        # Add CRYPTO_thread_local_cleanup() so pagespeed_iis.dll can release
        # the BoringSSL TLS slot on DLL unload (issue one change).
        patches = ["@mod_pagespeed//bazel:boringssl_dll_unload_tls_cleanup.patch"],
        patch_args = ["-p1"],
    )

    # Phase 2: Standalone libevent.
    # maybe(): WORKSPACE.envoy pre-defines this repo with Envoy's patched
    # snapshot (event2/watch.h) before calling mod_pagespeed_dependencies();
    # the lean WORKSPACE does not, so it gets this vanilla release.
    maybe(
        http_archive,
        name = "com_github_libevent_libevent",
        strip_prefix = "libevent-%s" % LIBEVENT_VERSION,
        url = "https://github.com/libevent/libevent/releases/download/release-%s/libevent-%s.tar.gz" % (LIBEVENT_VERSION, LIBEVENT_VERSION),
        sha256 = LIBEVENT_SHA,
        build_file_content = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
filegroup(
    name = "all",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
""",
    )

    # Phase 4: Standalone gRPC — previously only an Envoy transitive dep.
    # Patch fixes layering_check, missing includes, Apple builds, and
    # third_party/BUILD wiring for our standalone BoringSSL/zlib-ng/c-ares.
    http_archive(
        name = "com_github_grpc_grpc",
        strip_prefix = "grpc-%s" % GRPC_VERSION,
        url = "https://github.com/grpc/grpc/archive/v%s.tar.gz" % GRPC_VERSION,
        sha256 = GRPC_SHA,
        patches = ["//bazel:grpc.patch"],
        patch_args = ["-p1"],
        # Redirect gRPC's internal @abseil-cpp refs to our canonical repo.
        # Without this, gRPC targets add external/abseil-cpp/ to include paths,
        # causing "undeclared inclusion" errors on Windows.
        repo_mapping = {"@abseil-cpp": "@com_google_absl"},
    )

    # Standalone googletest — previously only an Envoy transitive dep.
    http_archive(
        name = "googletest",
        strip_prefix = "googletest-%s" % GOOGLETEST_VERSION,
        url = "https://github.com/google/googletest/releases/download/v%s/googletest-%s.tar.gz" % (GOOGLETEST_VERSION, GOOGLETEST_VERSION),
        sha256 = GOOGLETEST_SHA,
        # Redirect googletest's @abseil-cpp refs (activated by --define absl=1
        # in .bazelrc) to our canonical repo. Without this, googletest adds
        # external/abseil-cpp/ to include paths on Windows.
        repo_mapping = {"@abseil-cpp": "@com_google_absl"},
    )

    # Pre-declare @re2 to prevent grpc_extra_deps()/protobuf from creating
    # a duplicate alongside @com_googlesource_code_re2 (from grpc_deps).
    # Both names must resolve to the same version.
    # Bumped May 2026 from 2022-04-01 (~4 years of upstream drift); parity
    # with MPS 2.0 which already runs this version. The Windows strict-deps
    # fallout from this bump is fixed independently in test/pagespeed/kernel/util/BUILD.
    _RE2_VERSION = "2025-11-05"
    _RE2_SHA = "87f6029d2f6de8aa023654240a03ada90e876ce9a4676e258dd01ea4c26ffd67"
    for re2_name in ["com_googlesource_code_re2", "re2"]:
        http_archive(
            name = re2_name,
            strip_prefix = "re2-%s" % _RE2_VERSION,
            urls = ["https://github.com/google/re2/archive/%s.tar.gz" % _RE2_VERSION],
            sha256 = _RE2_SHA,
        )

    # Standalone fmt — header-only formatting library, dep of spdlog.
    http_archive(
        name = "fmt",
        strip_prefix = "fmt-%s" % FMT_VERSION,
        url = "https://github.com/fmtlib/fmt/releases/download/%s/fmt-%s.zip" % (FMT_VERSION, FMT_VERSION),
        sha256 = FMT_SHA,
        build_file_content = """
cc_library(
    name = "fmt",
    hdrs = glob(["include/fmt/*.h"]),
    includes = ["include"],
    defines = ["FMT_HEADER_ONLY"],
    visibility = ["//visibility:public"],
)
""",
    )

    # Standalone spdlog — header-only logging library, used by base/log_shim.
    http_archive(
        name = "spdlog",
        strip_prefix = "spdlog-%s" % SPDLOG_VERSION,
        url = "https://github.com/gabime/spdlog/archive/v%s.tar.gz" % SPDLOG_VERSION,
        sha256 = SPDLOG_SHA,
        build_file_content = """
cc_library(
    name = "spdlog",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    defines = ["SPDLOG_FMT_EXTERNAL", "SPDLOG_NO_EXCEPTIONS", "FMT_HEADER_ONLY"],
    deps = ["@fmt"],
    visibility = ["//visibility:public"],
)
""",
    )

    http_archive(
        name = "envoy",
        strip_prefix = "envoy-%s" % ENVOY_COMMIT,
        url = "https://github.com/envoyproxy/envoy/archive/%s.tar.gz" % ENVOY_COMMIT,
        sha256 = ENVOY_SHA,
        patches = ["//bazel:envoy_repo_yq_windows.patch"],
        patch_args = ["-p1"],
    )

    http_archive(
        name = "hiredis",
        strip_prefix = "hiredis-%s" % HIREDIS_COMMIT,
        url = "https://github.com/redis/hiredis/archive/v%s.tar.gz" % HIREDIS_COMMIT,
        build_file_content = hiredis_build_rule,
        sha256 = HIREDIS_SHA,
    )

    http_archive(
        name = "jsoncpp",
        strip_prefix = "jsoncpp-%s" % JSONCPP_COMMIT,
        url = "https://github.com/open-source-parsers/jsoncpp/archive/%s.tar.gz" % JSONCPP_COMMIT,
        build_file_content = jsoncpp_build_rule,
        sha256 = JSONCPP_SHA,
    )

    http_archive(
        name = "libpng",
        strip_prefix = "libpng-%s" % LIBPNG_COMMIT,
        url = "https://github.com/glennrp/libpng/archive/v%s.tar.gz" % LIBPNG_COMMIT,
        build_file_content = libpng_build_rule,
        sha256 = LIBPNG_SHA,
    )

    http_archive(
        name = "libwebp",
        urls = [
            "https://github.com/webmproject/libwebp/archive/v%s.tar.gz" % LIBWEBP_COMMIT,
        ],
        sha256 = LIBWEBP_SHA,
        strip_prefix = "libwebp-%s" % LIBWEBP_COMMIT,
        build_file_content = libwebp_build_rule,
    )

    http_archive(
        name = "google_sparsehash",
        strip_prefix = "sparsehash-%s" % GOOGLE_SPARSEHASH_COMMIT,
        url = "https://github.com/sparsehash/sparsehash/archive/%s.tar.gz" % GOOGLE_SPARSEHASH_COMMIT,
        build_file_content = google_sparsehash_build_rule,
        sha256 = GOOGLE_SPARSEHASH_SHA,
        patches = ["@mod_pagespeed//bazel:sparsehash_cstring.patch"],
        patch_args = ["-p1"],
    )

    http_archive(
        name = "com_github_gflags_gflags",
        strip_prefix = "gflags-%s" % GFLAGS_COMMIT,
        url = "https://github.com/gflags/gflags/archive/%s.tar.gz" % GFLAGS_COMMIT,
        sha256 = GFLAGS_SHA,
    )

    http_archive(
        name = "libpsl",
        url = "https://github.com/rockdaboot/libpsl/releases/download/%s/libpsl-%s.tar.gz" % (LIBPSL_VERSION, LIBPSL_VERSION),
        build_file_content = libpsl_build_rule,
        strip_prefix = "libpsl-%s" % LIBPSL_VERSION,
        sha256 = LIBPSL_SHA,
    )

    http_archive(
        name = "giflib",
        strip_prefix = "giflib-%s" % GIFLIB_COMMIT,
        url = "https://sourceforge.net/projects/giflib/files/giflib-5.x/giflib-%s.tar.gz/download" % GIFLIB_COMMIT,
        type = "tar.gz",
        build_file_content = giflib_build_rule,
        sha256 = GIFLIB_SHA,
    )

    http_archive(
        name = "optipng",
        strip_prefix = "optipng-%s" % OPTIPNG_COMMIT,
        url = "https://prdownloads.sourceforge.net/optipng/optipng-%s.tar.gz?download" % OPTIPNG_COMMIT,
        build_file_content = optipng_build_rule,
        sha256 = OPTIPNG_SHA,
    )

    http_archive(
        name = "libjpeg_turbo",
        strip_prefix = "libjpeg-turbo-%s" % LIBJPEG_TURBO_VERSION,
        url = "https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/%s.tar.gz" % LIBJPEG_TURBO_VERSION,
        build_file = "//bazel:libjpeg_turbo.BUILD",
        sha256 = LIBJPEG_TURBO_SHA,
    )

    http_archive(
        name = "apr",
        strip_prefix = "apr-%s" % APR_COMMIT,
        url = "https://github.com/apache/apr/archive/%s.tar.gz" % APR_COMMIT,
        build_file_content = apr_build_rule,
        patches = ["apr.patch"],
        patch_args = ["-p1"],
        sha256 = APR_SHA,
    )

    http_archive(
        name = "aprutil",
        strip_prefix = "apr-util-%s" % APRUTIL_COMMIT,
        url = "https://github.com/apache/apr-util/archive/%s.tar.gz" % APRUTIL_COMMIT,
        build_file_content = aprutil_build_rule,
        sha256 = APRUTIL_SHA,
    )

    # Cyclone Cache - high-performance disk cache
    git_repository(
        name = "cyclone",
        remote = "https://github.com/We-Amp/cyclone-cache.git",
        commit = CYCLONE_COMMIT,
        build_file_content = cyclone_build_rule,
    )

    # libcurl source - built via cmake in //bazel:curl
    http_archive(
        name = "curl_src",
        strip_prefix = "curl-curl-%s" % LIBCURL_VERSION.replace(".", "_"),
        url = "https://github.com/curl/curl/archive/refs/tags/curl-%s.tar.gz" % LIBCURL_VERSION.replace(".", "_"),
        sha256 = LIBCURL_SHA,
        build_file_content = _ALL_SRCS_BUILD_FILE,
        patches = ["@mod_pagespeed//bazel:curl_boringssl_ssl_connect_8_20.patch"],
        patch_args = ["-p1"],
    )

    # libmemcached source - built via cmake in //bazel:libmemcached
    http_archive(
        name = "libmemcached_src",
        strip_prefix = "libmemcached-%s" % LIBMEMCACHED_VERSION,
        url = "https://github.com/awesomized/libmemcached/archive/refs/tags/%s.tar.gz" % LIBMEMCACHED_VERSION,
        sha256 = LIBMEMCACHED_SHA,
        build_file_content = _ALL_SRCS_BUILD_FILE,
    )

    # orlp/ed25519 - compact Ed25519 implementation
    http_archive(
        name = "ed25519",
        strip_prefix = "ed25519-b1f19fab4aebe607805620d25a5e42566ce46a0e",
        url = "https://github.com/orlp/ed25519/archive/b1f19fab4aebe607805620d25a5e42566ce46a0e.tar.gz",
        build_file_content = ed25519_build_rule,
        sha256 = "aedb26c46d3dc3b721ab37c5248d5c923142e4d56009a9605c470383f32ce77a",
    )

