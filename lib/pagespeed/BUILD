# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load("//bazel:test.bzl", "gtest_test")

cc_library(
    name = "pagespeed_api",
    srcs = [
        "pagespeed.cc",
        "pagespeed_internal.h",
    ],
    hdrs = ["pagespeed.h"],
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-fvisibility=default"],
    }) + select({
        # Vendored lib/html headers name unused parameters (canonical idiom);
        # keep the warnings non-fatal in consumer TUs.
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    defines = select({
        "@platforms//os:windows": ["PS_BUILDING_SHARED"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = [
        "//lib/base:message_handler",
        "//lib/base:string_writer",
        "//lib/html:compat",
        "//lib/cache:cache_control_header",
        "//lib/cache:freshness",
        "//lib/cache:html_cache_decision",
        "//lib/cache:html_cache_policy",
        "//lib/cache:html_withheld_marker",
        "//lib/cache:origin_cache_control",
        "//lib/cache:pagespeed_cache",
        "//lib/cache:vary_storability",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:content_type",
        "//lib/classify:hostname",
        "//lib/classify:option_context",
        "//lib/classify:pagespeed_selector",
        "//lib/css:css_import_flattener",
        "//lib/css:css_minify",
        "//lib/html:empty_html_filter",
        "//lib/html:html_keywords",
        "//lib/html:html_parse",
        "//lib/html:html_writer_filter",
        "//src/product_version:build_commit",
        "//src/product_version:version",
        "//src/proto:notification_sender",
        "//src/proto:worker_ipc",
        "//src/worker:async_css_loader",
        "//src/worker:critical_css_extractor",
        "//src/worker:html_css_injector",
        "//src/worker:html_scanner",
        "//src/worker:html_mode",
        "//src/worker:html_transform_filter",
        "//src/worker:serve_stats",
        "//src/worker:shared_config",
        # The bundled SHA-256, for the Markdown entity tag (PS_API 1.13).
        "@cyclone",
    ],
    alwayslink = True,
)

cc_binary(
    name = "libpagespeed.so",
    additional_linker_inputs = select({
        "@platforms//os:macos": [":symbols.exp"],
        "@platforms//os:windows": [":symbols.def"],
        "//conditions:default": [":symbols.lds"],
    }),
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-fvisibility=default"],
    }),
    linkopts = select({
        "@platforms//os:macos": [
            "-Wl,-exported_symbols_list,$(location :symbols.exp)",
        ],
        "@platforms//os:windows": [
            "/DEF:$(location :symbols.def)",
        ],
        # Linux: statically link libc++/libc++abi/libunwind so the resulting
        # libpagespeed.so has no DT_NEEDED for them. Two reasons:
        #   1. NuGet silently drops files whose name has both `++` and a
        #      `.so.N` version suffix (auto-detection regex bug), so shipping
        #      libc++.so.1 as a sibling asset doesn't work — `dotnet run`
        #      ends up with libpagespeed.so but not libc++.so.1.
        #   2. Fewer co-shipped files = simpler customer install, smaller
        #      attack surface, no LD_LIBRARY_PATH dance.
        #
        # `-nostdlib++` is critical: without it, the clang driver re-injects
        # `-lc++` after our user linkopts and the dynamic version wins. The
        # actual static-link order then is: source objects (driver-positioned)
        # → -nostdlib++ (suppresses driver's -lc++) → our explicit Bstatic
        # block → -Bdynamic (restores default link mode for libm/libc).
        #
        # `-Wl,-rpath,$$ORIGIN` is defence-in-depth in case future deps land
        # next to libpagespeed.so. The build base is Ubuntu 22.04 (glibc 2.35)
        # so no `__isoc23_*` symbols leak from libc++.a — see docker/Dockerfile
        # for the GLIBC baseline rationale.
        "//conditions:default": [
            "-Wl,--version-script,$(location :symbols.lds)",
            # Pin DT_SONAME instead of leaving it absent. Without it, a
            # consumer that links against the library records whatever PATH it
            # was handed at link time, which then has to exist verbatim on the
            # target host. With it, the recorded dependency is the plain
            # filename the packages actually install. It stays
            # `libpagespeed.so` for the whole 1.x series and only changes on a
            # MAJOR bump, at which point both files can be installed side by
            # side; see ABI.md.
            "-Wl,-soname,libpagespeed.so",
            "-nostdlib++",
            "-Wl,-Bstatic",
            "-lc++",
            "-lc++abi",
            "-lunwind",
            "-Wl,-Bdynamic",
            "-Wl,-rpath,$$ORIGIN",
        ],
    }),
    linkshared = True,
    linkstatic = True,
    tags = ["manual"],
    visibility = ["//visibility:public"],
    deps = [":pagespeed_api"],
)

gtest_test(
    name = "pagespeed_test",
    size = "large",  # ASan ~10x slowdown pushes past medium's 300s limit
    # Vendored lib/html headers name unused parameters (canonical idiom);
    # keep the warnings non-fatal in consumer TUs.
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    data = ["//test/lib/classify:testdata/accept_markdown_vectors.txt"],
    deps = [
        ":pagespeed_api",
        "//lib/cache:html_withheld_marker",
        "//src/worker:html_mode",
        "//test/lib/cache:daemon_html_policy_cases",
        "//src/worker:serve_stats",
        "//src/worker:shared_config",
        "//test/test_util:temp_dir",
        "@nlohmann_json//:json",
    ],
)
