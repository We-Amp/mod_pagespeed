# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# libcurl build rules for PageSpeed
#
# Builds libcurl from source using cmake via rules_foreign_cc

load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

def libcurl_from_source():
    """Creates the cmake build target for libcurl."""
    cmake(
        name = "curl",
        lib_source = "@curl_src//:all_srcs",
        out_static_libs = ["libcurl.a"],
        cache_entries = {
            "BUILD_CURL_EXE": "OFF",
            "BUILD_SHARED_LIBS": "OFF",
            "BUILD_TESTING": "OFF",
            "CURL_DISABLE_LDAP": "ON",
            "CURL_DISABLE_LDAPS": "ON",
            "CURL_USE_LIBSSH2": "OFF",
            "CURL_USE_LIBPSL": "OFF",
            "HTTP_ONLY": "ON",
            "ENABLE_UNIX_SOCKETS": "ON",
            # Use system SSL
            "CURL_USE_OPENSSL": "ON",
        },
        visibility = ["//visibility:public"],
    )

# Build file content for the curl source archive
curl_src_build_file = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
"""
