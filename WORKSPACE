workspace(name = "mod_pagespeed")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

load("//bazel:repositories.bzl", "mod_pagespeed_dependencies")

mod_pagespeed_dependencies()

# NGINX external dependency (for pagespeed/nginx module)
# Requires NGINX_PATH env var or nginx source at third_party/nginx
load("//bazel:nginx.bzl", "nginx_dependencies")

nginx_dependencies()

# googleurl — used by pagespeed/kernel/http for URL parsing (parses
# attacker-controllable href/src/url()). Originally an Envoy dep, but
# referenced directly by core PageSpeed code. Fully encapsulated behind the
# GoogleUrl wrapper.
# Snapshot e6c272102e (Aug 2025) - re-pinned from the stale undated Nov 2022
# snapshot (dd4080fe) to pull in upstream Chromium URL-parser fixes and record
# a dated snapshot. Source switched from the quiche-envoy-integration GCS
# bucket (which only mirrors Envoy-pinned commits) to the canonical
# github.com/google/gurl mirror that Envoy itself now uses.
# This is the newest snapshot for which bazel/googleurl_visibility.patch still
# applies cleanly (the Nov 2025 HEAD 94ff147 drifted; see openQuestions).
http_archive(
    name = "com_googlesource_googleurl",
    sha256 = "9b998fea702bfcfa7d8e763389e56a1e889f718a11ada6b7c4e8c77b43d5a999",
    strip_prefix = "gurl-e6c272102e0554e02c1bb317edff927ee56c7d0b",
    urls = ["https://github.com/google/gurl/archive/e6c272102e0554e02c1bb317edff927ee56c7d0b.tar.gz"],
    patches = ["//bazel:googleurl_visibility.patch"],
    patch_args = ["-p1"],
    # NOTE: the former Darwin-only patch_cmds sed renaming
    # __is_cpp17_contiguous_iterator -> __libcpp_is_contiguous_iterator was
    # removed: current libc++ (Xcode 16+ / macOS 26) already uses the new
    # spelling, and the pinned gurl snapshot now carries an upstream block with
    # that spelling too, so the sed produced a duplicate-definition error.
)

# build_bazel_apple_support — declared explicitly here so we can carry
# bazel/apple_support_macos26.patch: 1.17.1's osx_cc_configure.bzl compiles
# wrapped_clang/libtool_check_unique with -Wl,-no_uuid and
# -Wl,-no_adhoc_codesign, which the macOS 26 toolchain rejects (missing
# LC_UUID / immediate SIGKILL on arm64). The patch just drops those two flags.
# This declaration predates and outlives gRPC: grpc_deps() used to pin the
# same 1.17.1 transitively via its existing_rules() guard, but the patched
# Apple crosstool is needed by bazel itself, not by gRPC.
http_archive(
    name = "build_bazel_apple_support",
    sha256 = "b53f6491e742549f13866628ddffcc75d1f3b2d6987dc4f14a16b242113c890b",
    urls = [
        "https://storage.googleapis.com/grpc-bazel-mirror/github.com/bazelbuild/apple_support/releases/download/1.17.1/apple_support.1.17.1.tar.gz",
        "https://github.com/bazelbuild/apple_support/releases/download/1.17.1/apple_support.1.17.1.tar.gz",
    ],
    patches = ["//bazel:apple_support_macos26.patch"],
    patch_args = ["-p1"],
)

# Protocol Buffers C++ runtime + codegen is declared in
# bazel/repositories.bzl (com_google_protobuf, pinned to the v31.1 release
# commit gRPC 1.78.1's grpc_deps() used to provide before the gRPC dependency
# was removed along with the experimental central controller).

# rules_cc and rules_proto for cc_library/cc_proto_library and proto_library.
# Same pins gRPC 1.78.1's grpc_deps() used; declared before protobuf_deps()
# below so its maybe()-style guards keep these versions.
http_archive(
    name = "rules_cc",
    sha256 = "abc605dd850f813bb37004b77db20106a19311a96b2da1c92b789da529d28fe1",
    strip_prefix = "rules_cc-0.0.17",
    urls = [
        "https://github.com/bazelbuild/rules_cc/releases/download/0.0.17/rules_cc-0.0.17.tar.gz",
    ],
)

http_archive(
    name = "rules_proto",
    sha256 = "0e5c64a2599a6e26c6a03d6162242d231ecc0de219534c38cb4402171def21e8",
    strip_prefix = "rules_proto-7.0.2",
    urls = [
        "https://github.com/bazelbuild/rules_proto/archive/refs/tags/7.0.2.tar.gz",
    ],
)

# Protobuf's transitive deps (bazel_skylib, rules_java, rules_python, etc.).
# Skips abseil-cpp/zlib/jsoncpp/rules_cc, already declared above or in
# mod_pagespeed_dependencies().
load("@com_google_protobuf//:protobuf_deps.bzl", "protobuf_deps")

protobuf_deps()

load("@rules_java//java:rules_java_deps.bzl", "rules_java_dependencies")

rules_java_dependencies()

load("@rules_python//python:repositories.bzl", "py_repositories")

py_repositories()

load("@rules_proto//proto:repositories.bzl", "rules_proto_dependencies")

rules_proto_dependencies()

# rules_foreign_cc - for building cmake/autoconf projects (curl, libmemcached)
http_archive(
    name = "rules_foreign_cc",
    sha256 = "4b33d62cf109bcccf286b30ed7121129cc34cf4f4ed9d8a11f38d9108f40ba74",
    strip_prefix = "rules_foreign_cc-0.11.1",
    url = "https://github.com/bazelbuild/rules_foreign_cc/releases/download/0.11.1/rules_foreign_cc-0.11.1.tar.gz",
)

load("@rules_foreign_cc//foreign_cc:repositories.bzl", "rules_foreign_cc_dependencies")

rules_foreign_cc_dependencies()

# bazel_compdb — generates compile_commands.json for clang-tidy.
# Used by tools/gen_compilation_database.py via the compilation_database_aspect.
# NOTE: sha updated 2026-07-20 — GitHub re-rolled the 0.5.2 archive (same
# release content, new tarball bytes); any cold fetch with the old sha fails.
http_archive(
    name = "bazel_compdb",
    sha256 = "d32835b26dd35aad8fd0ba0d712265df6565a3ad860d39e4c01ad41059ea7eda",
    strip_prefix = "bazel-compilation-database-0.5.2",
    urls = ["https://github.com/grailbio/bazel-compilation-database/archive/refs/tags/0.5.2.tar.gz"],
)
