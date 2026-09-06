# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# libavif build rules for PageSpeed — Stream 0 / Stream A
#
# Builds libavif from source via cmake through rules_foreign_cc, with the AV1
# codecs threaded as its OWN cmake() deps. This is the #1 integration risk:
# under rules_foreign_cc each cmake() runs in an isolated sandbox, so libavif
# cannot see aom/dav1d unless they are passed as deps (which threads
# CMAKE_PREFIX_PATH / PKG_CONFIG_PATH into libavif's configure). A libavif
# built as a flat sibling of the codecs — WITHOUT deps= here — compiles green
# with ZERO codecs and returns AVIF_RESULT_NO_CODEC_AVAILABLE on every
# encode/decode at runtime. The spike's runtime round-trip check exists
# precisely to catch that failure mode, which the link check cannot.
#
# Codec policy:
#   AVIF_CODEC_AOM   = SYSTEM  -> encode + decode via the :aom static lib.
#   AVIF_CODEC_DAV1D = SYSTEM  -> faster decode via :dav1d (optional; aom
#                                 already decodes, so dav1d is layered in only
#                                 once its Meson build is wired — see
#                                 libdav1d.bzl and docs/spikes/avif-stream0/README.md).

load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

def libavif_from_source(with_dav1d = False):
    """cmake build target for libavif, codecs threaded as deps.

    Args:
      with_dav1d: also thread :dav1d for accelerated decode. Left False until
        the dav1d Meson build is wired into Bazel (Stream A follow-up); aom
        alone gives a correct encode+decode round-trip.
    """
    codec_deps = [":aom"]

    # Platform-invariant cache entries. Anything whose VALUE differs per platform
    # (the codec archive paths) lives in the select()s below instead: a select()
    # cannot be a value inside a dict, but `dict | select({...: dict})` is a
    # supported cache_entries expression (same shape //bazel:curl uses).
    cache_entries = {
        "BUILD_SHARED_LIBS": "OFF",
        "AVIF_CODEC_AOM": "SYSTEM",  # encode + decode
        # Codec discovery fix (Stream 0 spike, VERIFIED under Bazel):
        # libavif's Findaom/Finddav1d try pkg-config first -- but
        # rules_foreign_cc 0.11.1 does NOT thread PKG_CONFIG_PATH -- then fall
        # back to find_library/find_path. Pre-seed the result cache vars (see
        # AOM_LIBRARY / AOM_INCLUDE_DIR below) so the search short-circuits with
        # no pkg-config. rules_foreign_cc stages each foreign_cc dep under its
        # OWN named subdir of ${EXT_BUILD_DEPS} (aom/, dav1d/), NOT the flat
        # include/lib a cc_library dep gets -- so the paths are per-dep. Without
        # this, libavif builds codec-less and every encode/decode returns
        # AVIF_RESULT_NO_CODEC_AVAILABLE -- a failure a green build and a clean
        # link both miss, so it must be checked at runtime.
        "AOM_INCLUDE_DIR": "${EXT_BUILD_DEPS}/aom/include",
        "AVIF_LIBYUV": "OFF",  # avoid a second image-scaling dep in the spike
        "AVIF_LIBSHARPYUV": "OFF",
        "AVIF_BUILD_APPS": "OFF",
        "AVIF_BUILD_TESTS": "OFF",
        "AVIF_BUILD_EXAMPLES": "OFF",
        "AVIF_JPEG": "OFF",
        "AVIF_ZLIBPNG": "OFF",
        "CMAKE_INSTALL_LIBDIR": "lib",  # el9 lib64/ -> lib/
    }

    # AOM_LIBRARY must name the archive EXACTLY as :aom installed it, or
    # find_library falls through and libavif silently configures with no codec.
    # MSVC/clang-cl installs aom as `aom.lib`; everywhere else `libaom.a`.
    windows_entries = {
        "AOM_LIBRARY": "${EXT_BUILD_DEPS}/aom/lib/aom.lib",
    }
    default_entries = {
        "AOM_LIBRARY": "${EXT_BUILD_DEPS}/aom/lib/libaom.a",
    }

    # Static CRT ONLY when the rest of the build is using one, consistent with
    # :aom. Keyed on //bazel:windows_static_crt rather than os:windows because
    # --config=win-asan turns Bazel's static_link_msvcrt feature back off and
    # select() cannot see --features (see the config_setting in bazel/BUILD).
    # libavif declares cmake_minimum_required(VERSION 3.22), so CMP0091 is
    # already NEW here and MSVC_RUNTIME_LIBRARY is honoured; the explicit
    # policy default is belt-and-braces and costs nothing.
    static_crt_entries = {
        "CMAKE_POLICY_DEFAULT_CMP0091": "NEW",
        "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreaded",
    }

    if with_dav1d:
        codec_deps = [":aom", ":dav1d"]
        cache_entries["AVIF_CODEC_DAV1D"] = "SYSTEM"
        cache_entries["DAV1D_INCLUDE_DIR"] = "${EXT_BUILD_DEPS}/dav1d/include"

        # NOTE: the Windows dav1d archive name is UNVERIFIED -- the dav1d Meson
        # build is not wired for Windows yet, so this branch is unreachable
        # there. Confirm against a real install before enabling it.
        #
        # A WRONG NAME HERE FAILS SILENTLY, unlike AOM_LIBRARY above. libavif's
        # Finddav1d falls through when find_library misses and simply
        # configures WITHOUT dav1d: cmake exits 0, the archive links, and the
        # only symptom is that decode never gets the faster path. There is no
        # out_static_libs check to catch it either, since dav1d is a dep and
        # not a declared output of this rule. Whoever enables this must verify
        # at runtime (avifCodecName / a decode timing) that dav1d is present,
        # not just that the build went green.
        windows_entries["DAV1D_LIBRARY"] = "${EXT_BUILD_DEPS}/dav1d/lib/dav1d.lib"
        default_entries["DAV1D_LIBRARY"] = "${EXT_BUILD_DEPS}/dav1d/lib/libdav1d.a"

    # Named "avif_lib" so the codec cc_library in
    # pagespeed/kernel/image/BUILD can take the natural ":avif" name
    # (mirroring how :webp wraps @libwebp).
    cmake(
        name = "avif_lib",
        lib_source = "@libavif_src//:all_srcs",
        # Static libavif installs the MERGED archive target `avif_static`, whose
        # OUTPUT_NAME is set to `avif` -- so MSVC/clang-cl yields `avif.lib` and
        # every other leg `libavif.a`.
        out_static_libs = select({
            "@platforms//os:windows": ["avif.lib"],
            "//conditions:default": ["libavif.a"],
        }),
        cache_entries = cache_entries | select({
            "@platforms//os:windows": windows_entries,
            "//conditions:default": default_entries,
        }) | select({
            "//bazel:windows_static_crt": static_crt_entries,
            "//conditions:default": {},
        }),
        deps = codec_deps,
        visibility = ["//visibility:public"],
    )

# The @libavif_src archive's build file lives in bazel/repositories.bzl
# (_LIBAVIF_SRCS_BUILD_FILE) — this module cannot be loaded there (see the
# rules_foreign_cc load-order NOTE at the top of repositories.bzl), so do not
# define build_file_content strings here: they would be dead code that never
# reaches the fetched repo.
