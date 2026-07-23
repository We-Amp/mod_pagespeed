# dav1d (AV1 decoder) build rules for PageSpeed — Stream 0 / Stream A
#
# dav1d is Meson-native upstream (no CMake). This was the flagged #1 build-
# system friction point for the AV1 stack. rules_foreign_cc 0.11.1 DOES export
# a meson() rule (foreign_cc/meson.bzl), so dav1d builds under Bazel with no
# CMake shim — the decision recorded by the Stream 0 spike.
#
# dav1d is a decode-speed OPTIMIZATION: libaom already decodes, so an aom-only
# libavif has a correct encode+decode round-trip. dav1d is threaded into
# libavif (AVIF_CODEC_DAV1D=SYSTEM) only for faster decode.
#
# BUILD REQUIREMENTS on host (register_default_tools = False):
#   - meson  (build system; dav1d is Meson-native)
#   - ninja  (meson's backend)
#   - nasm   (x86_64 SIMD .asm; without it dav1d drops to slower C paths)
# Add all three to every CI build leg (docker image, darwin, MSVC/Windows).
#
# Source: GitHub mirror videolan/dav1d release tag (byte-stable http_archive),
# NOT the code.videolan.org GitLab archive (non-byte-stable auto-tarballs, a
# pinned sha256 there fails verification intermittently — the design record Stream 0).

load("@rules_foreign_cc//foreign_cc:defs.bzl", "meson")

def libdav1d_from_source():
    """Creates the meson build target for dav1d (static decoder)."""
    meson(
        name = "dav1d",
        lib_source = "@dav1d_src//:all_srcs",
        out_static_libs = ["libdav1d.a"],
        options = {
            # Force a flat libdir: meson defaults to the Debian multiarch
            # lib/<triplet>/ on the CI image, so rules_foreign_cc would not
            # find libdav1d.a at out_static_libs (Stream 0 spike finding;
            # the meson analogue of cmake's CMAKE_INSTALL_LIBDIR=lib).
            "libdir": "lib",
            "default_library": "static",
            "enable_tools": "false",
            "enable_tests": "false",
            "enable_examples": "false",
            # 8/10-bit only is enough for AVIF still images; keep default
            # bitdepths to avoid surprising libavif's decoder probe.
        },
        visibility = ["//visibility:public"],
    )

# Build file content for the dav1d source archive (http_archive tarball).
dav1d_src_build_file = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
"""
