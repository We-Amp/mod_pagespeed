filegroup(
    name = "aprutil_pagespeed_linux",
    srcs = [
        "gen/arch/linux/x64/include/apu.h",
        "gen/arch/linux/x64/include/private/apu_config.h",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "aprutil_pagespeed_mac",
    srcs = [
        "gen/arch/mac/x64/include/apu.h",
        "gen/arch/mac/x64/include/private/apu_config.h",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "aprutil_pagespeed_linux_arm64",
    srcs = [
        "gen/arch/linux/arm64/include/apu.h",
        "gen/arch/linux/arm64/include/private/apu_config.h",
    ],
    visibility = ["//visibility:public"],
)

alias(
    name = "aprutil_pagespeed",
    actual = select({
        "@platforms//os:macos": ":aprutil_pagespeed_mac",
        "//bazel:linux_arm64": ":aprutil_pagespeed_linux_arm64",
        "//conditions:default": ":aprutil_pagespeed_linux",
    }),
    visibility = ["//visibility:public"],
)
