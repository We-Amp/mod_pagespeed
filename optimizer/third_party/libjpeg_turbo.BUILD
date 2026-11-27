# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# Generate jconfig.h from template
# libjpeg-turbo 2.1.x uses CMake to generate this; we provide a
# minimal configuration for Bazel.
genrule(
    name = "gen_jconfig",
    srcs = ["jconfig.h.in"],
    outs = ["jconfig.h"],
    cmd = """
sed -e 's/@JPEG_LIB_VERSION@/62/' \
    -e 's/@VERSION@/2.1.5.1/' \
    -e 's/@LIBJPEG_TURBO_VERSION_NUMBER@/2001005/' \
    -e 's/@BITS_IN_JSAMPLE@/8/' \
    -e 's/#cmakedefine C_ARITH_CODING_SUPPORTED/#define C_ARITH_CODING_SUPPORTED/' \
    -e 's/#cmakedefine D_ARITH_CODING_SUPPORTED/#define D_ARITH_CODING_SUPPORTED/' \
    -e 's/#cmakedefine MEM_SRCDST_SUPPORTED/#define MEM_SRCDST_SUPPORTED/' \
    -e 's/#cmakedefine WITH_SIMD/\\/\\/ #undef WITH_SIMD/' \
    -e 's/#cmakedefine HAVE_LOCALE_H/#define HAVE_LOCALE_H/' \
    -e 's/#cmakedefine HAVE_STDDEF_H/#define HAVE_STDDEF_H/' \
    -e 's/#cmakedefine HAVE_STDLIB_H/#define HAVE_STDLIB_H/' \
    -e 's/#cmakedefine NEED_SYS_TYPES_H/#define NEED_SYS_TYPES_H/' \
    -e 's/#cmakedefine NEED_BSD_STRINGS/\\/\\/ #undef NEED_BSD_STRINGS/' \
    -e 's/#cmakedefine HAVE_UNSIGNED_CHAR/#define HAVE_UNSIGNED_CHAR/' \
    -e 's/#cmakedefine HAVE_UNSIGNED_SHORT/#define HAVE_UNSIGNED_SHORT/' \
    -e 's/#cmakedefine INCOMPLETE_TYPES_BROKEN/\\/\\/ #undef INCOMPLETE_TYPES_BROKEN/' \
    -e 's/#cmakedefine RIGHT_SHIFT_IS_UNSIGNED/\\/\\/ #undef RIGHT_SHIFT_IS_UNSIGNED/' \
    -e '/#cmakedefine/d' \
    $< > $@
""",
)

# Generate jconfigint.h from template
genrule(
    name = "gen_jconfigint",
    srcs = ["jconfigint.h.in"],
    outs = ["jconfigint.h"],
    cmd = """
sed -e 's/@BUILD@/20240101/' \
    -e 's/@CMAKE_PROJECT_NAME@/libjpeg-turbo/' \
    -e 's/@VERSION@/2.1.5.1/' \
    -e 's/@SIZE_T@/8/' \
    -e 's/@INLINE@/inline/' \
    -e 's/@THREAD_LOCAL@/_Thread_local/' \
    -e 's/#cmakedefine HAVE_BUILTIN_CTZL/#define HAVE_BUILTIN_CTZL/' \
    -e 's/#cmakedefine HAVE_INTRIN_H/\\/\\/ #undef HAVE_INTRIN_H/' \
    -e '/#cmakedefine/d' \
    $< > $@
""",
)

# Generate jversion.h from template
genrule(
    name = "gen_jversion",
    srcs = ["jversion.h.in"],
    outs = ["jversion.h"],
    cmd = """
sed -e 's/@CMAKE_PROJECT_NAME@/libjpeg-turbo/' \
    -e 's/@VERSION@/2.1.5.1/' \
    -e 's/@BLDDIR@/./' \
    $< > $@
""",
)

# Header-only libjpeg-turbo: provides type definitions (jpeglib.h structs,
# jerror.h codes, jmorecfg.h types) but NO implementation .c files.
# Jpegli provides the actual jpeg_* function implementations.
cc_library(
    name = "libjpeg_turbo",
    hdrs = glob(["*.h"]) + [
        ":gen_jconfig",
        ":gen_jconfigint",
        ":gen_jversion",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
)
