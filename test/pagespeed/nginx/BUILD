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
