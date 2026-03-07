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
load(":closure_compiler.bzl", "closure_library_rules")
load(":cyclone.bzl", "cyclone_build_rule")

ENVOY_COMMIT = "83082b0bf0db8a5b6bb3e691df387bf8566f072d"
ENVOY_SHA = "71a2dc9186d1ef916a952aae8949953dc7ae6bbcce4415bdc2f3ffa0c1ec1427"

BROTLI_COMMIT = "028fb5a23661f123017c060daa546b55cf4bde29"  # v1.2.0 - Updated Jan 2026
BROTLI_SHA = "0afe09a53c8bad9861c8dd1fc1284308d54f19d2979ba3541cfdcc9b05fe360f"
HIREDIS_COMMIT = "1.3.0"  # Updated Jan 2026 - major version upgrade
HIREDIS_SHA = "25cee4500f359cf5cad3b51ed62059aadfc0939b05150c1f19c7e2829123631c"
JSONCPP_COMMIT = "1.9.6"  # Updated Jan 2026
JSONCPP_SHA = "f93b6dd7ce796b13d02c108bc9f79812245a82e577581c4c9aabe57075c90ea2"
LIBPNG_COMMIT = "1.6.54"  # Updated Jan 2026 - security fixes
LIBPNG_SHA = "ba7efce137409079989df4667706c339bebfbb10e9f413474718012a13c8cd4c"
LIBWEBP_COMMIT = "1.2.0"  # Updated Jan 2026 - one major version bump without sharpyuv
LIBWEBP_SHA = "d60608c45682fa1e5d41c3c26c199be5d0184084cd8a971a6fc54035f76487d3"
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
LIBJPEG_TURBO_COMMIT = "ab7cd970a83609f98e8542cea8b81e8d92ddab83"  # July 24th, 2020
LIBJPEG_TURBO_SHA = "3a6b383a957d87b4d60b67e2e1a950c695ee3016e817d04a13af05b9a98c6aea"
# APR 1.7.x branch head - Updated Feb 2026
APR_COMMIT = "d7a4f5be56969ebb5d2f9d093e17eb39dd016693"
APR_SHA = "5c56af0a8ad7dee32dc381620496f6ebbaa9bf64a470a4ad4fdb0eed84e87fc7"
# APR-util 1.6.x branch head - Updated Feb 2026
APRUTIL_COMMIT = "efbe77e09f0f3e872f2a1126d88d791ab4361b23"
APRUTIL_SHA = "4ce5fead950705f6b33dcac5b7fae45f4295b80cb75a6a1378baaec896fd4fc1"

# Cyclone Cache - high-performance disk cache with scan-resistant CLFUS algorithm
# Requires C++23 - wrapper provides C ABI for C++20 consumers
CYCLONE_COMMIT = "cb4c0ea497e546b5fd6c4f8f27d75faebdf5d07c"

# Libevent - cross-platform event notification library
# Used by LibeventDispatcher for standalone event loop (Apache deployments)
LIBEVENT_VERSION = "2.1.12-stable"
LIBEVENT_SHA = "92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb"

# libcurl - HTTP client library (built from source)
LIBCURL_VERSION = "8.11.0"
LIBCURL_SHA = "5a231145114589491fc52da118f9c7ef8abee885d1cb1ced99c7290e9a352f07"

# libmemcached - memcached client library (built from source)
# Using awesomized/libmemcached fork which is actively maintained
LIBMEMCACHED_VERSION = "1.1.4"
LIBMEMCACHED_SHA = "c477e1f6510e1dc698e84f3717ce690a8f65b94c616ecaa62306cce0f5e3116a"

# NOTE: Closure isn't at the latest, because of that introcucing top level comments which
# break tests, but more importantly, make generated js files appear as if they're apache
# licenced. We're at the last revision before that change gets introduced.
# Full context at:
# https://github.com/google/closure-compiler/issues/3551, which got introcuded via
# https://github.com/google/closure-library/commit/1fe1bd873b1b772cca7de983cbaf72ef4011de0b
CLOSURE_LIBRARY_COMMIT = "20191111"  # July 27th, 2020 (latest release was 20200719)

# Build file content for source archives used by rules_foreign_cc
_ALL_SRCS_BUILD_FILE = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
"""

def mod_pagespeed_dependencies():
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
        url = "https://downloads.sourceforge.net/project/giflib/giflib-%s.tar.gz" % GIFLIB_COMMIT,
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
        # with non-deterministic checksums. See TODO(oschaaf) comment above.
        # sha256 = LIBJPEG_TURBO_SHA,
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
        remote = "git@github.com:We-Amp/cyclone-cache.git",
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
    )

    # libmemcached source - built via cmake in //bazel:libmemcached
    http_archive(
        name = "libmemcached_src",
        strip_prefix = "libmemcached-%s" % LIBMEMCACHED_VERSION,
        url = "https://github.com/awesomized/libmemcached/archive/refs/tags/%s.tar.gz" % LIBMEMCACHED_VERSION,
        sha256 = LIBMEMCACHED_SHA,
        build_file_content = _ALL_SRCS_BUILD_FILE,
    )

    http_archive(
        name = "closure_library",
        strip_prefix = "closure-library-%s" % CLOSURE_LIBRARY_COMMIT,
        url = "https://github.com/google/closure-library/archive/v%s.tar.gz" % CLOSURE_LIBRARY_COMMIT,
        sha256 = "21400f56c5b8f9e2548facb30658e4a09fe1cbaba39440735441735d2f900e55",
        build_file_content = closure_library_rules,
    )
