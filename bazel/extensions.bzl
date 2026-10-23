# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Module extension for the dependencies that are not taken from the BCR.

The repositories keep the apparent names they had under WORKSPACE (@apr,
@libpng, @cyclone, ...), so BUILD files and labels are unchanged. Their pins
stay in bazel/repositories.bzl, which the SBOM, notices, CVE and vendoring
tools read.

Repo specs carry no host paths, so the extension uses the default metadata
and its repos are recorded in MODULE.bazel.lock. @nginx reads NGINX_PATH at
fetch time (nginx_repository is local and lists it in environ), so it is
re-fetched when that variable changes.
"""

load("//bazel:nginx.bzl", "nginx_dependencies")
load("//bazel:repositories.bzl", "non_bcr_repositories")

def _non_bcr_deps_impl(_mctx):
    non_bcr_repositories()
    nginx_dependencies()

non_bcr_deps = module_extension(implementation = _non_bcr_deps_impl)
