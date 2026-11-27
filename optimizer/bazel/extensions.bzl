# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Module extensions for non-BCR dependencies."""

load("//bazel:nginx.bzl", "nginx_repository")

def _nginx_impl(_module_ctx):
    nginx_repository(name = "nginx")

nginx = module_extension(
    implementation = _nginx_impl,
    environ = ["NGINX_PATH"],
    os_dependent = True,
    arch_dependent = True,
)
