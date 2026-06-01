# libmemcached build rules for PageSpeed
#
# Builds libmemcached from source using cmake via rules_foreign_cc
# Uses the awesomized/libmemcached fork which is actively maintained

load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

def libmemcached_from_source():
    """Creates the cmake build target for libmemcached."""
    cmake(
        name = "libmemcached",
        lib_source = "@libmemcached_src//:all_srcs",
        out_static_libs = [
            "libmemcached.a",
            "libhashkit.a",
            "libmemcachedutil.a",
        ],
        cache_entries = {
            "BUILD_TESTING": "OFF",
            "BUILD_DOCS": "OFF",
            "ENABLE_SASL": "OFF",  # Disable SASL to simplify dependencies
            "ENABLE_DTRACE": "OFF",
            "ENABLE_HASH_HSIEH": "ON",
            "ENABLE_HASH_FNV64": "ON",
            "ENABLE_HASH_MURMUR": "ON",
            "ENABLE_MEMASLAP": "OFF",
        },
        visibility = ["//visibility:public"],
    )

# Build file content for the libmemcached source archive
libmemcached_src_build_file = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
"""
