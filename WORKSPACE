workspace(name = "mod_pagespeed")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

load("//bazel:repositories.bzl", "mod_pagespeed_dependencies")

mod_pagespeed_dependencies()

# NGINX external dependency (for pagespeed/nginx module)
# Requires NGINX_PATH env var or nginx source at third_party/nginx
load("//bazel:nginx.bzl", "nginx_dependencies")

nginx_dependencies()

# googleurl — used by pagespeed/kernel/http for URL parsing.
# Originally an Envoy dep, but referenced directly by core PageSpeed code.
http_archive(
    name = "com_googlesource_googleurl",
    sha256 = "fc694942e8a7491dcc1dde1bddf48a31370a1f46fef862bc17acf07c34dc6325",
    urls = ["https://storage.googleapis.com/quiche-envoy-integration/dd4080fec0b443296c0ed0036e1e776df8813aa7.tar.gz"],
    patches = ["//bazel:googleurl_visibility.patch"],
    patch_args = ["-p1"],
    patch_cmds = [
        """if [ "$(uname)" = "Darwin" ]; then sed -i.bak 's/__is_cpp17_contiguous_iterator/__libcpp_is_contiguous_iterator/g' base/containers/checked_iterators.h && rm -f base/containers/checked_iterators.h.bak; fi""",
    ],
)

# gRPC transitive dependencies (abseil, protobuf, c-ares, upb, etc.)
# Uses maybe() — only declares repos not already in mod_pagespeed_dependencies().
load("@com_github_grpc_grpc//bazel:grpc_deps.bzl", "grpc_deps")

grpc_deps()

load("@com_github_grpc_grpc//bazel:grpc_extra_deps.bzl", "grpc_extra_deps")

grpc_extra_deps()

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
http_archive(
    name = "bazel_compdb",
    sha256 = "cd9e2dd65ee15c985b4b4b0f0f0c39dc3ef84fd749cbeef9c40c2f24a3c45a4d",
    strip_prefix = "bazel-compilation-database-0.5.2",
    urls = ["https://github.com/grailbio/bazel-compilation-database/archive/refs/tags/0.5.2.tar.gz"],
)
