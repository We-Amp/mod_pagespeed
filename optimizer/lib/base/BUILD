# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# POSIX compatibility layer (cross-platform open/write/close/rename/unlink)
cc_library(
    name = "posix_compat",
    hdrs = ["posix_compat.h"],
    visibility = ["//:internal"],
)

# Atomic file writer (write-to-temp + fsync + rename)
cc_library(
    name = "atomic_file_writer",
    srcs = ["atomic_file_writer.cc"],
    hdrs = ["atomic_file_writer.h"],
    visibility = ["//:internal"],
    deps = [":posix_compat"],
)

# URL utility functions (UrlDirectory, ResolvePath)
cc_library(
    name = "url_util",
    srcs = ["url_util.cc"],
    hdrs = ["url_util.h"],
    visibility = ["//:internal"],
)

# Basic types and macros
cc_library(
    name = "basictypes",
    hdrs = ["basictypes.h"],
    visibility = ["//:internal"],
)

# Scoped pointer compatibility (maps to std::unique_ptr)
cc_library(
    name = "scoped_ptr",
    hdrs = ["scoped_ptr.h"],
    visibility = ["//:internal"],
)

# Printf format string annotations
cc_library(
    name = "printf_format",
    hdrs = ["printf_format.h"],
    visibility = ["//:internal"],
)

# Intrusive singly-linked list
cc_library(
    name = "inline_slist",
    hdrs = ["inline_slist.h"],
    visibility = ["//:internal"],
)

# String utilities - unique functions beyond C++23 and abseil
cc_library(
    name = "string_util",
    srcs = ["string_util.cc"],
    hdrs = ["string_util.h"],
    visibility = ["//:internal"],
)

# Message handler interface
cc_library(
    name = "message_handler",
    srcs = ["message_handler.cc"],
    hdrs = ["message_handler.h"],
    visibility = ["//:internal"],
)

# JSON structured message handler
cc_library(
    name = "json_message_handler",
    srcs = ["json_message_handler.cc"],
    hdrs = ["json_message_handler.h"],
    visibility = ["//:internal"],
    deps = [
        ":message_handler",
        ":string_util",
    ],
)

# Tee message handler - delegates + callback
cc_library(
    name = "tee_message_handler",
    srcs = ["tee_message_handler.cc"],
    hdrs = ["tee_message_handler.h"],
    visibility = ["//:internal"],
    deps = [":message_handler"],
)

# Arena memory allocator
cc_library(
    name = "arena",
    hdrs = ["arena.h"],
    visibility = ["//:internal"],
)

# Writer interface for output streams
cc_library(
    name = "writer",
    srcs = ["writer.cc"],
    hdrs = ["writer.h"],
    visibility = ["//:internal"],
)

# String writer implementation
cc_library(
    name = "string_writer",
    srcs = ["string_writer.cc"],
    hdrs = ["string_writer.h"],
    visibility = ["//:internal"],
    deps = [":writer"],
)

# Null writer (discards output)
cc_library(
    name = "null_writer",
    srcs = ["null_writer.cc"],
    hdrs = ["null_writer.h"],
    visibility = ["//:internal"],
    deps = [":writer"],
)

# Atom: Interned string representation
cc_library(
    name = "atom",
    hdrs = ["atom.h"],
    visibility = ["//:internal"],
)

# Symbol table: String interning
cc_library(
    name = "symbol_table",
    srcs = ["symbol_table.cc"],
    hdrs = ["symbol_table.h"],
    visibility = ["//:internal"],
    deps = [
        ":atom",
        ":basictypes",
    ],
)

# Combined base library for convenience
cc_library(
    name = "base",
    visibility = ["//:internal"],
    deps = [
        ":arena",
        ":atom",
        ":atomic_file_writer",
        ":basictypes",
        ":inline_slist",
        ":json_message_handler",
        ":message_handler",
        ":null_writer",
        ":posix_compat",
        ":printf_format",
        ":scoped_ptr",
        ":string_util",
        ":string_writer",
        ":symbol_table",
        ":writer",
    ],
)
