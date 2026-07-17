load("//bazel:pagespeed_test.bzl", "pagespeed_cc_test", "pagespeed_cc_test_library")

licenses(["notice"])  # Apache 2

# Mock IIS infrastructure for testing
# This can be built on any platform for development, but IIS-specific tests
# require Windows.
pagespeed_cc_test_library(
    name = "mock_iis",
    srcs = ["mock_iis.cc"],
    hdrs = ["mock_iis.h"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
    ],
)

# Test base class for IIS tests
pagespeed_cc_test_library(
    name = "iis_test_base",
    srcs = ["iis_test_base.cc"],
    hdrs = ["iis_test_base.h"],
    deps = [
        ":mock_iis",
        "//net/instaweb/rewriter:rewriter",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
        "//pagespeed/kernel/thread:thread",
        "//pagespeed/kernel/util:util",
        "//test/pagespeed/kernel/base:kernel_test_util",
        "//test/pagespeed/kernel/thread:thread_test_base",
        "@googletest//:gtest",
    ],
)

# Unit tests for IIS module components
# These tests run on Windows only

pagespeed_cc_test(
    name = "iis_config_test",
    tags = ["manual"],  # Old 1.1 API; needs rewrite for IISpeed adoption
    srcs = ["iis_config_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/iis:iis_module",
            ],
)

pagespeed_cc_test(
    name = "iis_html_rewriter_test",
    srcs = ["iis_html_rewriter_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/automatic",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

pagespeed_cc_test(
    name = "iis_http_module_test",
    srcs = ["iis_http_module_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
    ],
)

# Cross-platform: the zero-copy serve decision core is header-only pure
# logic (no IIS/Windows headers), so its safety rules run everywhere.
pagespeed_cc_test(
    name = "iis_zerocopy_serve_test",
    srcs = ["iis_zerocopy_serve_test.cc"],
    deps = [
        "//pagespeed/iis:iis_zerocopy_serve",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

pagespeed_cc_test(
    name = "iis_base_fetch_test",
    srcs = ["iis_base_fetch_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/kernel/http:http",
    ],
)

pagespeed_cc_test(
    name = "iis_inplace_resource_handler_test",
    tags = ["manual"],  # Old 1.1 API; needs rewrite for IISpeed adoption
    srcs = ["iis_inplace_resource_handler_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/iis:iis_module",
            ],
)

pagespeed_cc_test(
    name = "mock_iis_test",
    srcs = ["mock_iis_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
            ],
)

# New tests added for TEST_PARITY_PLAN.md

pagespeed_cc_test(
    name = "iis_header_util_test",
    srcs = ["iis_header_util_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/kernel/http:http",
    ],
)

pagespeed_cc_test(
    name = "iis_writer_test",
    srcs = ["iis_writer_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
    ],
)

# Platform-independent regression test for the OnSendResponse "leave" path
# use-after-free (intermittent w3wp crash caught by AppVerifier/Page Heap).
# No IIS/Windows deps, so it runs on the Linux CI lanes too.
pagespeed_cc_test(
    name = "iis_proxy_fetch_completion_test",
    srcs = ["iis_proxy_fetch_completion_test.cc"],
    deps = [
        "//pagespeed/iis:iis_proxy_fetch_completion",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

pagespeed_cc_test(
    name = "iis_fetch_lifecycle_test",
    srcs = ["iis_fetch_lifecycle_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
    ],
)

# Test base class for IIS tests with RewriteDriver integration
pagespeed_cc_test_library(
    name = "iis_rewrite_test_base",
    srcs = ["iis_rewrite_test_base.cc"],
    hdrs = ["iis_rewrite_test_base.h"],
    deps = [
        ":mock_iis",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
        "//test/net/instaweb/rewriter:test_base",
    ],
)

# Integration test combining IIS mock with RewriteDriver
pagespeed_cc_test(
    name = "iis_rewrite_integration_test",
    srcs = ["iis_rewrite_integration_test.cc"],
    deps = [
        ":iis_rewrite_test_base",
        ":mock_iis",
        "//net/instaweb/rewriter:rewriter",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
    ],
)

# Tests for Phase 1 critical fixes
# These tests are platform-independent and can run on Linux.

# Config tokenizer - parses IIS config file lines
# Platform-independent test (header-only, no Windows deps)
pagespeed_cc_test(
    name = "iis_config_util_test",
    srcs = ["iis_config_util_test.cc"],
    deps = [
        "//pagespeed/iis:iis_config_util",
    ],
)

# Dechunker - HTTP chunked transfer encoding decoder
# Platform-independent test
pagespeed_cc_test(
    name = "iis_dechunker_test",
    tags = ["manual"],  # Old 1.1 Dechunker API; new IISpeed dechunker needs new test
    srcs = ["iis_dechunker_test.cc"],
    deps = [
        "//pagespeed/iis:iis_dechunker",
    ],
)

# ContentDecoder - gzip/deflate decompression
# Platform-independent test (uses zlib)
pagespeed_cc_test(
    name = "iis_content_decoder_test",
    tags = ["manual"],  # Excluded: old 1.1 iis_content_decoder target removed by the design record
    srcs = ["iis_content_decoder_test.cc"],
    deps = [
        "//pagespeed/iis:iis_content_decoder",
    ],
)

# IisStreamingFetch - Streaming AsyncFetch with HTTP_DATA_CHUNK support
# Platform-independent test (mock IIS types used on Linux)
pagespeed_cc_test(
    name = "iis_streaming_fetch_test",
    tags = ["manual"],  # Excluded: old 1.1 iis_streaming_fetch target removed by the design record
    srcs = ["iis_streaming_fetch_test.cc"],
    deps = [
        ":mock_iis",
        "//pagespeed/iis:iis_streaming_fetch",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/util:util",
        "//pagespeed/opt/http:http",
    ],
)

# FlushNotification - Custom IIS notification for flush events
# Platform-independent test (uses mock notification class on Linux)
pagespeed_cc_test(
    name = "iis_flush_notification_test",
    tags = ["manual"],  # Excluded: old 1.1 iis_flush_notification target removed by the design record
    srcs = ["iis_flush_notification_test.cc"],
    deps = [
        "//pagespeed/iis:iis_flush_notification",
    ],
)

# HTML flushing end-to-end integration test
# Combines IisStreamingFetch with mock IIS infrastructure to verify
# the complete HTML flushing flow works correctly.
# Platform-independent test (mock IIS types used on Linux)
pagespeed_cc_test(
    name = "iis_html_flushing_integration_test",
    srcs = ["iis_html_flushing_integration_test.cc"],
    tags = ["manual"],  # Excluded from wildcard builds: pre-existing build failure (missing TakePendingChunks)
    deps = [
        ":mock_iis",
        "//pagespeed/iis:iis_streaming_fetch",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/util:util",
        "//pagespeed/opt/http:http",
    ],
)

# IisServerContext - Server context lifecycle and configuration
# Windows-only test (requires IisRewriteDriverFactory)
pagespeed_cc_test(
    name = "iis_server_context_test",
    tags = ["manual"],  # Old 1.1 API; needs rewrite for IISpeed adoption
    srcs = ["iis_server_context_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        "//pagespeed/iis:iis_module",
    ],
)

# IisModuleFactory - Module factory lifecycle, module creation, cleanup
# Windows-only test (requires IHttpServer and related IIS types)
pagespeed_cc_test(
    name = "iis_module_factory_test",
    tags = ["manual"],  # Old 1.1 API; needs rewrite for IISpeed adoption
    srcs = ["iis_module_factory_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        "//pagespeed/iis:iis_module",
    ],
)

# ConfigurationFile parsing tests (Phases 2-5)
# Tests ParseConfigText extraction, bug exposure, functionality, and edge cases.
# Windows-only: depends on iis_module which requires Windows headers.
pagespeed_cc_test(
    name = "iis_configuration_test",
    srcs = ["iis_configuration_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        "//pagespeed/iis:iis_module",
    ],
)

# Regression test for IisAsyncUrlFetcher::DeriveHost lifetime contract.
# Windows-only because iis_async_url_fetcher.h pulls in <Windows.h> and
# <winhttp.h> via asyncwinhttp.h. The test itself exercises only the
# platform-independent host-derivation helper, but linking the symbol
# requires the Windows-only iis_module library.
pagespeed_cc_test(
    name = "iis_async_url_fetcher_test",
    srcs = ["iis_async_url_fetcher_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        "//pagespeed/iis:iis_module",
    ],
)

# GetIPString(SOCKADDR*) unit tests. Windows-only (winsock2 types); the helper
# was split out of iis_http_module.cpp into //pagespeed/iis:iis_ip_string
# precisely so it could be linked and tested here in isolation (the previous
# free function had no header declaration and was buried in iis_module).
pagespeed_cc_test(
    name = "iis_get_ip_string_test",
    srcs = ["iis_get_ip_string_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        "//pagespeed/iis:iis_ip_string",
    ],
)

# Windows semantics of StdioFileSystem::RenameFile: renaming
# onto an existing path must replace it (the POSIX contract WriteFileAtomic
# and PurgeContext depend on), riding out transient no-share-delete readers.
# Lives here because this is the unit-test tree the Windows CI lane builds.
pagespeed_cc_test(
    name = "iis_win_rename_test",
    srcs = ["iis_win_rename_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base_core",
    ],
)
