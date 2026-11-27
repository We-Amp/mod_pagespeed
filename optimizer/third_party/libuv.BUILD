# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

LIBUV_COMMON_SRCS = [
    "src/fs-poll.c",
    "src/idna.c",
    "src/inet.c",
    "src/random.c",
    "src/strscpy.c",
    "src/strtok.c",
    "src/thread-common.c",
    "src/threadpool.c",
    "src/timer.c",
    "src/uv-common.c",
    "src/uv-data-getter-setters.c",
    "src/version.c",
]

LIBUV_UNIX_SRCS = [
    "src/unix/async.c",
    "src/unix/core.c",
    "src/unix/dl.c",
    "src/unix/fs.c",
    "src/unix/getaddrinfo.c",
    "src/unix/getnameinfo.c",
    "src/unix/loop.c",
    "src/unix/loop-watcher.c",
    "src/unix/pipe.c",
    "src/unix/poll.c",
    "src/unix/process.c",
    "src/unix/proctitle.c",
    "src/unix/random-devurandom.c",
    "src/unix/signal.c",
    "src/unix/stream.c",
    "src/unix/tcp.c",
    "src/unix/thread.c",
    "src/unix/tty.c",
    "src/unix/udp.c",
]

LIBUV_DARWIN_SRCS = [
    "src/unix/bsd-ifaddrs.c",
    "src/unix/darwin.c",
    "src/unix/darwin-proctitle.c",
    "src/unix/fsevents.c",
    "src/unix/kqueue.c",
    "src/unix/random-getentropy.c",
]

LIBUV_LINUX_SRCS = [
    "src/unix/linux.c",
    "src/unix/procfs-exepath.c",
    "src/unix/random-getrandom.c",
    "src/unix/random-sysctl-linux.c",
]

LIBUV_WIN_SRCS = [
    "src/win/async.c",
    "src/win/core.c",
    "src/win/detect-wakeup.c",
    "src/win/dl.c",
    "src/win/error.c",
    "src/win/fs-event.c",
    "src/win/fs.c",
    "src/win/getaddrinfo.c",
    "src/win/getnameinfo.c",
    "src/win/handle.c",
    "src/win/loop-watcher.c",
    "src/win/pipe.c",
    "src/win/poll.c",
    "src/win/process-stdio.c",
    "src/win/process.c",
    "src/win/signal.c",
    "src/win/snprintf.c",
    "src/win/stream.c",
    "src/win/tcp.c",
    "src/win/thread.c",
    "src/win/tty.c",
    "src/win/udp.c",
    "src/win/util.c",
    "src/win/winapi.c",
    "src/win/winsock.c",
]

cc_library(
    name = "libuv",
    srcs = LIBUV_COMMON_SRCS + select({
        "@platforms//os:windows": LIBUV_WIN_SRCS,
        "//conditions:default": LIBUV_UNIX_SRCS,
    }) + select({
        "@platforms//os:macos": LIBUV_DARWIN_SRCS,
        "@platforms//os:linux": LIBUV_LINUX_SRCS,
        "//conditions:default": [],
    }),
    hdrs = glob(["include/**/*.h"]) + glob(["src/**/*.h"]),
    copts = select({
        "@platforms//os:windows": [
            "/DWIN32_LEAN_AND_MEAN",
            "/D_WIN32_WINNT=0x0601",
            "/D_CRT_SECURE_NO_DEPRECATE",
            "/D_CRT_NONSTDC_NO_DEPRECATE",
        ],
        "//conditions:default": [
            "-D_LARGEFILE_SOURCE",
            "-D_FILE_OFFSET_BITS=64",
            # Upstream libuv callback signatures have unused parameters
            "-Wno-unused-parameter",
            # Upstream libuv uses mixed signed/unsigned comparisons
            "-Wno-sign-compare",
        ],
    }) + select({
        "@platforms//os:macos": ["-D_DARWIN_USE_64_BIT_INODE=1"],
        "@platforms//os:linux": [
            "-D_GNU_SOURCE",
            "-D_POSIX_C_SOURCE=200112",
        ],
        "//conditions:default": [],
    }),
    includes = [
        "include",
        "src",
    ],
    linkopts = select({
        "@platforms//os:windows": [
            "-DEFAULTLIB:iphlpapi.lib",
            "-DEFAULTLIB:psapi.lib",
            "-DEFAULTLIB:user32.lib",
            "-DEFAULTLIB:userenv.lib",
            "-DEFAULTLIB:ws2_32.lib",
            "-DEFAULTLIB:advapi32.lib",
            "-DEFAULTLIB:dbghelp.lib",
            "-DEFAULTLIB:ole32.lib",
            "-DEFAULTLIB:shell32.lib",
        ],
        "@platforms//os:linux": [
            "-lpthread",
            "-ldl",
            "-lrt",
        ],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)
