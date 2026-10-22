# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Zig cross-compilation toolchain for Windows targets.
#
# This file provides Bazel toolchain configuration for cross-compiling
# from Linux to Windows using Zig as the C/C++ compiler (zig cc).
#
# Zig bundles MinGW headers and CRT, so no Windows SDK is needed for
# the cross-compile validation phase.
#
# Usage:
#   bazel build --config=zig-windows //pagespeed/windows:pagespeed_c_api
#
# Setup:
#   1. Install zig (>= 0.11) on the build host.
#   2. Add hermetic_cc_toolchain to MODULE.bazel (see below).
#
# MODULE.bazel setup (hermetic_cc_toolchain is in the BCR):
#
#   bazel_dep(name = "hermetic_cc_toolchain", version = "4.3.0")
#   zig_toolchains = use_extension("@hermetic_cc_toolchain//toolchain:ext.bzl", "toolchains")
#   use_repo(zig_toolchains, "zig_sdk")
#
# Then in .bazelrc:
#   build:zig-windows --platforms=@zig_sdk//platform:windows_amd64
#   build:zig-windows --extra_toolchains=@zig_sdk//toolchain:windows_amd64
#
# Notes:
#   - The Zig cross-compile produces MinGW ABI binaries, not MSVC ABI.
#   - The C API boundary (pagespeed_c_api.h) isolates this ABI difference
#     so the DLL can still be consumed by MSVC-compiled IIS modules.
#   - For production, use --config=msvc-cl on a native Windows build.

# Placeholder: the toolchain comes from the hermetic_cc_toolchain module once
# it is added to MODULE.bazel. This file exists for documentation.
def zig_toolchain_setup():
    """No-op; see the MODULE.bazel setup instructions in this file."""
    pass
