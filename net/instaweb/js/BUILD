exports_files([
    "externs.js",
    "js_utils.js",
])

# Host tool for the data2c genrules (e.g. pagespeed/system/BUILD) and for
# tools/regenerate-rewriter-js.sh. As a genrule tool it runs in the exec
# configuration, so every dependency added here is compiled a second time for
# the host. Keep it to the C++ standard library.
cc_binary(
    name = "data2c",
    srcs = ["data_to_c.cc"],
    visibility = ["//visibility:public"],
)
