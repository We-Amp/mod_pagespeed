# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Collapse the duplicate deflate implementation.
#
# Two zlib implementations used to be linked into every binary (and the
# production ngx_pagespeed_module.so): stock madler @zlib — pulled in
# transitively by protobuf's gzip_stream — alongside the vendored
# @zlib_ng. Both export the same symbols (zlib-ng is built ZLIB_COMPAT), so the
# linker bound one or the other depending on link order, producing different —
# both valid — deflate output per binary. That is an ODR/UB hazard and was the
# real cause of the PngOptimizer "golden flake" (different deflate stream by
# link composition, misattributed to zlib-ng SIMD nondeterminism).
#
# This repository rule declares @zlib as a thin alias onto @zlib_ng so the whole
# graph links exactly ONE deflate. It must be invoked from
# mod_pagespeed_dependencies() — which runs before protobuf_deps()
# in WORKSPACE — so its maybe()-guarded madler @zlib declaration is skipped.
def _zlib_ng_alias_impl(repository_ctx):
    repository_ctx.file("WORKSPACE", 'workspace(name = "zlib")\n')
    repository_ctx.file("BUILD.bazel", """\
# @zlib -> @zlib_ng alias. See bazel/zlib_compat.bzl for the why.
alias(
    name = "zlib",
    actual = "@zlib_ng//:zlib_ng",
    visibility = ["//visibility:public"],
)
""")

zlib_ng_alias_repository = repository_rule(
    implementation = _zlib_ng_alias_impl,
    doc = "Declares @zlib as an alias to @zlib_ng so only one deflate is linked.",
)
