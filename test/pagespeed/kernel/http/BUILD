load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
    "pagespeed_cc_test_library",
)

licenses(["notice"])  # Apache 2

pagespeed_cc_test_library(
    name = "http_test_base",
    srcs = [
        "user_agent_matcher_test_base.cc",
    ],
    hdrs = [
        "unknown_headers_field.h",
        "user_agent_matcher_test_base.h",
    ],
    deps = [
        "//pagespeed/kernel/http",
        "//test/pagespeed/kernel/base:kernel_test_util",
        "//test/pagespeed/kernel/util:util_test_base",
    ],
)

pagespeed_cc_test(
    name = "http",
    srcs = glob(
        ["*_test.cc"],
        exclude = ["known_agent_table_test.cc"],
    ),
    deps = [
        "//test/pagespeed/kernel/http:http_test_base",
    ],
)

# The shared known-agent vectors (test/system/daemon_html/agent_vectors.txt)
# for every port's unit tests.
pagespeed_cc_test_library(
    name = "agent_vectors",
    srcs = ["agent_vectors.cc"],
    hdrs = ["agent_vectors.h"],
    data = ["//test/system/daemon_html:agent_vectors.txt"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//test/pagespeed/kernel/base:pagespeed_gtest",
    ],
)

pagespeed_cc_test(
    name = "known_agent_table_test",
    srcs = ["known_agent_table_test.cc"],
    shard_count = 1,
    deps = [
        ":agent_vectors",
        "//pagespeed/kernel/http:known_agent_table",
    ],
)
