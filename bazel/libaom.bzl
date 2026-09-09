# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# libaom (AV1) build rules for PageSpeed — part of the AVIF codec stack.
#
# Builds libaom from source via cmake through rules_foreign_cc.
# libaom is the AV1 *reference* codec and provides BOTH an encoder and a
# decoder, so an aom-only libavif already does a full encode<->decode
# round-trip. dav1d (see libdav1d.bzl) is a decode-speed *optimization*
# layered on top, not a correctness prerequisite.
#
# BUILD REQUIREMENT: nasm on host for x86_64 SIMD (.asm). This repo's WORKSPACE
# calls rules_foreign_cc_dependencies() with no arguments, and rules_foreign_cc
# 0.11.1 registers no nasm toolchain of its own, so nasm is a host build-tool
# dependency on every leg (CI docker image, darwin, MSVC). Without nasm, aom
# drops to slower C paths (still correct) on any leg that resolves to an x86
# target CPU.

load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

def _simd_assert(arch_macro, cpu):
    """Post-build assertion that aom configured for the SIMD arch we expect.

    Nothing else in the build proves it. out_static_libs only validates a
    filename, and a scalar `generic` build produces a perfectly valid archive
    of the same name. On Windows the arch comes from an AOM_TARGET_CPU cache
    entry that only works because aom guards its own detection with
    `if(NOT AOM_TARGET_CPU)` -- an implementation detail of the pinned commit.
    A future bump that renames or revalidates that variable would drop us back
    to a scalar codec with a green build, nasm present and the CI preflight
    passing. So read the answer out of the header aom actually generated.

    rules_foreign_cc runs this fragment as bash on EVERY platform (the Windows
    toolchain shells out to MSYS bash -- see windows_commands.bzl, "Windows
    uses Bash"), under `set -euo pipefail`, with the cwd already at
    $BUILD_TMPDIR. `$$VAR$$` is the framework's portable variable spelling.

    Args:
      arch_macro: the AOM_ARCH_* macro that must be 1 in the generated config.
      cpu: human-readable arch name, for the failure message.

    Returns:
      A bash fragment for the cmake() rule's postfix_script.
    """
    return """
# Fail loudly rather than shipping a silently scalar AV1 encoder.
aom_cfg="$$BUILD_TMPDIR$$/config/aom_config.h"
if [ ! -f "$aom_cfg" ]; then
  echo "libaom SIMD assertion: expected generated config at $aom_cfg, not found." >&2
  echo "The aom build layout changed; update bazel/libaom.bzl." >&2
  exit 1
fi
if ! grep -Eq '^#define {macro}[[:space:]]+1$' "$aom_cfg"; then
  echo "libaom SIMD assertion FAILED: {macro} is not 1 in $aom_cfg." >&2
  echo "aom configured a scalar codec instead of {cpu}; AVIF encode would be" >&2
  echo "~2.6-2.9x slower with nothing else in the build to say so. Check that" >&2
  echo "AOM_TARGET_CPU still short-circuits detection in the pinned aom's" >&2
  echo "build/cmake/aom_configure.cmake, and that nasm is on PATH." >&2
  echo "--- arch/SIMD lines actually generated ---" >&2
  grep -E '^#define (AOM_ARCH_|HAVE_)' "$aom_cfg" >&2 || true
  exit 1
fi
echo "libaom SIMD assertion: {macro}=1 ({cpu})"
""".format(macro = arch_macro, cpu = cpu)

def libaom_from_source():
    """Creates the cmake build target for libaom (static, encoder+decoder)."""
    cmake(
        name = "aom",
        lib_source = "@aom_src//:all_srcs",
        # MSVC/clang-cl installs the archive as `aom.lib` (no `lib` prefix, per
        # MSVC convention — clang-cl sets MSVC=1 in cmake); every other leg gets
        # `libaom.a`. rules_foreign_cc validates the declared outputs after
        # install, so a wrong name here fails the build with
        # "output ... was not created" even though cmake itself exited 0.
        out_static_libs = select({
            "@platforms//os:windows": ["aom.lib"],
            "//conditions:default": ["libaom.a"],
        }),
        # aom installs the archive + aom.pc (pkgconfig) + the aom/ headers.
        # rules_foreign_cc threads this install dir onto CMAKE_PREFIX_PATH /
        # PKG_CONFIG_PATH of any downstream cmake() that lists :aom in deps,
        # which is how libavif's AVIF_CODEC_AOM=SYSTEM discovers it.
        cache_entries = {
            "BUILD_SHARED_LIBS": "OFF",
            "CONFIG_AV1_ENCODER": "1",
            "CONFIG_AV1_DECODER": "1",
            # Trim everything that is not the codec library itself.
            "ENABLE_TESTS": "0",
            "ENABLE_TESTDATA": "0",
            "ENABLE_EXAMPLES": "0",
            "ENABLE_TOOLS": "0",
            "ENABLE_DOCS": "0",
            "CONFIG_AV1_TEMPORAL_DENOISING": "0",
            # Still-image encode: we do not need the realtime-only encoder.
            # (Leaving CONFIG_REALTIME_ONLY at default keeps the good-quality
            # still-image encoder that AVIF needs.)
            # ENABLE_NASM=1 asks aom to use the external assembler. NOTE: it is
            # NOT a guard that proves asm is wired. aom only searches for an
            # assembler once it has resolved AOM_TARGET_CPU to x86/x86_64; when
            # CPU detection falls through to the `generic` target (no SIMD, no
            # .asm at all) the search never runs, and configure succeeds with
            # ENABLE_NASM=1 and no nasm anywhere on PATH. Verify SIMD from the
            # generated config/aom_config.h (AOM_ARCH_X86_64 / HAVE_SSE2), never
            # from configure exiting 0.
            "ENABLE_NASM": "1",
            # Force lib/ over the RHEL/el9 GNUInstallDirs lib64/ default so
            # rules_foreign_cc finds the archive at out_static_libs (el9 is a
            # packaging target).
            "CMAKE_INSTALL_LIBDIR": "lib",
        } | select({
            # Static CRT ONLY when the rest of the build is using one. Keyed on
            # //bazel:windows_static_crt, not os:windows, because
            # --config=win-asan turns Bazel's static_link_msvcrt feature back
            # off and select() cannot see --features -- see the config_setting
            # comment in bazel/BUILD. Under win-asan this branch drops out and
            # cmake keeps its /MD default, matching ASan's dynamic-CRT model.
            "//bazel:windows_static_crt": {
                # CMAKE_MSVC_RUNTIME_LIBRARY on its own is silently IGNORED
                # here: aom declares
                # cmake_minimum_required(VERSION 3.9), so policy CMP0091 is OLD
                # and the CRT flag keeps coming from CMAKE_<LANG>_FLAGS_<CONFIG>
                # (which default to /MD). Forcing the policy default to NEW
                # activates the abstraction and drops /MD from those flags.
                # Measured on a clang-cl configure probe: with MSVC_RUNTIME_-
                # LIBRARY alone build.ninja carries /MD on 191 compile lines and
                # /MT on 0; adding CMP0091=NEW inverts that to /MT 191, /MD 0.
                "CMAKE_POLICY_DEFAULT_CMP0091": "NEW",
                "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreaded",
            },
            "//conditions:default": {},
        }) | select({
            # Restore x86_64 SIMD. rules_foreign_cc leaves the Windows build
            # looking like a cross-compile with an EMPTY
            # CMAKE_SYSTEM_PROCESSOR, so aom's own detection in
            # build/cmake/aom_configure.cmake matches nothing and falls all the
            # way through to AOM_TARGET_CPU=generic -- a scalar, zero-SIMD
            # codec, silently. Setting AOM_TARGET_CPU directly is the reliable
            # lever: aom guards detection with `if(NOT AOM_TARGET_CPU)`, so a
            # cache entry short-circuits it. Overriding CMAKE_SYSTEM_PROCESSOR
            # would not: the generated toolchain file re-sets it as a normal
            # variable, which shadows the cache.
            #
            # Keyed on windows_x86_64 (os AND cpu) so a future win-arm64 leg
            # gets aom's own detection rather than an x86_64 target CPU.
            # Linux/macOS are left alone: there CMAKE_SYSTEM_PROCESSOR is
            # populated and aom detects the CPU correctly on its own -- the
            # postfix_script assertion below is what proves that stays true.
            #
            # REQUIRES nasm on PATH. Once AOM_TARGET_CPU resolves to x86_64,
            # aom searches for an assembler and hard-fails configure without
            # one, so a runner with no nasm turns a silently-slow build into
            # a build error. aom 3.12.0 also rejects nasm 3.x -- it greps
            # `nasm -hf` for `-Ox` (dropped in nasm 3) and reports
            # "Unsupported nasm: multipass optimization not supported".
            # nasm 2.16.03 is known good. Revert this single entry to fall
            # back to the correct-but-scalar generic build.
            "//bazel:windows_x86_64": {"AOM_TARGET_CPU": "x86_64"},
            "//conditions:default": {},
        }),
        # Assert the arch aom actually configured, on every platform that has
        # SIMD to lose. Keyed on the CPU constraint alone, so the Linux ARM64
        # leg asserts AArch64/NEON rather than being exempted -- an assertion
        # that only guards one leg is the one that rots. Any other CPU (there
        # is none in CI today) gets no assertion rather than a wrong one.
        postfix_script = select({
            "@platforms//cpu:x86_64": _simd_assert("AOM_ARCH_X86_64", "x86_64"),
            "@platforms//cpu:arm64": _simd_assert("AOM_ARCH_AARCH64", "arm64/AArch64"),
            "//conditions:default": "",
        }),
        visibility = ["//visibility:public"],
    )

# Build file content for the aom source tree (git_repository, byte-stable pin).
aom_src_build_file = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
"""
