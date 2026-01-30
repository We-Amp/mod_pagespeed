google_sparsehash_build_rule = """
cc_library(
    name = "google_sparsehash",
    hdrs = [
        "src/google/dense_hash_map",
        "src/google/dense_hash_set",
        "src/google/sparse_hash_map",
        "src/google/sparse_hash_set",
        "src/google/sparsetable",
        "src/google/sparsehash/libc_allocator_with_realloc.h",
        "src/google/sparsehash/densehashtable.h",
        "src/google/sparsehash/sparsehashtable.h",
        "src/google/type_traits.h",
    ],
    # Add cstring include for memset/memcpy - newer GCC requires explicit include
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-include", "cstring"],
    }),
    visibility = ["//visibility:public"],
    strip_include_prefix = "src/",

)
"""
