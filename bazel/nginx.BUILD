# Build file for NGINX headers
#
# This BUILD file exposes NGINX header files as a cc_library for use with
# ngx_pagespeed and other NGINX modules built with Bazel.
#
# NGINX source structure:
#   src/core/      - Core headers (ngx_config.h, ngx_core.h, etc.)
#   src/http/      - HTTP module headers (ngx_http.h, ngx_http_config.h, etc.)
#   src/event/     - Event loop headers (ngx_event.h, ngx_event_connect.h, etc.)
#   src/os/unix/   - Unix platform headers (ngx_process.h, ngx_files.h, etc.)
#   src/os/win32/  - Windows platform headers
#   src/mail/      - Mail module headers (optional)
#   src/stream/    - Stream module headers (optional)
#   objs/          - Auto-generated headers from ./configure (ngx_auto_config.h)
#
# Usage:
#   This repository is typically configured via new_local_repository:
#
#   new_local_repository(
#       name = "nginx",
#       path = "/path/to/nginx-source",
#       build_file = "//bazel:nginx.BUILD",
#   )
#
#   Or for the NGINX source tree to work, you must first run:
#     cd /path/to/nginx-source
#     ./configure [options]
#   This generates objs/ngx_auto_config.h and objs/ngx_auto_headers.h
#
# Note: NGINX modules are typically compiled as dynamic modules (.so) or
# statically linked into the nginx binary. This BUILD file provides headers
# only, suitable for compiling PageSpeed as a dynamic module.

load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # BSD-2-Clause

package(default_visibility = ["//visibility:public"])

# Platform-specific OS headers
# On Unix: src/os/unix/
# On Windows: src/os/win32/
# This is configured via select()

cc_library(
    name = "nginx",
    hdrs = glob([
        # Core NGINX headers
        "src/core/*.h",
        # HTTP module headers
        "src/http/*.h",
        "src/http/modules/*.h",
        "src/http/modules/perl/*.h",
        "src/http/v2/*.h",
        "src/http/v3/*.h",
        # Event system headers
        "src/event/*.h",
        "src/event/modules/*.h",
        # Stream module headers (TCP/UDP proxy)
        "src/stream/*.h",
        # Mail module headers
        "src/mail/*.h",
        # Auto-generated headers from ./configure
        # These must exist (run ./configure first)
        "objs/ngx_auto_config.h",
        "objs/ngx_auto_headers.h",
    ]) + select({
        "@platforms//os:linux": glob(["src/os/unix/*.h"]),
        "@platforms//os:macos": glob(["src/os/unix/*.h"]),
        "@platforms//os:freebsd": glob(["src/os/unix/*.h"]),
        "@platforms//os:windows": glob(["src/os/win32/*.h"]),
        "//conditions:default": glob(["src/os/unix/*.h"]),
    }),
    includes = [
        "src/core",
        "src/http",
        "src/http/modules",
        "src/http/v2",
        "src/event",
        "src/event/modules",
        "src/stream",
        "src/mail",
        "objs",
    ] + select({
        "@platforms//os:linux": ["src/os/unix"],
        "@platforms//os:macos": ["src/os/unix"],
        "@platforms//os:freebsd": ["src/os/unix"],
        "@platforms//os:windows": ["src/os/win32"],
        "//conditions:default": ["src/os/unix"],
    }),
    # NGINX headers require certain defines
    copts = select({
        "@platforms//os:linux": [
            "-D_GNU_SOURCE",
        ],
        "@platforms//os:windows": [
            "-DNGX_WIN32",
        ],
        "//conditions:default": [],
    }),
    # Propagated to all dependents so ngx_event_openssl.h is included
    # before ngx_http_ssl_module.h (which uses ngx_ssl_t).
    defines = ["NGX_OPENSSL=1"],
    # OpenSSL/BoringSSL headers needed when nginx is configured with
    # --with-http_ssl_module (ngx_http_ssl_module.h uses ngx_ssl_t).
    deps = ["@boringssl//:ssl"],
    # Link against pthread on Unix
    linkopts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-lpthread"],
    }),
)

# Minimal headers library for modules that only need core types
# Does not require objs/ generated headers (useful for code analysis)
cc_library(
    name = "nginx_core_headers",
    hdrs = glob([
        "src/core/*.h",
        "src/http/*.h",
        "src/event/*.h",
    ]) + select({
        "@platforms//os:linux": glob(["src/os/unix/*.h"]),
        "@platforms//os:macos": glob(["src/os/unix/*.h"]),
        "@platforms//os:windows": glob(["src/os/win32/*.h"]),
        "//conditions:default": glob(["src/os/unix/*.h"]),
    }),
    includes = [
        "src/core",
        "src/http",
        "src/event",
    ] + select({
        "@platforms//os:linux": ["src/os/unix"],
        "@platforms//os:macos": ["src/os/unix"],
        "@platforms//os:windows": ["src/os/win32"],
        "//conditions:default": ["src/os/unix"],
    }),
    copts = select({
        "@platforms//os:linux": [
            "-D_GNU_SOURCE",
        ],
        "@platforms//os:windows": [
            "-DNGX_WIN32",
        ],
        "//conditions:default": [],
    }),
)
