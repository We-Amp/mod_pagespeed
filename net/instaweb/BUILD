licenses(["notice"])  # Apache 2

load("//bazel:version.bzl", "version_header")

# Export VERSION so packages outside net/instaweb (e.g. //pagespeed/iis,
# which generates a VS_VERSION_INFO .rc from the same source-of-truth)
# can reference it via the //net/instaweb:public/VERSION label.
exports_files(["public/VERSION"])

# Generate version.h from version.h.in template
version_header(
    name = "version_header_gen",
    template = "public/version.h.in",
    version_file = "public/VERSION",
    out = "public/version.h",
)

cc_library(
    name = "net_instaweb_lib",
    hdrs = [
        ":version_header_gen",
        "public/global_constants.h",
    ],
    visibility = ["//visibility:public"],
    deps = ["//pagespeed/kernel/base:pagespeed_base_core"],
)
