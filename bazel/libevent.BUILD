# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Build file for libevent - cross-platform event notification library
# Used by LibeventDispatcher for standalone event loop (Apache deployments)
#
# This build file creates a minimal libevent library sufficient for
# the LibeventDispatcher's needs.

load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # BSD 3-clause

cc_library(
    name = "libevent",
    srcs = glob(
        [
            "*.c",
        ],
        exclude = [
            "sample/*.c",
            "test/*.c",
            "bench*.c",
            "win32*.c",
            "evthread_win32.c",
            "buffer_iocp.c",
            "bufferevent_async.c",
            "bufferevent_openssl.c",
            "bufferevent_mbedtls.c",
            "arc4random.c",
            "kqueue.c",  # Exclude kqueue on Linux
            "devpoll.c",  # Exclude devpoll (Solaris-specific)
        ],
    ),
    hdrs = glob([
        "*.h",
        "include/**/*.h",
        "compat/**/*.h",
    ]),
    copts = [
        "-DHAVE_CONFIG_H",
        "-D_GNU_SOURCE",
        "-Iexternal/com_github_libevent_libevent",
        "-Iexternal/com_github_libevent_libevent/include",
        "-Iexternal/com_github_libevent_libevent/compat",
    ] + select({
        "@platforms//os:linux": [
            "-DHAVE_EPOLL",
            "-DHAVE_EPOLL_CREATE1",
        ],
        "@platforms//os:macos": [
            "-DHAVE_KQUEUE",
        ],
        "//conditions:default": [],
    }),
    includes = [
        ".",
        "include",
        "compat",
    ],
    linkopts = ["-lpthread"],
    visibility = ["//visibility:public"],
    deps = [
        ":event_config",
    ],
)

# Generate a basic event-config.h for the platform
cc_library(
    name = "event_config",
    hdrs = [
        ":gen_event_config",
        ":gen_evconfig_private",
    ],
    includes = [
        "include/event2",
        ".",
    ],
    visibility = ["//visibility:private"],
)

genrule(
    name = "gen_evconfig_private",
    outs = ["evconfig-private.h"],
    cmd = """
cat > $@ << 'EOF'
/* evconfig-private.h - auto-generated */
#ifndef EVCONFIG_PRIVATE_H_INCLUDED_
#define EVCONFIG_PRIVATE_H_INCLUDED_

/* Internal configuration for libevent */
#define EVENT__HAVE_ARC4RANDOM 1
#define EVENT__HAVE_ARC4RANDOM_BUF 1
#define EVENT__HAVE_ARC4RANDOM_ADDRANDOM 1
#define EVENT__HAVE_CLOCK_GETTIME 1
#define EVENT__HAVE_DECL_CTL_KERN 1
#define EVENT__HAVE_DECL_KERN_ARND 0
#define EVENT__HAVE_FD_MASK 1
#define EVENT__HAVE_GETTIMEOFDAY 1
#define EVENT__HAVE_NANOSLEEP 1
#define EVENT__HAVE_PUTENV 1
#define EVENT__HAVE_SETENV 1
#define EVENT__HAVE_SETRLIMIT 1
#define EVENT__HAVE_SIGACTION 1
#define EVENT__HAVE_STRLCPY 1
#define EVENT__HAVE_STRSEP 1
#define EVENT__HAVE_STRTOK_R 1
#define EVENT__HAVE_STRTOLL 1
#define EVENT__HAVE_STRUCT_ADDRINFO 1
#define EVENT__HAVE_STRUCT_IN6_ADDR 1
#define EVENT__HAVE_STRUCT_SOCKADDR_IN6 1
#define EVENT__HAVE_STRUCT_SOCKADDR_STORAGE 1
#define EVENT__HAVE_STRUCT_SOCKADDR_UN 1
#define EVENT__HAVE_TIMERCLEAR 1
#define EVENT__HAVE_TIMERCMP 1
#define EVENT__HAVE_TIMERISSET 1
#define EVENT__HAVE_UNSETENV 1
#define EVENT__HAVE_USLEEP 1
#define EVENT__HAVE_VASPRINTF 1
#define EVENT__HAVE_GETADDRINFO 1
#define EVENT__HAVE_GETNAMEINFO 1
#define EVENT__HAVE_GETPROTOBYNUMBER 1
#define EVENT__HAVE_GETSERVBYNAME 1
#define EVENT__HAVE_INET_ATON 1
#define EVENT__HAVE_INET_NTOP 1
#define EVENT__HAVE_INET_PTON 1
#define EVENT__HAVE_PIPE 1
#define EVENT__HAVE_PIPE2 1
#define EVENT__HAVE_MMAP 1
#define EVENT__HAVE_SENDFILE 1
#define EVENT__HAVE_SPLICE 1
#define EVENT__HAVE_GETIFADDRS 1
#define EVENT__HAVE_ISSETUGID 1
#define EVENT__HAVE_GETHOSTBYNAME_R 1
#define EVENT__HAVE_FCNTL 1
#define EVENT__DNS_USE_CPU_CLOCK_FOR_ID 1

#ifdef __linux__
#define EVENT__HAVE_EPOLL 1
#define EVENT__HAVE_EPOLL_CREATE1 1
#define EVENT__HAVE_EVENTFD 1
#define EVENT__HAVE_TIMERFD_CREATE 1
#define EVENT__HAVE_SYS_EPOLL_H 1
#define EVENT__HAVE_SYS_EVENTFD_H 1
#define EVENT__HAVE_SYS_TIMERFD_H 1
#define EVENT__HAVE_ACCEPT4 1
/* Note: Don't define EVENT__HAVE_NETINET_IN6_H - use netinet/in.h instead */
#endif

#ifdef __APPLE__
#define EVENT__HAVE_KQUEUE 1
#define EVENT__HAVE_SYS_EVENT_H 1
#endif

#endif /* EVCONFIG_PRIVATE_H_INCLUDED_ */
EOF
""",
    visibility = ["//visibility:private"],
)

genrule(
    name = "gen_event_config",
    outs = ["include/event2/event-config.h"],
    cmd = """
cat > $@ << 'EOF'
/* event-config.h - auto-generated for common Unix systems */
#ifndef EVENT2_EVENT_CONFIG_H_INCLUDED_
#define EVENT2_EVENT_CONFIG_H_INCLUDED_

/* Basic type sizes - 64-bit Unix */
#define EVENT__SIZEOF_INT 4
#define EVENT__SIZEOF_LONG 8
#define EVENT__SIZEOF_LONG_LONG 8
#define EVENT__SIZEOF_OFF_T 8
#define EVENT__SIZEOF_PTHREAD_T 8
#define EVENT__SIZEOF_SHORT 2
#define EVENT__SIZEOF_SIZE_T 8
#define EVENT__SIZEOF_TIME_T 8
#define EVENT__SIZEOF_VOID_P 8
#define EVENT__SIZEOF_SSIZE_T 8

/* Feature detection */
#define EVENT__HAVE_CLOCK_GETTIME 1
#define EVENT__HAVE_DLFCN_H 1
#define EVENT__HAVE_FCNTL_H 1
#define EVENT__HAVE_FD_MASK 1
#define EVENT__HAVE_GETADDRINFO 1
#define EVENT__HAVE_GETNAMEINFO 1
#define EVENT__HAVE_GETTIMEOFDAY 1
#define EVENT__HAVE_INET_NTOP 1
#define EVENT__HAVE_INET_PTON 1
#define EVENT__HAVE_INTTYPES_H 1
#define EVENT__HAVE_MEMORY_H 1
#define EVENT__HAVE_MMAP 1
#define EVENT__HAVE_NANOSLEEP 1
#define EVENT__HAVE_PIPE 1
#define EVENT__HAVE_PIPE2 1
#define EVENT__HAVE_POLL 1
#define EVENT__HAVE_POLL_H 1
#define EVENT__HAVE_PTHREADS 1
#define EVENT__HAVE_SA_FAMILY_T 1
#define EVENT__HAVE_SELECT 1
#define EVENT__HAVE_SETFD 1
#define EVENT__HAVE_SIGNAL 1
#define EVENT__HAVE_SIGACTION 1
#define EVENT__HAVE_STDARG_H 1
#define EVENT__HAVE_STDINT_H 1
#define EVENT__HAVE_STDLIB_H 1
#define EVENT__HAVE_STRINGS_H 1
#define EVENT__HAVE_STRING_H 1
#define EVENT__HAVE_STRUCT_ADDRINFO 1
#define EVENT__HAVE_STRUCT_IN6_ADDR 1
#define EVENT__HAVE_STRUCT_SOCKADDR_IN6 1
#define EVENT__HAVE_STRUCT_SOCKADDR_STORAGE 1
#define EVENT__HAVE_SYS_IOCTL_H 1
#define EVENT__HAVE_SYS_MMAN_H 1
#define EVENT__HAVE_SYS_PARAM_H 1
#define EVENT__HAVE_SYS_QUEUE_H 1
#define EVENT__HAVE_SYS_SELECT_H 1
#define EVENT__HAVE_SYS_SOCKET_H 1
#define EVENT__HAVE_SYS_STAT_H 1
#define EVENT__HAVE_SYS_TIME_H 1
#define EVENT__HAVE_SYS_TYPES_H 1
#define EVENT__HAVE_SYS_UIO_H 1
#define EVENT__HAVE_UINT16_T 1
#define EVENT__HAVE_UINT32_T 1
#define EVENT__HAVE_UINT64_T 1
#define EVENT__HAVE_UINT8_T 1
#define EVENT__HAVE_UNISTD_H 1
#define EVENT__HAVE_USLEEP 1
#define EVENT__TIME_WITH_SYS_TIME 1
#define EVENT__HAVE_STRTOK_R 1
#define EVENT__HAVE_STRTOLL 1
#define EVENT__HAVE_GETPROTOBYNUMBER 1
#define EVENT__HAVE_GETSERVBYNAME 1
#define EVENT__HAVE_GETHOSTBYNAME_R 1
#define EVENT__HAVE_UINTPTR_T 1
#define EVENT__HAVE_ISSETUGID 1
#define EVENT__HAVE_GETIFADDRS 1
#define EVENT__HAVE_ARC4RANDOM 1
#define EVENT__HAVE_ARC4RANDOM_BUF 1
#define EVENT__HAVE_ARC4RANDOM_ADDRANDOM 1

/* Linux-specific */
#ifdef __linux__
#define EVENT__HAVE_EPOLL 1
#define EVENT__HAVE_EPOLL_CREATE1 1
#define EVENT__HAVE_SYS_EPOLL_H 1
#define EVENT__HAVE_SYS_EVENTFD_H 1
#define EVENT__HAVE_EVENTFD 1
#define EVENT__HAVE_SYS_TIMERFD_H 1
#define EVENT__HAVE_TIMERFD_CREATE 1
/* EVENT__HAVE_NETINET_IN6_H - not on Linux, IPv6 is in netinet/in.h */
#endif

/* macOS-specific */
#ifdef __APPLE__
#define EVENT__HAVE_KQUEUE 1
#define EVENT__HAVE_SYS_EVENT_H 1
#endif

/* Version info */
#define EVENT__VERSION "2.1.12-stable"
#define EVENT__NUMERIC_VERSION 0x02010c00

/* Package info */
#define EVENT__PACKAGE "libevent"
#define EVENT__PACKAGE_BUGREPORT ""
#define EVENT__PACKAGE_NAME ""
#define EVENT__PACKAGE_STRING ""
#define EVENT__PACKAGE_TARNAME ""
#define EVENT__PACKAGE_URL ""
#define EVENT__PACKAGE_VERSION ""

#endif /* EVENT2_EVENT_CONFIG_H_INCLUDED_ */
EOF
""",
    visibility = ["//visibility:private"],
)
