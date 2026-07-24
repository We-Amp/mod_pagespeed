licenses(["notice"])  # Apache 2

package(default_visibility = ["//visibility:public"])

load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

exports_files([
    "gflags_cstring.patch",
    "sparsehash_cstring.patch",
    "googleurl_visibility.patch",
    "googleurl_macos_libcxx.patch",
    "curl_boringssl_ssl_connect_8_20.patch",
    "boringssl_dll_unload_tls_cleanup.patch",
    "libavif_merge_static_libs_clang_cl.patch",
])

# =============================================================================
# Set by --config=envoy-filter (.bazelrc); selects the EXT_BUILD_DEPS
# spelling the Envoy workspace's rules_foreign_cc pin accepts (see the
# cache_entries select below).
config_setting(
    name = "rules_foreign_cc_envoy_pin",
    define_values = {"rules_foreign_cc_pin": "envoy"},
)

# Selects the Linux/arm64 target — used to pick the arm64 pre-generated APR /
# apr-util configure headers (third_party/{apr,aprutil}/gen/arch/linux/arm64)
# over the x64 tree. macOS is matched separately (os:macos) so a native
# Apple-Silicon build keeps its existing mac/x64 tree; this setting requires
# os:linux so it can never collide with the macOS branch.
config_setting(
    name = "linux_arm64",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:arm64",
    ],
)

# =============================================================================
# AVIF codec stack — see bazel/libaom.bzl and bazel/libavif.bzl.
# =============================================================================

# The AV1 codecs are built by cmake through rules_foreign_cc, so they do NOT
# see Bazel's C++ features — a cmake() cache entry is the only way to pick the
# CRT, and select() cannot read --features. .bazelrc therefore mirrors the CRT
# choice into --define=avif_crt: `static` globally (matching the
# static_link_msvcrt feature --config=windows turns on) and `dynamic` under
# --config=win-asan, which turns static_link_msvcrt back off because ASan's
# interceptors hook the DYNAMIC CRT.
#
# Keying the cmake entries on os:windows alone silently mixed the two under
# --config=win-asan: lld-link reports a CRT mix as LNK4098, a *warning*, so the
# leg stayed green while ASan quietly stopped instrumenting allocations inside
# the statically-CRT'd codec — the one thing that leg exists to do.
config_setting(
    name = "windows_static_crt",
    constraint_values = ["@platforms//os:windows"],
    define_values = {"avif_crt": "static"},
)

# libaom's own CPU detection sees an empty CMAKE_SYSTEM_PROCESSOR under
# rules_foreign_cc and falls through to a scalar `generic` target, so
# AOM_TARGET_CPU has to be set explicitly. Keyed on os+cpu rather than os alone
# so a future win-arm64 leg does not inherit an x86_64 target CPU.
config_setting(
    name = "windows_x86_64",
    constraint_values = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
)

# libcurl - HTTP client library (built from source via cmake)
# =============================================================================

cmake(
    name = "curl",
    lib_source = "@curl_src//:all_srcs",
    out_static_libs = select({
        "@platforms//os:windows": ["libcurl.lib"],
        "//conditions:default": ["libcurl.a"],
    }),
    # Windows SChannel TLS and networking require system libraries at link time.
    linkopts = select({
        "@platforms//os:windows": [
            "Crypt32.lib",
            "Secur32.lib",
            "Schannel.lib",
            "Advapi32.lib",
            "ws2_32.lib",      # Winsock
            "Iphlpapi.lib",    # if_nametoindex
        ],
        "//conditions:default": [],
    }),
    deps = select({
        "@platforms//os:macos": [],
        "@platforms//os:windows": [],
        "//conditions:default": [
            "@boringssl//:ssl",
            "@boringssl//:crypto",
        ],
    }),
    cache_entries = {
        "BUILD_CURL_EXE": "OFF",
        "BUILD_SHARED_LIBS": "OFF",
        "BUILD_TESTING": "OFF",
        "CURL_DISABLE_LDAP": "ON",
        "CURL_DISABLE_LDAPS": "ON",
        "CURL_USE_LIBSSH2": "OFF",
        "CURL_USE_LIBPSL": "OFF",
        "CURL_DISABLE_FTP": "ON",
        "CURL_DISABLE_TELNET": "ON",
        "CURL_DISABLE_DICT": "ON",
        "CURL_DISABLE_FILE": "ON",
        "CURL_DISABLE_TFTP": "ON",
        "CURL_DISABLE_IMAP": "ON",
        "CURL_DISABLE_POP3": "ON",
        "CURL_DISABLE_SMTP": "ON",
        "CURL_DISABLE_RTSP": "ON",
        "CURL_DISABLE_GOPHER": "ON",
        "CURL_DISABLE_SMB": "ON",
        "CURL_DISABLE_MQTT": "ON",
        "ENABLE_UNIX_SOCKETS": "ON",
        # Force lib/ over the RHEL/el9 GNUInstallDirs default lib64/ so
        # rules_foreign_cc finds libcurl.a at the out_static_libs path on el9
        # (harmless on Debian/Ubuntu/macOS; the design record el9 packaging build).
        "CMAKE_INSTALL_LIBDIR": "lib",
    } | select({
        "@platforms//os:macos": {
            # Use macOS SecureTransport for SSL
            "CURL_USE_SECTRANSPORT": "ON",
            "CURL_USE_OPENSSL": "OFF",
            "CURL_CA_BUNDLE": "none",
            "CURL_CA_PATH": "none",
        },
        "@platforms//os:windows": {
            # Use Windows Schannel for SSL
            "CURL_USE_SCHANNEL": "ON",
            "CURL_USE_OPENSSL": "OFF",
            # Use static CRT to match Bazel's static_link_msvcrt feature
            "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreaded",
            "CURL_CA_BUNDLE": "none",
            "CURL_CA_PATH": "none",
        },
        "//conditions:default": {
            # Link against BoringSSL (same instance used by Envoy) for HTTPS.
            # BoringSSL libs are staged as libssl_internal.a / libcrypto_internal.a
            # by rules_foreign_cc, so we set explicit paths for FindOpenSSL.
            # ${VAR} syntax for rules_foreign_cc variable substitution.
            "CURL_USE_OPENSSL": "ON",
            "OPENSSL_INCLUDE_DIR": "${EXT_BUILD_DEPS}/include",
            "OPENSSL_SSL_LIBRARY": "${EXT_BUILD_DEPS}/lib/libssl_internal.a",
            "OPENSSL_CRYPTO_LIBRARY": "${EXT_BUILD_DEPS}/lib/libcrypto_internal.a",
            "CURL_CA_BUNDLE": "/etc/ssl/certs/ca-certificates.crt",
            "CURL_CA_PATH": "/etc/ssl/certs",
        },
    }) | select({
        # The two workspaces pin INCOMPATIBLE rules_foreign_cc versions and
        # no single $-spelling for EXT_BUILD_DEPS passes both:
        #   lean 0.11.1:  ${X} ok, $X ok, $$X fails its $$VAR$$ parser,
        #                 $$X$$ ok
        #   Envoy pin:    runs bazel make-var expansion, so ${X}/$X/$(X) all
        #                 fail analysis; only $$X survives (escapes to a
        #                 literal $X the wrapper shell script then expands)
        # The envoy-filter config (the only supported way to build the Envoy
        # workspace, see pagespeed/envoy/CLAUDE.md) sets this define; the
        # union overrides the lean ${X} spellings above with the $$X ones.
        ":rules_foreign_cc_envoy_pin": {
            "OPENSSL_INCLUDE_DIR": "$$EXT_BUILD_DEPS/include",
            "OPENSSL_SSL_LIBRARY": "$$EXT_BUILD_DEPS/lib/libssl_internal.a",
            "OPENSSL_CRYPTO_LIBRARY": "$$EXT_BUILD_DEPS/lib/libcrypto_internal.a",
        },
        "//conditions:default": {},
    }),
    visibility = ["//visibility:public"],
)

# =============================================================================
# libevent - cross-platform event notification library (built from source via cmake)
# =============================================================================
# Used by LibeventDispatcher for standalone event loop (Apache deployments).
# Replaces @envoy//bazel/foreign_cc:event with a standalone build.

cmake(
    name = "libevent",
    lib_source = "@com_github_libevent_libevent//:all_srcs",
    out_static_libs = select({
        "@platforms//os:macos": [
            "libevent.a",
            "libevent_pthreads.a",
        ],
        "//conditions:default": [
            "libevent.a",
            "libevent_pthreads.a",
            "libevent_core.a",
        ],
    }),
    cache_entries = {
        "EVENT__DISABLE_OPENSSL": "ON",
        "EVENT__DISABLE_MBEDTLS": "ON",
        "EVENT__DISABLE_REGRESS": "ON",
        "EVENT__DISABLE_TESTS": "ON",
        "EVENT__DISABLE_SAMPLES": "ON",
        "EVENT__DISABLE_BENCHMARK": "ON",
        "EVENT__LIBRARY_TYPE": "STATIC",
        # _GNU_SOURCE ensures UINT32_MAX/SIZE_MAX/INT64_MAX are defined
        # in <stdint.h> and <limits.h> under all compiler configurations
        # (including sanitizer builds which may affect header inclusion).
        "CMAKE_C_FLAGS": "-D_GNU_SOURCE",
    },
    visibility = ["//visibility:public"],
)

# =============================================================================
# libmemcached - memcached client library (built from source via cmake)
# =============================================================================

cmake(
    name = "libmemcached",
    lib_source = "@libmemcached_src//:all_srcs",
    out_static_libs = select({
        "@platforms//os:windows": [
            "libmemcached.lib",
            "libhashkit.lib",
            "libmemcachedutil.lib",
        ],
        "//conditions:default": [
            "libmemcached.a",
            "libhashkit.a",
            "libmemcachedutil.a",
        ],
    }),
    cache_entries = {
        "BUILD_SHARED_LIBS": "OFF",
        "BUILD_TESTING": "OFF",
        "BUILD_DOCS": "OFF",
        "ENABLE_SASL": "OFF",
        "ENABLE_DTRACE": "OFF",
        "ENABLE_HASH_HSIEH": "ON",
        "ENABLE_HASH_FNV64": "ON",
        "ENABLE_HASH_MURMUR": "ON",
        "ENABLE_MEMASLAP": "OFF",
        # Force lib/ (not the RHEL/el9 GNUInstallDirs default lib64/) so
        # rules_foreign_cc finds the .a at the out_static_libs path on every
        # distro. Harmless on Debian/Ubuntu where lib/ is already the default
        #.
        "CMAKE_INSTALL_LIBDIR": "lib",
    },
    visibility = ["//visibility:public"],
)
