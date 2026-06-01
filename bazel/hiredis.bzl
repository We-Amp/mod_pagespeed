hiredis_build_rule = """
cc_library(
    name = "hiredis",
    srcs = [
        "async.c",
        "hiredis.c",
        "net.c",
        "read.c",
        "sds.c",
        "alloc.c",
        "sockcompat.c",
    ],
    hdrs = [
        # adding dict.c here since async.c includes it
        "dict.c",
        "async.h",
        "async_private.h",
        "dict.h",
        "hiredis.h",
        "net.h",
        "read.h",
        "sds.h",
        "alloc.h",
        "sdsalloc.h",
        "fmacros.h",
        "sockcompat.h",
        "win32.h",
    ],
    linkopts = select({
        "@platforms//os:windows": ["-lws2_32"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)
"""
