load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_benchmark",
)

licenses(["notice"])  # Apache 2

# The agent class's cost on the unknown (browser) path against an empty
# classifier, with the counter increments every such request pays.
#   bazel run -c opt //benchmark/pagespeed/system:daemon_html_agent_class
pagespeed_cc_benchmark(
    name = "daemon_html_agent_class",
    srcs = ["daemon_html_agent_class_speed_test.cc"],
    deps = [
        "//net/instaweb/rewriter",
        "//pagespeed/kernel/http:known_agent_table",
        "//pagespeed/kernel/util",
        "//pagespeed/system:daemon_html_agent_class",
    ],
)
