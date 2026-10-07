load("//bazel:pagespeed_test.bzl", "pagespeed_cc_test")

licenses(["notice"])  # Apache 2

# Tests for the nginx-free pieces of the nginx port glue. The nginx-dependent
# glue (NgxBaseFetch etc.) has no standalone unit tests; it is covered by the
# nginx system test suite (test/system/run_nginx_tests.sh).
pagespeed_cc_test(
    name = "ngx_segment_buffer_test",
    srcs = ["ngx_segment_buffer_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/nginx:ngx_segment_buffer",
    ],
)

# The weak If-None-Match comparison the module answers its own resources'
# conditionals with -- a pure function over StringPiece, tested standalone.
pagespeed_cc_test(
    name = "ngx_etag_match_test",
    srcs = ["ngx_etag_match_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/nginx:ngx_etag_match",
    ],
)

# The admin-style handler path lookup: exact vs subtree, unconfigured never
# matches, and case-sensitive like the nginx `location` blocks that guard
# these paths.
pagespeed_cc_test(
    name = "ngx_admin_path_match_test",
    srcs = ["ngx_admin_path_match_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/nginx:ngx_admin_path_match",
    ],
)

# The static asset name lookup: both readings of the request path must agree
# and name a file directly under the static asset prefix.
pagespeed_cc_test(
    name = "ngx_static_asset_path_test",
    srcs = ["ngx_static_asset_path_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/nginx:ngx_static_asset_path",
    ],
)

# The daemon record completion queue, caps, drop counter and open-attempt
# latch -- the nginx-free half of the daemon record arm, tested standalone.
pagespeed_cc_test(
    name = "ngx_daemon_record_completion_test",
    srcs = ["ngx_daemon_record_completion_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/thread",
        "//pagespeed/kernel/util:util",
        "//pagespeed/nginx:ngx_daemon_record_completion",
        "//pagespeed/system:daemon_adapter",
        "//pagespeed/system:ipro_recorder",
    ],
)

# The composed response of the daemon serve arm -- the nginx-free half of
# that arm, tested standalone.
pagespeed_cc_test(
    name = "ngx_daemon_serve_emit_test",
    srcs = ["ngx_daemon_serve_emit_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
        "//pagespeed/nginx:ngx_daemon_serve_emit",
        "//pagespeed/system:daemon_serve_arm",
    ],
)

# The gzip_types matching rules for the serve arm's compressibility
# predicate -- parameters ignored, trailing blanks trimmed,
# case-insensitive, text/html implied.
pagespeed_cc_test(
    name = "ngx_gzip_type_match_test",
    srcs = ["ngx_gzip_type_match_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/nginx:ngx_gzip_type_match",
    ],
)

# The serve path's posted notifications: the closure sends exactly one
# notification through the real shared functions against the stub daemon
# library, a cancelled one sends nothing, and it owns its strings.
pagespeed_cc_test(
    name = "ngx_daemon_serve_notify_test",
    srcs = ["ngx_daemon_serve_notify_test.cc"],
    data = ["//test/pagespeed/system:libdaemon_stub.so"],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//net/instaweb/rewriter",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/thread",
        "//pagespeed/kernel/util:util",
        "//pagespeed/nginx:ngx_daemon_record_completion",
        "//pagespeed/nginx:ngx_daemon_serve_notify",
        "//pagespeed/system:daemon_serve_arm",
    ],
)

# Source pin for the two daemon path options: the options are parsed and
# stored but not acted on, so there is no behaviour to execute -- and no
# linkable unit test for the parsing either, because linking
# //pagespeed/nginx:nginx_pagespeed pulls in undefined ngx_* symbols (nginx
# is a headers-only library in this build graph). The test reads the nginx,
# Apache and IIS registrations as text and pins that they agree; see the
# test for why.
pagespeed_cc_test(
    name = "ngx_daemon_options_pin_test",
    srcs = ["ngx_daemon_options_pin_test.cc"],
    data = [
        "//pagespeed/apache:apache_sources",
        "//pagespeed/iis:iis_rewrite_options_src",
        "//pagespeed/nginx:ngx_rewrite_options_src",
    ],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Source pin for the in-place recorder seam: the retype is behaviour-free, so
# there is nothing to execute, and the port cannot be linked into a test (see
# above). The test reads the nginx sources and pins that the recorder is
# built through the shared gate and held by its interface; see the test.
pagespeed_cc_test(
    name = "ngx_daemon_seam_test",
    srcs = ["ngx_daemon_seam_test.cc"],
    data = [
        "//pagespeed/nginx:nginx_sources",
        "//pagespeed/nginx:ngx_pagespeed_src",
    ],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)
