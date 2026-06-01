load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load("//bazel:version.bzl", "version_header")

licenses(["notice"])  # Apache 2

# Generate Windows resource (.rc) file embedding VS_VERSION_INFO into
# pagespeed_iis.dll. Without this, the DLL ships with empty FileVersion /
# ProductVersion metadata, which (a) makes Get-FileVersionInfo return
# nothing for CI diagnostics and the day-2 baseline probe, and (b)
# causes Windows Installer's REINSTALLMODE=omus default "overwrite-if-
# newer" logic to silently skip pagespeed_iis.dll replacement on upgrade
# whenever the on-disk file has been modified outside MSI (e.g.,
# manual deploy, antivirus quarantine). The .rc encodes FILEVERSION as
# 1.1.0.<beta-number> for betas (strictly increasing) and 1.1.0.65535
# for finals (sorts above all betas). The accompanying REINSTALLMODE=amus
# in install/iis/Product.wxs is belt-and-braces against future scenarios
# where version comparison would still fall back to no-op.
version_header(
    name = "pagespeed_iis_rc_gen",
    template = "pagespeed_iis.rc.in",
    version_file = "//net/instaweb:public/VERSION",
    out = "pagespeed_iis.rc",
)

# Compile the generated .rc to a .res via the Windows SDK resource
# compiler. compile_rc.bat handles rc.exe discovery across runner
# configurations (one host ships SDK 10.0.22621.0, another ships
# 10.0.14393.0 through 10.0.26100.0; rc.exe is not on PATH by default
# on either). cmd_bat (cmd.exe) is used rather than the default bash
# to avoid msys path translation munging the Windows-style paths
# rc.exe expects. Tagged no-remote-cache because the rc.exe output is
# host-toolchain-dependent.
genrule(
    name = "pagespeed_iis_res_gen",
    srcs = [
        ":pagespeed_iis_rc_gen",
        "compile_rc.bat",
    ],
    outs = ["pagespeed_iis.res"],
    cmd_bat = "$(location compile_rc.bat) $(OUTS) $(location :pagespeed_iis_rc_gen)",
    target_compatible_with = ["@platforms//os:windows"],
    tags = ["no-remote-cache"],
)

# =============================================================================
# IIS Native Module for PageSpeed — IISpeed adoption
# =============================================================================
# Build with: bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis

# =============================================================================
# Core IIS module library (IISpeed-based)
# =============================================================================

cc_library(
    name = "iis_module",
    srcs = [
        # IISpeed module sources
        "iis_http_module.cpp",
        "iis_module_factory.cpp",
        "iis_module_request_context.cpp",
        "iis_module_base_fetch.cpp",
        "iis_module_misc.cpp",
        # IISpeed shared sources
        "iis_process_context.cpp",
        "iis_request_context.cpp",
        "iis_psol_request_context.cpp",
        "iis_server_context.cpp",
        "iis_message_handler.cpp",
        "log_message_handler.cpp",
        "iis_rewrite_driver_factory.cpp",
        "iis_rewrite_options.cpp",
        "iis_configuration.cpp",
        "iis_global_constants.cpp",
        "iis_base_fetch.cpp",
        "iis_async_url_fetcher.cpp",
        "asyncwinhttp.cpp",
        "iis_misc.cpp",
        # IISpeed utilities
        "util.cpp",
        "FileCheck.cpp",
    ],
    hdrs = [
        # IISpeed headers
        "iis_http_module.h",
        "iis_module_factory.h",
        "iis_module_request_context.h",
        "iis_module_base_fetch.h",
        "iis_module_misc.h",
        "iis_process_context.h",
        "iis_request_context.h",
        "iis_psol_request_context.h",
        "iis_server_context.h",
        "iis_message_handler.h",
        "log_message_handler.h",
        "iis_rewrite_driver_factory.h",
        "iis_rewrite_options.h",
        "iis_configuration.h",
        "iis_global_constants.h",
        "iis_base_fetch.h",
        "iis_async_url_fetcher.h",
        "asyncwinhttp.h",
        "dechunker.h",
        "iis_misc.h",
        "iis_debugging_string_writer.h",
        "iis_config_util.h",
        "util.h",
        "FileCheck.h",
    ],
    defines = [
        "PAGESPEED_IIS_MODULE",
        "_WINSOCKAPI_",
    ],
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb:net_instaweb_lib",
        "//net/instaweb/http",
        "//net/instaweb/rewriter",
        "//pagespeed/automatic",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/image",
        "//pagespeed/kernel/sharedmem",
        "//pagespeed/kernel/thread:win_thread",
        "//pagespeed/system:system_windows",
        "@com_googlesource_code_re2//:re2",
        "@zlib_ng//:zlib_ng",
    ],
    linkopts = select({
        "@platforms//os:windows": [
            "winhttp.lib",
            "advapi32.lib",
            "shlwapi.lib",
            "ws2_32.lib",
            "bcrypt.lib",
        ],
        "//conditions:default": [],
    }),
)

# IIS module as shared library (DLL)
cc_binary(
    name = "pagespeed_iis.dll",
    srcs = ["dll_main.cc"],
    additional_linker_inputs = [":pagespeed_iis_res_gen"],
    defines = ["PAGESPEED_DLL_EXPORT"],
    linkshared = True,
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [":iis_module"],
    linkopts = select({
        "@platforms//os:windows": [
            "/EXPORT:RegisterModule",
            # lld-link accepts .res files as positional inputs; embeds the
            # VS_VERSION_INFO resource into pagespeed_iis.dll. $(location)
            # resolves to the genrule's .res output.
            "$(location :pagespeed_iis_res_gen)",
        ],
        "//conditions:default": [],
    }),
)

# Performance counter manifest for Windows perfmon.exe integration
filegroup(
    name = "perf_counter_manifest",
    srcs = ["pagespeed_counters.man"],
    visibility = ["//visibility:public"],
)

# Platform-independent config tokenizer (header-only).
# Extracted from iis_configuration.cpp for cross-platform testing.
cc_library(
    name = "iis_config_util",
    hdrs = ["iis_config_util.h"],
    visibility = ["//visibility:public"],
)

# Platform-independent dechunker (header-only).
# Can be tested on any platform.
cc_library(
    name = "iis_dechunker",
    hdrs = ["dechunker.h"],
    visibility = ["//visibility:public"],
)
