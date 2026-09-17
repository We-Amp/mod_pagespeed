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
#   2. Add hermetic_cc_toolchain to WORKSPACE (see below).
#   3. Register the toolchain in WORKSPACE.
#
# WORKSPACE setup:
#
#   load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
#
#   HERMETIC_CC_TOOLCHAIN_VERSION = "v4.0.0"
#   http_archive(
#       name = "hermetic_cc_toolchain",
#       sha256 = "<sha256>",
#       urls = [
#           "https://github.com/nicholasgasior/hermetic_cc_toolchain/"
#           "releases/download/{v}/hermetic_cc_toolchain-{v}.tar.gz".format(
#               v = HERMETIC_CC_TOOLCHAIN_VERSION),
#       ],
#   )
#
#   load("@hermetic_cc_toolchain//toolchain:defs.bzl", zig_toolchains = "toolchains")
#   zig_toolchains()
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

# Placeholder — actual toolchain registration happens in WORKSPACE via
# hermetic_cc_toolchain.  This file exists for documentation.
def zig_toolchain_setup():
    """No-op; see WORKSPACE setup instructions in this file."""
    pass
