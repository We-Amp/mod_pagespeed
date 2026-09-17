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

# One-shot equivalence dump for the JsKeywords table: build before and after
# a lookup-implementation change and diff the outputs. See the file header.
cc_binary(
    name = "js_keywords_dump",
    srcs = ["js_keywords_dump.cc"],
    deps = [
        "//pagespeed/kernel/js",
    ],
)
