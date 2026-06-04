load("@rules_proto//proto:defs.bzl", "proto_library")
load("@rules_cc//cc:defs.bzl", "cc_library", "cc_proto_library")
load("@com_github_grpc_grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")

licenses(["notice"])  # Apache 2

proto_library(
    name = "controller_proto",
    srcs = ["controller.proto"],
)

cc_proto_library(
    name = "controller_proto_cc",
    deps = [":controller_proto"],
)

cc_grpc_library(
    name = "controller_proto_grpc",
    srcs = [":controller_proto"],
    grpc_only = True,
    proto_only = False,
    use_external = False,
    visibility = ["//visibility:public"],
    well_known_protos = False,
    deps = [":controller_proto_cc"],
)

# Core controller interfaces and in-process implementations.
# No gRPC dependency — safe for all ports (IIS, Envoy, nginx, Apache).
cc_library(
    name = "controller_core",
    srcs = [
        "central_controller.cc",
        "compatible_central_controller.cc",
        "expensive_operation_callback.cc",
        "in_process_central_controller.cc",
        "named_lock_schedule_rewrite_controller.cc",
        "popularity_contest_schedule_rewrite_controller.cc",
        "queued_expensive_operation_controller.cc",
        "schedule_rewrite_callback.cc",
        "work_bound_expensive_operation_controller.cc",
    ],
    hdrs = [
        "central_controller.h",
        "central_controller_callback.h",
        "compatible_central_controller.h",
        "expensive_operation_callback.h",
        "expensive_operation_controller.h",
        "in_process_central_controller.h",
        "named_lock_schedule_rewrite_controller.h",
        "popularity_contest_schedule_rewrite_controller.h",
        "priority_queue.h",
        "queued_expensive_operation_controller.h",
        "schedule_rewrite_callback.h",
        "schedule_rewrite_controller.h",
        "work_bound_expensive_operation_controller.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/thread",
        "//pagespeed/kernel/util",
        "//pagespeed/system:system_controller_process_h",
    ],
)

# Full controller with gRPC RPC layer for multi-process (fork) deployments.
# alwayslink ensures gRPC symbols are kept in shared libraries (.so) where
# the linker might otherwise strip them during dead code elimination.
cc_library(
    name = "controller",
    alwayslink = True,
    srcs = [
        "central_controller_rpc_client.cc",
        "central_controller_rpc_server.cc",
        "expensive_operation_rpc_context.cc",
        "expensive_operation_rpc_handler.cc",
        "grpc_rtti_anchor.cc",
        "schedule_rewrite_rpc_context.cc",
        "schedule_rewrite_rpc_handler.cc",
    ],
    hdrs = [
        "central_controller_rpc_client.h",
        "central_controller_rpc_server.h",
        "context_registry.h",
        "expensive_operation_rpc_context.h",
        "expensive_operation_rpc_handler.h",
        "grpc.h",
        "request_result_rpc_client.h",
        "request_result_rpc_handler.h",
        "rpc_handler.h",
        "schedule_rewrite_rpc_context.h",
        "schedule_rewrite_rpc_handler.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":controller_core",
        ":controller_proto_grpc",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/thread",
        "//pagespeed/kernel/util",
        "//pagespeed/system:system_controller_process_h",
        # Full gRPC++ runtime (not just codegen stubs) — needed for
        # ServerContextBase RTTI used by the RPC server/client. The controller
        # speaks only InsecureServer/ChannelCredentials over a local socket, so
        # link the _unsecure runtime and skip gRPC's TLS/ALTS/credentials glue.
        "@com_github_grpc_grpc//:grpc++_unsecure",
    ],
)
