# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_rust//rust:defs.bzl", "rust_static_library", "rust_test")

# Exposed so crate_universe's from_cargo (MODULE.bazel) can read them as labels.
exports_files([
    "Cargo.toml",
    "Cargo.lock",
])

# Rust static library for the VTracer FFI binding.
# Produces a C-compatible static archive that exposes vtracer_convert(),
# vtracer_free(), and vtracer_default_config().
rust_static_library(
    name = "vtracer_ffi_rust",
    srcs = ["src/lib.rs"],
    crate_name = "vtracer_ffi",
    edition = "2021",
    visibility = ["//:internal"],
    deps = [
        "@crates//:vtracer",
    ],
)

# Rust unit tests for the FFI layer.
rust_test(
    name = "vtracer_ffi_test",
    size = "small",
    crate = ":vtracer_ffi_rust",
    edition = "2021",
    tags = ["no_tsan"],
)
