# Native Bazel BUILD file for upstream libjpeg-turbo.
#
# Upstream (vs. the Chromium fork) ships only the CMake input templates
# src/jconfig.h.in, src/jconfigint.h.in, src/jversion.h.in. CMake's
# configure_file() expands the @VAR@ placeholders and the #cmakedefine lines
# at configure time based on probes of the target compiler.
#
# We can't run CMake from Bazel, so the genrules below emulate the minimal
# subset of configure_file() that this dependency actually needs for an 8-bit
# build. Where CMake would emit a probed value (e.g. HIDDEN, INLINE,
# THREAD_LOCAL, SIZEOF_SIZE_T, HAVE_BUILTIN_CTZL), we substitute a portable
# preprocessor block — exactly the strategy the Chromium fork uses in its
# static src/jconfig*.h files. The 12-bit and 16-bit cc_libraries share the
# generated jconfig.h and override BITS_IN_JSAMPLE via -D copts.
#
# Substitution table (only the @VARS@ that this build actually needs):
#   @JPEG_LIB_VERSION@             -> 62          (jpeg6b ABI; no WITH_JPEG7/8)
#   @VERSION@                      -> 3.1.4.1
#   @LIBJPEG_TURBO_VERSION_NUMBER@ -> 3001004     (CMake's MAJOR*1e6 + pad3(MINOR) + pad3(REVISION))
#   @CMAKE_PROJECT_NAME@           -> libjpeg-turbo
#   @COPYRIGHT_YEAR@               -> 1991-2026   (jversion.h is included but its strings are unused)
#   @BUILD@                        -> ""          (no datestamp; reproducible build)
#   @HIDDEN@ / @INLINE@ /
#   @THREAD_LOCAL@ / @SIZE_T@      -> per-compiler preprocessor block
# The cmakedefine lines (C_ARITH_CODING_SUPPORTED, D_ARITH_CODING_SUPPORTED,
# WITH_SIMD, RIGHT_SHIFT_IS_UNSIGNED, HAVE_BUILTIN_CTZL, HAVE_INTRIN_H) are
# either commented out (feature not enabled) or replaced with a portable
# preprocessor block (compiler-conditional).

package(default_visibility = ["//visibility:public"])

# ---------------------------------------------------------------------------
# Generate jconfig.h, jconfigint.h, jversion.h from the upstream templates.
# ---------------------------------------------------------------------------
genrule(
    name = "gen_jconfig_h",
    srcs = ["src/jconfig.h.in"],
    outs = ["src/jconfig.h"],
    cmd = """sed -f - $< > $@ <<'__SED__'
s|@JPEG_LIB_VERSION@|62|g
s|@VERSION@|3.1.4.1|g
s|@LIBJPEG_TURBO_VERSION_NUMBER@|3001004|g
s|^#cmakedefine \\(C_ARITH_CODING_SUPPORTED\\) 1$$|/* #undef \\1 */|
s|^#cmakedefine \\(D_ARITH_CODING_SUPPORTED\\) 1$$|/* #undef \\1 */|
s|^#cmakedefine \\(WITH_SIMD\\) 1$$|/* #undef \\1 */|
s|^#cmakedefine \\(RIGHT_SHIFT_IS_UNSIGNED\\) 1$$|/* #undef \\1 */|
__SED__
""",
)

genrule(
    name = "gen_jconfigint_h",
    srcs = ["src/jconfigint.h.in"],
    outs = ["src/jconfigint.h"],
    cmd = """sed -f - $< > $@ <<'__SED__'
s|@VERSION@|3.1.4.1|g
s|@CMAKE_PROJECT_NAME@|libjpeg-turbo|g
s|@BUILD@||g
/^#define HIDDEN  @HIDDEN@$$/c\\
#if defined(__GNUC__)\\
#define HIDDEN  __attribute__((visibility("hidden")))\\
#else\\
#define HIDDEN\\
#endif
/^#define INLINE  @INLINE@$$/c\\
#if defined(__GNUC__)\\
#define INLINE  inline __attribute__((always_inline))\\
#elif defined(_MSC_VER)\\
#define INLINE  __forceinline\\
#else\\
#define INLINE\\
#endif
/^#define THREAD_LOCAL  @THREAD_LOCAL@$$/c\\
#if defined(_MSC_VER) \\&\\& (defined(_WIN32) || defined(_WIN64))\\
#define THREAD_LOCAL  __declspec(thread)\\
#else\\
#define THREAD_LOCAL  __thread\\
#endif
/^#define SIZEOF_SIZE_T  @SIZE_T@$$/c\\
#include <stdint.h>\\
#if __WORDSIZE==64 || defined(_WIN64)\\
#define SIZEOF_SIZE_T  8\\
#else\\
#define SIZEOF_SIZE_T  4\\
#endif
/^#cmakedefine HAVE_BUILTIN_CTZL$$/c\\
#if defined(__GNUC__)\\
#define HAVE_BUILTIN_CTZL\\
#endif
/^#cmakedefine HAVE_INTRIN_H$$/c\\
#if defined(_MSC_VER)\\
#define HAVE_INTRIN_H  1\\
#endif
s|^#cmakedefine \\(C_ARITH_CODING_SUPPORTED\\) 1$$|/* #undef \\1 */|
s|^#cmakedefine \\(D_ARITH_CODING_SUPPORTED\\) 1$$|/* #undef \\1 */|
s|^#cmakedefine \\(WITH_SIMD\\) 1$$|/* #undef \\1 */|
__SED__
""",
)

genrule(
    name = "gen_jversion_h",
    srcs = ["src/jversion.h.in"],
    outs = ["src/jversion.h"],
    cmd = "sed -e 's|@JPEG_LIB_VERSION@|62|g' -e 's|@COPYRIGHT_YEAR@|1991-2026|g' $< > $@",
)

# Note: upstream libjpeg-turbo does NOT ship src/jpeglibmangler.h. That header
# was a Chromium-fork-specific addition that mangled all externally visible
# libjpeg symbols with a chromium_ prefix (via #include in their patched
# jpeglib.h) to avoid collisions with the system libjpeg. mod_pagespeed loads
# as a self-contained Apache/nginx module and the build graph never links
# system libjpeg, so the mangler is not needed.
cc_library(
    name = "libjpeg_headers",
    hdrs = [
        "src/jconfig.h",
        "src/jdct.h",
        "src/jerror.h",
        "src/jinclude.h",
        "src/jmorecfg.h",
        "src/jpeglib.h",
    ],
    includes = ["src"],
)

# 16-bit JPEG support (lossless only).
# Same sources as 8-bit subset, compiled with -DBITS_IN_JSAMPLE=16.
cc_library(
    name = "libjpeg16",
    srcs = [
        "src/jcapistd.c",
        "src/jccolor.c",
        "src/jcdiffct.c",
        "src/jclossls.c",
        "src/jcmainct.c",
        "src/jcprepct.c",
        "src/jcsample.c",
        "src/jdapistd.c",
        "src/jdcolor.c",
        "src/jddiffct.c",
        "src/jdlossls.c",
        "src/jdmainct.c",
        "src/jdpostct.c",
        "src/jdsample.c",
        "src/jutils.c",
    ],
    hdrs = [
        "src/cmyk.h",
        "src/jchuff.h",
        "src/jcmaster.h",
        "src/jconfigint.h",
        "src/jdcoefct.h",
        "src/jdmainct.h",
        "src/jdmaster.h",
        "src/jdmerge.h",
        "src/jdhuff.h",
        "src/jdsample.h",
        "src/jlossls.h",
        "src/jmemsys.h",
        "src/jpeg_nbits.h",
        "src/jpegapicomp.h",
        "src/jpegint.h",
        "src/jsamplecomp.h",
        "src/jsimd.h",
        "src/jsimddct.h",
        "src/jversion.h",
        "src/jccolext.c",
        "src/jdcol565.c",
        "src/jdcolext.c",
        "src/jdmrg565.c",
        "src/jdmrgext.c",
        "src/jstdhuff.c",
    ],
    copts = ["-DBITS_IN_JSAMPLE=16", "-DNO_GETENV", "-DNO_PUTENV"],
    includes = ["src"],
    deps = [":libjpeg_headers"],
)

# 12-bit JPEG support (lossy + lossless).
# Same as 16-bit plus DCT/quantization sources.
cc_library(
    name = "libjpeg12",
    srcs = [
        "src/jcapistd.c",
        "src/jccoefct.c",
        "src/jccolor.c",
        "src/jcdctmgr.c",
        "src/jcdiffct.c",
        "src/jclossls.c",
        "src/jcmainct.c",
        "src/jcprepct.c",
        "src/jcsample.c",
        "src/jdapistd.c",
        "src/jdcoefct.c",
        "src/jdcolor.c",
        "src/jddctmgr.c",
        "src/jddiffct.c",
        "src/jdlossls.c",
        "src/jdmainct.c",
        "src/jdmerge.c",
        "src/jdpostct.c",
        "src/jdsample.c",
        "src/jfdctfst.c",
        "src/jfdctint.c",
        "src/jidctflt.c",
        "src/jidctfst.c",
        "src/jidctint.c",
        "src/jidctred.c",
        "src/jquant1.c",
        "src/jquant2.c",
        "src/jutils.c",
    ],
    hdrs = [
        "src/cmyk.h",
        "src/jchuff.h",
        "src/jcmaster.h",
        "src/jconfigint.h",
        "src/jdcoefct.h",
        "src/jdmainct.h",
        "src/jdmaster.h",
        "src/jdmerge.h",
        "src/jdhuff.h",
        "src/jdsample.h",
        "src/jlossls.h",
        "src/jmemsys.h",
        "src/jpeg_nbits.h",
        "src/jpegapicomp.h",
        "src/jpegint.h",
        "src/jsamplecomp.h",
        "src/jsimd.h",
        "src/jsimddct.h",
        "src/jversion.h",
        "src/jccolext.c",
        "src/jdcol565.c",
        "src/jdcolext.c",
        "src/jdmrg565.c",
        "src/jdmrgext.c",
        "src/jstdhuff.c",
    ],
    copts = ["-DBITS_IN_JSAMPLE=12", "-DNO_GETENV", "-DNO_PUTENV"],
    includes = ["src"],
    deps = [":libjpeg_headers"],
)

cc_library(
    name = "libjpeg",
    srcs = [
        "src/jcapimin.c",
        "src/jcapistd.c",
        "src/jccoefct.c",
        "src/jccolor.c",
        "src/jcdctmgr.c",
        "src/jcdiffct.c",
        "src/jchuff.c",
        "src/jcicc.c",
        "src/jcinit.c",
        "src/jclhuff.c",
        "src/jclossls.c",
        "src/jcmainct.c",
        "src/jcmarker.c",
        "src/jcmaster.c",
        "src/jcomapi.c",
        "src/jcparam.c",
        "src/jcphuff.c",
        "src/jcprepct.c",
        "src/jcsample.c",
        "src/jctrans.c",
        "src/jdapimin.c",
        "src/jdapistd.c",
        "src/jdatadst.c",
        "src/jdatasrc.c",
        "src/jdcoefct.c",
        "src/jdcolor.c",
        "src/jddctmgr.c",
        "src/jddiffct.c",
        "src/jdhuff.c",
        "src/jdicc.c",
        "src/jdinput.c",
        "src/jdlhuff.c",
        "src/jdlossls.c",
        "src/jdmainct.c",
        "src/jdmarker.c",
        "src/jdmaster.c",
        "src/jdmerge.c",
        "src/jdphuff.c",
        "src/jdpostct.c",
        "src/jdsample.c",
        "src/jdtrans.c",
        "src/jerror.c",
        "src/jfdctflt.c",
        "src/jfdctfst.c",
        "src/jfdctint.c",
        "src/jidctflt.c",
        "src/jidctfst.c",
        "src/jidctint.c",
        "src/jidctred.c",
        "src/jmemmgr.c",
        "src/jmemnobs.c",
        "src/jpeg_nbits.c",
        "src/jquant1.c",
        "src/jquant2.c",
        "src/jutils.c",
    ],
    hdrs = [
        "src/cmyk.h",
        "src/jchuff.h",
        "src/jcmaster.h",
        "src/jconfigint.h",
        "src/jdcoefct.h",
        "src/jdmainct.h",
        "src/jdmaster.h",
        "src/jdmerge.h",
        "src/jdhuff.h",
        "src/jdsample.h",
        "src/jlossls.h",
        "src/jmemsys.h",
        "src/jpeg_nbits.h",
        "src/jpegapicomp.h",
        "src/jpegint.h",
        "src/jsamplecomp.h",
        "src/jsimd.h",
        "src/jsimddct.h",
        "src/jversion.h",
        # .c files included by other .c files (not compiled directly)
        "src/jccolext.c",
        "src/jdcol565.c",
        "src/jdcolext.c",
        "src/jdmrg565.c",
        "src/jdmrgext.c",
        "src/jstdhuff.c",
    ],
    copts = ["-DNO_GETENV", "-DNO_PUTENV"],
    includes = ["src"],
    deps = [
        ":libjpeg_headers",
        ":libjpeg12",
        ":libjpeg16",
    ],
)
