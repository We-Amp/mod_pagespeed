
filegroup(
    name = "apr_pagespeed_linux",
    srcs = [
        "gen/arch/linux/x64/include/apr.h",
        "gen/arch/linux/x64/include/apr_private.h",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "apr_pagespeed_mac",
    srcs = [
        "gen/arch/mac/x64/include/apr.h",
        "gen/arch/mac/x64/include/apr_private.h",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "apr_pagespeed_linux_arm64",
    srcs = [
        "gen/arch/linux/arm64/include/apr.h",
        "gen/arch/linux/arm64/include/apr_private.h",
    ],
    visibility = ["//visibility:public"],
)

alias(
    name = "apr_pagespeed",
    actual = select({
        "@platforms//os:macos": ":apr_pagespeed_mac",
        "//bazel:linux_arm64": ":apr_pagespeed_linux_arm64",
        "//conditions:default": ":apr_pagespeed_linux",
    }),
    visibility = ["//visibility:public"],
)
