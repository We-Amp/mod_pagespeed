workspace(name = "mod_pagespeed")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

load("//bazel:repositories.bzl", "mod_pagespeed_dependencies")

mod_pagespeed_dependencies()

# Override com_googlesource_googleurl with corrected checksum
# (upstream archive content changed, breaking the checksum and structure in envoy's config)
http_archive(
    name = "com_googlesource_googleurl",
    sha256 = "fc694942e8a7491dcc1dde1bddf48a31370a1f46fef862bc17acf07c34dc6325",
    urls = ["https://storage.googleapis.com/quiche-envoy-integration/dd4080fec0b443296c0ed0036e1e776df8813aa7.tar.gz"],
    patches = ["//bazel:googleurl_visibility.patch"],
    patch_args = ["-p1"],
)

local_repository(
    name = "envoy_build_config",
    path = ".",
)

load("@envoy//bazel:api_binding.bzl", "envoy_api_binding")

envoy_api_binding()

load("@envoy//bazel:api_repositories.bzl", "envoy_api_dependencies")

envoy_api_dependencies()

load("@envoy//bazel:repositories.bzl", "envoy_dependencies")

envoy_dependencies()

load("@envoy//bazel:bazel_deps.bzl", "envoy_bazel_dependencies")

envoy_bazel_dependencies()

load("@envoy//bazel:repositories_extra.bzl", "envoy_dependencies_extra")

envoy_dependencies_extra()

load("@envoy//bazel:python_dependencies.bzl", "envoy_python_dependencies")

envoy_python_dependencies()

load("@envoy//bazel:dependency_imports.bzl", "envoy_dependency_imports")

envoy_dependency_imports()

load("@envoy//bazel:repo.bzl", "envoy_repo")

envoy_repo()

# NOTE: pip_install has been removed from rules_python in favor of bzlmod.
# If Python pip dependencies are needed, migrate to MODULE.bazel.
