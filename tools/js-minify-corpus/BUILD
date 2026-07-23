licenses(["notice"])  # Apache 2

# Single-file minifier probe for corpus stress runs. Not a test: the
# oracle/driver scripts live next to it in this directory (python3 stdlib
# only). See README.md.
cc_binary(
    name = "js_minify_probe",
    srcs = ["js_minify_probe.cc"],
    deps = [
        "//pagespeed/kernel/js",
    ],
)
