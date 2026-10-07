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

# Header-only helper for the OnSendResponse "leave" completion path. Kept free
# of IIS/Windows headers (pure template) so the use-after-free invariant it
# encodes is unit-testable on every platform, not just Windows.
cc_library(
    name = "iis_proxy_fetch_completion",
    hdrs = ["iis_proxy_fetch_completion.h"],
    visibility = ["//visibility:public"],
)

# Decision core for the zero-copy aliased serve (CycloneZeroCopyServe):
# chunking, copy-out and abort policy.  Header-only and free of
# IIS/Windows headers (same pattern as iis_proxy_fetch_completion) so the
# safety rules are unit-testable on every platform.
cc_library(
    name = "iis_zerocopy_serve",
    hdrs = ["iis_zerocopy_serve.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# =============================================================================
# IIS Native Module for PageSpeed — built on the adopted IISpeed code base
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
        "iis_config_util.cpp",
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
        ":iis_daemon_check",
        ":iis_daemon_record",
        ":iis_daemon_serve",
        ":iis_global_constants",
        ":iis_ip_string",
        ":iis_proxy_fetch_completion",
        ":iis_zerocopy_serve",
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
        # The shared daemon substrate: the startup check, the record arm and
        # the serve arm with its notifications.
        "//pagespeed/system:daemon_adapter",
        "//pagespeed/system:daemon_record_arm",
        "//pagespeed/system:daemon_serve_arm",
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

# The Windows event-log writer (Event Viewer source "PageSpeed"), with a
# test seam that lets unit tests count entries without writing the real log.
# Windows-only.
cc_library(
    name = "iis_event_log",
    srcs = ["iis_event_log.cc"],
    hdrs = ["iis_event_log.h"],
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# The process-wide daemon startup-check registry: one adapter per distinct
# (library, socket, volume) triple for the life of the process, so a
# configuration rebuild reuses the bound library instead of growing the
# loader's reference count.  Windows-only like its port.
cc_library(
    name = "iis_daemon_check",
    srcs = ["iis_daemon_check.cc"],
    hdrs = ["iis_daemon_check.h"],
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [
        ":iis_event_log",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/system:daemon_adapter",
    ],
)

# The port's response-header name tables (known headers, their internal
# bookmarks, the unknown-header bookmark set) and the module version
# constant.  Split out so tests can iterate the tables without linking the
# port.
cc_library(
    name = "iis_global_constants",
    srcs = ["iis_global_constants.cpp"],
    hdrs = ["iis_global_constants.h"],
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb:net_instaweb_lib",
    ],
)

# The daemon record arm's request facts: the capability-header reader
# shared by both arms, the begin-time request properties, and the
# authorization fact re-taken after IIS authentication.  Windows-only like
# its port.
cc_library(
    name = "iis_daemon_record",
    srcs = ["iis_daemon_record.cc"],
    hdrs = ["iis_daemon_record.h"],
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb/rewriter",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
        "//pagespeed/system:daemon_record_arm",
    ],
)

# The serve arm's own parts: the request facts the peer needs, the response
# composition (pure, so the whole header list is testable without an IIS
# request), and the write itself. Windows-only for the same reason as the
# record arm's file: it reaches the IIS runtime headers.
cc_library(
    name = "iis_daemon_serve",
    srcs = ["iis_daemon_serve.cc"],
    hdrs = ["iis_daemon_serve.h"],
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [
        ":iis_daemon_record",
        "//net/instaweb/rewriter",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
        "//pagespeed/system:daemon_serve_arm",
    ],
)

# Socket-address -> printable IP string helper, split out of iis_http_module.cpp
# so it can be unit-tested in isolation (it is otherwise buried in the big
# iis_module library behind the IIS runtime headers, with no header declaration
# a test could link against). Windows-only (winsock2 / ws2_32 inet_ntop).
cc_library(
    name = "iis_ip_string",
    srcs = ["iis_ip_string.cc"],
    hdrs = ["iis_ip_string.h"],
    defines = ["_WINSOCKAPI_"],
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
    linkopts = select({
        "@platforms//os:windows": ["ws2_32.lib"],
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

# The options registration, for the cross-port pin that asserts the three
# ports bind the daemon options' names, ids and accessor spellings
# identically (test/pagespeed/nginx/ngx_daemon_options_pin_test.cc).
filegroup(
    name = "iis_rewrite_options_src",
    srcs = [
        "iis_rewrite_options.cpp",
        "iis_rewrite_options.h",
    ],
    visibility = ["//visibility:public"],
)

# All port sources, as data: the recorder seam pin under test/pagespeed/iis
# reads them as text (the port cannot be linked into a test -- it sits behind
# the IIS runtime headers), in the same idiom as the nginx port's source
# filegroups.
filegroup(
    name = "iis_module_srcs",
    srcs = glob(
        ["*.cpp", "*.cc", "*.h"],
        allow_empty = False,
    ),
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
