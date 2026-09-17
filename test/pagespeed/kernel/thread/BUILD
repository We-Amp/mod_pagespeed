load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
    "pagespeed_cc_test_library",
)

licenses(["notice"])  # Apache 2

pagespeed_cc_test_library(
    name = "thread_test_base",
    srcs = [
        "mock_scheduler.cc",
        "thread_system_test_base.cc",
        "worker_test_base.cc",
    ],
    hdrs = [
        "blocking_callback.h",
        "condvar_test_base.h",
        "mock_scheduler.h",
        "thread_system_test_base.h",
        "worker_test_base.h",
    ],
    deps = [
        "//pagespeed/kernel/base:std_timer",
        "//pagespeed/kernel/thread",
        "//pagespeed/kernel/thread:std_thread",
        "//pagespeed/kernel/util",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

pagespeed_cc_test(
    name = "thread_test",
    srcs = glob(
        ["*_test.cc"],
        exclude = [
            # These tests need libevent which is a separate dependency.
            "dispatcher_sequence_test.cc",
            "libevent_dispatcher_test.cc",
            "event_scheduler_test.cc",
            # Pthread tests don't build on Windows.
            "pthread_condvar_test.cc",
            "pthread_thread_system_test.cc",
        ],
    ),
    deps = [
        ":thread_test_base",
        "//pagespeed/kernel/thread:event_dispatcher",
    ],
)

# Tests for event dispatcher components that require libevent.
# Use shard_count=1 because libevent-based tests don't work well with sharding
# (multiple processes may interfere with each other's event loops).
# libevent_dispatcher includes <unistd.h> (POSIX-only).
pagespeed_cc_test(
    name = "event_dispatcher_test",
    srcs = [
        "dispatcher_sequence_test.cc",
        "event_dispatcher_test.cc",
        "event_scheduler_test.cc",
        "libevent_dispatcher_test.cc",
    ],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        ":thread_test_base",
        "//pagespeed/kernel/thread:event_dispatcher",
        "//pagespeed/kernel/thread:event_scheduler",
        "//pagespeed/kernel/thread:libevent_dispatcher",
    ],
)
