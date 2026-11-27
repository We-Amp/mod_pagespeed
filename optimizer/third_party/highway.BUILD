# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# Highway - Performance-portable SIMD (google/highway v1.3.0)
# Custom BUILD to add includes = ["."] for downstream repos (e.g. jpegli)
# that use #include <hwy/...> with angle brackets.

cc_library(
    name = "hwy",
    srcs = [
        "hwy/abort.cc",
        "hwy/aligned_allocator.cc",
        "hwy/per_target.cc",
        "hwy/print.cc",
        "hwy/targets.cc",
    ],
    hdrs = [
        "hwy/abort.h",
        "hwy/aligned_allocator.h",
        "hwy/auto_tune.h",
        "hwy/base.h",
        "hwy/bit_set.h",
        "hwy/cache_control.h",
        "hwy/detect_compiler_arch.h",
        "hwy/print.h",
        "hwy/x86_cpuid.h",
    ],
    # Upstream highway SIMD dispatch has unused parameters
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-unused-parameter"],
    }),
    includes = ["."],
    local_defines = ["hwy_EXPORTS"],
    textual_hdrs = [
        "hwy/detect_targets.h",
        "hwy/targets.h",
        "hwy/per_target.cc",
        "hwy/highway.h",
        "hwy/foreach_target.h",
        "hwy/per_target.h",
        "hwy/print-inl.h",
        "hwy/highway_export.h",
        "hwy/ops/arm_neon-inl.h",
        "hwy/ops/arm_sve-inl.h",
        "hwy/ops/emu128-inl.h",
        "hwy/ops/generic_ops-inl.h",
        "hwy/ops/inside-inl.h",
        "hwy/ops/loongarch_lasx-inl.h",
        "hwy/ops/loongarch_lsx-inl.h",
        "hwy/ops/scalar-inl.h",
        "hwy/ops/set_macros-inl.h",
        "hwy/ops/shared-inl.h",
        "hwy/ops/x86_128-inl.h",
        "hwy/ops/x86_256-inl.h",
        "hwy/ops/x86_512-inl.h",
        "hwy/ops/x86_avx3-inl.h",
    ],
    visibility = ["//visibility:public"],
)
