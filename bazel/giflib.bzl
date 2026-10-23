# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

giflib_build_rule = """
cc_library(
    name = "giflib_core",
    srcs = [
        'gifalloc.c',
        'gif_err.c',
        'openbsd-reallocarray.c'
    ],
    hdrs = [
        'gif_lib.h',
        'gif_lib_private.h',
        'gif_hash.h',
    ],
    # Our sources include these headers as "external/giflib/...". Under bzlmod
    # the repo directory carries a canonical name, so serve the headers at the
    # old spelling through _virtual_includes.
    include_prefix = "external/giflib",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "dgiflib",
    srcs = [
        'dgif_lib.c',
    ],
    defines = select({
        "@platforms//os:windows": [
            '_GBA_NO_FILEIO',
        ],
        "//conditions:default": [
            'UINT32=\\"unsigned int\\"',
            '_GBA_NO_FILEIO',
        ],
    }),
    copts = select({
        "@platforms//os:windows": ['/DUINT32=unsigned'],
        "//conditions:default": ['-Wno-pointer-sign'],
    }),
    deps = ['@giflib//:giflib_core',],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "egiflib",
    srcs = [
        'egif_lib.c',
        'gif_hash.c'
    ],
    defines = select({
        "@platforms//os:windows": [
            '_GBA_NO_FILEIO',
            'HAVE_FCNTL_H',
        ],
        "//conditions:default": [
            'UINT32=\\"unsigned int\\"',
            '_GBA_NO_FILEIO',
            'HAVE_FCNTL_H',
        ],
    }),
    copts = select({
        "@platforms//os:windows": ['/DUINT32=unsigned'],
        "//conditions:default": ['-Wno-pointer-sign'],
    }),
    deps = ['@giflib//:giflib_core',],
    visibility = ["//visibility:public"],
)
"""

