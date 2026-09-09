load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # Apache 2

# Cross-platform threading using C++ standard library.
# Use this for Envoy filter builds and Windows support.
cc_library(
    name = "std_thread",
    srcs = [
        "std_condvar.cc",
        "std_mutex.cc",
        "std_rw_lock.cc",
        "std_thread_system.cc",
    ],
    hdrs = [
        "std_condvar.h",
        "std_mutex.h",
        "std_rw_lock.h",
        "std_thread_system.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "@mod_pagespeed//base:log_shim",
    ],
)

# POSIX-specific threading using pthreads.
# Use this for Apache module and POSIX-only builds.
cc_library(
    name = "pthread_thread",
    srcs = [
        "pthread_condvar.cc",
        "pthread_mutex.cc",
        "pthread_rw_lock.cc",
        "pthread_shared_mem.cc",
        "pthread_thread_system.cc",
    ],
    hdrs = [
        "pthread_condvar.h",
        "pthread_mutex.h",
        "pthread_rw_lock.h",
        "pthread_shared_mem.h",
        "pthread_thread_system.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "@mod_pagespeed//base:log_shim",
    ],
)

cc_library(
    name = "thread",
    srcs = [
        "queued_alarm.cc",
        "queued_worker.cc",
        "queued_worker_pool.cc",
        "scheduler.cc",
        "scheduler_based_abstract_lock.cc",
        "scheduler_sequence.cc",
        "scheduler_thread.cc",
        "sequence.cc",
        "thread_synchronizer.cc",
        "worker.cc",
    ],
    hdrs = [
        "queued_alarm.h",
        "queued_worker.h",
        "queued_worker_pool.h",
        "scheduler.h",
        "scheduler_based_abstract_lock.h",
        "scheduler_sequence.h",
        "scheduler_thread.h",
        "sequence.h",
        "thread_synchronizer.h",  # XXX(oschaaf): check, for test?
        "worker.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":std_thread",
        "//pagespeed/kernel/base:pagespeed_base",
        "@mod_pagespeed//base:log_shim",
    ] + select({
        "@platforms//os:windows": [],
        "//conditions:default": [":pthread_thread"],
    }),
)

# Event dispatcher abstraction - unified event loop interface.
# This is the core abstraction used by both Envoy and Apache backends.
cc_library(
    name = "event_dispatcher",
    srcs = [
        "dispatcher_sequence.cc",
        "event_dispatcher.cc",
    ],
    hdrs = [
        "dispatcher_sequence.h",
        "event_dispatcher.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":thread",
        "//pagespeed/kernel/base:pagespeed_base",
        "@mod_pagespeed//base:log_shim",
    ],
)

# Libevent-based dispatcher for standalone use (Apache).
# Runs a background thread with a libevent event loop.
# Note: Uses Envoy's libevent dependency via foreign_cc.
cc_library(
    name = "libevent_dispatcher",
    srcs = ["libevent_dispatcher.cc"],
    hdrs = ["libevent_dispatcher.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":event_dispatcher",
        ":thread",
        "//pagespeed/kernel/base:pagespeed_base",
        "//bazel:libevent",
        "@mod_pagespeed//base:log_shim",
    ],
)

# Scheduler backed by EventDispatcher.
# Uses dispatcher timers instead of condvar waits.
cc_library(
    name = "event_scheduler",
    srcs = ["event_scheduler.cc"],
    hdrs = ["event_scheduler.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":event_dispatcher",
        ":thread",
        "//pagespeed/kernel/base:pagespeed_base",
        "@mod_pagespeed//base:log_shim",
    ],
)

# Windows-native threading using Win32 primitives (CRITICAL_SECTION, SRWLOCK,
# CONDITION_VARIABLE). Adopted from the IISpeed code base.
# Use this for IIS module builds on Windows.
cc_library(
    name = "win_thread",
    srcs = [
        "win_condvar.cc",
        "win_mutex.cc",
        "win_rw_lock.cc",
        "win_thread_system.cc",
    ],
    hdrs = [
        "win_condvar.h",
        "win_mutex.h",
        "win_rw_lock.h",
        "win_thread_system.h",
    ],
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "@mod_pagespeed//base:log_shim",
    ],
)
