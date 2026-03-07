filegroup(
    name = "aprutil_pagespeed_linux",
    srcs = [
        "gen/arch/linux/x64/include/apu.h",
        "gen/arch/linux/x64/include/private/apu_config.h",
        "apr_memcache2.h",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "aprutil_pagespeed_mac",
    srcs = [
        "gen/arch/mac/x64/include/apu.h",
        "gen/arch/mac/x64/include/private/apu_config.h",
        "apr_memcache2.h",
    ],
    visibility = ["//visibility:public"],
)

alias(
    name = "aprutil_pagespeed",
    actual = select({
        "@platforms//os:macos": ":aprutil_pagespeed_mac",
        "//conditions:default": ":aprutil_pagespeed_linux",
    }),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "aprutil_pagespeed_memcache_c",
    srcs = [
        "apr_memcache2.c",
    ],
    visibility = ["//visibility:public"],
)
