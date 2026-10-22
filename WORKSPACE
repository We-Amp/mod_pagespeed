# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

workspace(name = "mod_pagespeed")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

load("//bazel:repositories.bzl", "mod_pagespeed_dependencies")

mod_pagespeed_dependencies()

# NGINX external dependency (for pagespeed/nginx module)
# Requires NGINX_PATH env var or nginx source at third_party/nginx
load("//bazel:nginx.bzl", "nginx_dependencies")

nginx_dependencies()

# googleurl (gurl) — used by pagespeed/kernel/http for URL parsing (parses
# attacker-controllable href/src/url()). Declared in bazel/repositories.bzl
# (com_googlesource_googleurl, GOOGLEURL_COMMIT) so the SBOM generator and
# the THIRD-PARTY-NOTICES drift gate cover it — a WORKSPACE-only declaration
# was invisible to both, which let it ship unattributed.

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
