load("@rules_cc//cc:defs.bzl", "cc_binary")

licenses(["notice"])  # Apache 2

exports_files(["GIT_COMMIT"])

cc_binary(
    name = "mod_pagespeed",
    deps = [
        "//net/instaweb:net_instaweb_lib",
        "//net/instaweb/rewriter:html_minifier_main_lib",
    ],
)

cc_binary(
    name = "libmod_pagespeed.so",
    linkopts = [
        # Version script hides all symbols except pagespeed_module (the Apache
        # module entry point). This prevents BoringSSL symbols linked into
        # mod_pagespeed from conflicting with system OpenSSL in mod_ssl.
        "-Wl,--version-script,$(location //pagespeed/apache:mod_pagespeed.lds)",
    ],
    linkshared = 1,
    linkstatic = 1,
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/apache",
        "//pagespeed/apache:mod_pagespeed.lds",
    ],
)
