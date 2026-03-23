load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load(":hiredis.bzl", "hiredis_build_rule")
load(":jsoncpp.bzl", "jsoncpp_build_rule")
load(":libpng.bzl", "libpng_build_rule")
load(":libwebp.bzl", "libwebp_build_rule")
load(":google_sparsehash.bzl", "google_sparsehash_build_rule")
load(":drp.bzl", "drp_build_rule")
load(":giflib.bzl", "giflib_build_rule")
load(":optipng.bzl", "optipng_build_rule")
load(":libjpeg_turbo.bzl", "libjpeg_turbo_build_rule")
load(":apr.bzl", "apr_build_rule")
load(":aprutil.bzl", "aprutil_build_rule")
load(":cyclone.bzl", "cyclone_build_rule")
load(":ed25519.bzl", "ed25519_build_rule")

ENVOY_COMMIT = "4be216785e2ddfc5e3d6297a3afa109f7769bb0a"
ENVOY_SHA = "2a24db9331e0ffe4679964d4747655a31155d525b71fc6ca2fcd731d56a65f9b"

# Standalone zlib-ng — replaces @envoy//bazel:zlib for non-Envoy builds.
ZLIB_NG_VERSION = "2.3.2"
ZLIB_NG_SHA = "6a0561b50b8f5f6434a6a9e667a67026f2b2064a1ffa959c6b2dae320161c2a8"

# Standalone BoringSSL — previously only an Envoy transitive dep.
BORINGSSL_VERSION = "0.20250514.0"
BORINGSSL_SHA = "71ef1eb84a035a033ad55867f89a141ddb2e5c5829dd4035ea7803bfff0257ed"

# Phase 4: Standalone gRPC — pinned to exact Envoy version for ABI compatibility.
GRPC_VERSION = "1.76.0"
GRPC_SHA = "0af37b800953130b47c075b56683ee60bdc3eda3c37fc6004193f5b569758204"

# Standalone googletest — previously only an Envoy transitive dep.
GOOGLETEST_VERSION = "1.17.0"
GOOGLETEST_SHA = "65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c"

# Standalone fmt + spdlog — previously only Envoy transitive deps.
# Both are header-only libraries used by base/log_shim.
FMT_VERSION = "12.1.0"
FMT_SHA = "695fd197fa5aff8fc67b5f2bbc110490a875cdf7a41686ac8512fb480fa8ada7"
SPDLOG_VERSION = "1.17.0"
SPDLOG_SHA = "d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744"

BROTLI_COMMIT = "028fb5a23661f123017c060daa546b55cf4bde29"  # v1.2.0 - Updated Jan 2026
BROTLI_SHA = "0afe09a53c8bad9861c8dd1fc1284308d54f19d2979ba3541cfdcc9b05fe360f"
HIREDIS_COMMIT = "1.3.0"  # Updated Jan 2026 - major version upgrade
HIREDIS_SHA = "25cee4500f359cf5cad3b51ed62059aadfc0939b05150c1f19c7e2829123631c"
JSONCPP_COMMIT = "1.9.6"  # Updated Jan 2026
JSONCPP_SHA = "f93b6dd7ce796b13d02c108bc9f79812245a82e577581c4c9aabe57075c90ea2"
LIBPNG_COMMIT = "1.6.54"  # Updated Jan 2026 - security fixes
LIBPNG_SHA = "ba7efce137409079989df4667706c339bebfbb10e9f413474718012a13c8cd4c"
LIBWEBP_COMMIT = "1.5.0"  # Updated Mar 2026 - CVE-2023-4863 fix (heap buffer overflow)
LIBWEBP_SHA = "668c9aba45565e24c27e17f7aaf7060a399f7f31dba6c97a044e1feacb930f37"
GOOGLE_SPARSEHASH_COMMIT = "6ff8809259d2408cb48ae4fa694e80b15b151af3"
GOOGLE_SPARSEHASH_SHA = "4ae105acb6b53f957b6005fa103a9fd342c39dbc7c87673663e782325b8296b3"
GFLAGS_COMMIT = "de1b8d3daa40b5b07208ec9e82f223d430e2ecc1"  # v2.3.0 - Updated Jan 2026
GFLAGS_SHA = "b563851a60342abc35281fa9c684de7e9604a2bf1982c85035180b9ce3afa774"
DRP_COMMIT = "21a7a0f0513b7adad7889ee68edcff49601e4a3a"
DRP_SHA = "9cc8b9a34a73d0e00ff404a4a75a5f386edd9f6d70a9afee5a76a2f41536fab1"
GIFLIB_COMMIT = "5.2.2"  # Updated Jan 2026
GIFLIB_SHA = "be7ffbd057cadebe2aa144542fd90c6838c6a083b5e8a9048b8ee3b66b29d5fb"
OPTIPNG_COMMIT = "0.7.8"  # Updated Jan 2026 - security fix for GIF decoder buffer overflow (CVE)
OPTIPNG_SHA = "25a3bd68481f21502ccaa0f4c13f84dcf6b20338e4c4e8c51f2cefbd8513398c"
LIBJPEG_TURBO_COMMIT = "d1f5f2393e0d51f840207342ae86e55a86443288"  # v3.1.0 - Updated Mar 2026 (was July 2020)
LIBJPEG_TURBO_SHA = ""  # Chromium gitiles archives have non-deterministic checksums
# APR 1.7.x branch head - Updated Feb 2026
APR_COMMIT = "d7a4f5be56969ebb5d2f9d093e17eb39dd016693"
APR_SHA = "5c56af0a8ad7dee32dc381620496f6ebbaa9bf64a470a4ad4fdb0eed84e87fc7"
# APR-util 1.6.x branch head - Updated Feb 2026
APRUTIL_COMMIT = "efbe77e09f0f3e872f2a1126d88d791ab4361b23"
APRUTIL_SHA = "4ce5fead950705f6b33dcac5b7fae45f4295b80cb75a6a1378baaec896fd4fc1"

# Cyclone Cache - high-performance disk cache with scan-resistant CLFUS algorithm
# Requires C++23 - wrapper provides C ABI for C++20 consumers
# Not pinned during development; pinned to a specific commit at release time.

# ModPageSpeed 2.0 - canonical license crypto code (Ed25519 token/verifier/signer)
# Used as a Bazel dependency to share license verification code across products.
# Compiled with -DPAGESPEED_LICENSE_NAMESPACE=net_instaweb to match 1.1's namespace.
MODPAGESPEED2_COMMIT = "bb5d5bb502f7f3aef1adfcf6b355cc2a305fc1a9"

# Libevent - cross-platform event notification library
# Used by LibeventDispatcher for standalone event loop (Apache deployments)
LIBEVENT_VERSION = "2.1.12-stable"
LIBEVENT_SHA = "92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb"

# libcurl - HTTP client library (built from source)
LIBCURL_VERSION = "8.18.0"
LIBCURL_SHA = "be4b1e146ddd84bbb57f081e5c7238eee794e40563976ba2c89a44c433da219c"

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

_ABSEIL_VERSION = "20260107.0"
_ABSEIL_SHA = "4c124408da902be896a2f368042729655709db5e3004ec99f57e3e14439bc1b2"
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

    # Phase 1: Standalone BoringSSL
    http_archive(
        name = "boringssl",
        strip_prefix = "boringssl-%s" % BORINGSSL_VERSION,
        url = "https://github.com/google/boringssl/archive/%s.tar.gz" % BORINGSSL_VERSION,
        sha256 = BORINGSSL_SHA,
    )

    # Phase 2: Standalone libevent
    http_archive(
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
    _RE2_VERSION = "2022-04-01"
    _RE2_SHA = "1ae8ccfdb1066a731bba6ee0881baad5efd2cd661acd9569b689f2586e1a50e9"
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
        name = "brotli",
        strip_prefix = "brotli-%s" % BROTLI_COMMIT,
        url = "https://github.com/google/brotli/archive/%s.tar.gz" % BROTLI_COMMIT,
        sha256 = BROTLI_SHA,
        patch_args = ["-p1"],
        patches = ["@mod_pagespeed//external:brotli.patch"],
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
        name = "drp",
        url = "https://github.com/apache/incubator-pagespeed-drp/archive/%s.tar.gz" % DRP_COMMIT,
        build_file_content = drp_build_rule,
        strip_prefix = "incubator-pagespeed-drp-%s" % DRP_COMMIT,
        sha256 = DRP_SHA,
        patches = ["@mod_pagespeed//bazel:drp_python3.patch"],
        patch_args = ["-p1"],
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
        url = "https://chromium.googlesource.com/chromium/deps/libjpeg_turbo/+archive/%s.tar.gz" % LIBJPEG_TURBO_COMMIT,
        build_file_content = libjpeg_turbo_build_rule,
        # NOTE: sha256 disabled because Chromium's gitiles generates archives dynamically
        # with non-deterministic checksums.
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
        branch = "main",
        build_file_content = cyclone_build_rule,
    )

    # ModPageSpeed 2.0 - canonical license crypto (Ed25519 verification)
    git_repository(
        name = "modpagespeed2",
        remote = "https://github.com/We-Amp/pagespeed-optimizer.git",
        commit = MODPAGESPEED2_COMMIT,
        shallow_since = "2026-03-18",
    )

    # libcurl source - built via cmake in //bazel:curl
    http_archive(
        name = "curl_src",
        strip_prefix = "curl-curl-%s" % LIBCURL_VERSION.replace(".", "_"),
        url = "https://github.com/curl/curl/archive/refs/tags/curl-%s.tar.gz" % LIBCURL_VERSION.replace(".", "_"),
        sha256 = LIBCURL_SHA,
        build_file_content = _ALL_SRCS_BUILD_FILE,
        patches = ["@mod_pagespeed//bazel:curl_boringssl_ssl_connect_8_18.patch"],
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

