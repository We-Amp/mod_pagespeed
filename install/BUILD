licenses(["notice"])  # Apache 2

package(default_visibility = ["//visibility:public"])

# Test content directories for system tests

filegroup(
    name = "mod_pagespeed_example",
    srcs = glob(["mod_pagespeed_example/**/*"]),
)

filegroup(
    name = "mod_pagespeed_test",
    srcs = glob(["mod_pagespeed_test/**/*"]),
)

# The rule_packs system test's pack and pages, for the chain-level test that
# checks them without a server (//test/net/instaweb/rewriter:rule_pack_fixture_test).
filegroup(
    name = "rule_packs_testdata",
    srcs = glob([
        "mod_pagespeed_test/rule_packs/*",
        "mod_pagespeed_test/rule_packs/multi/*",
    ]),
)

filegroup(
    name = "do_not_modify",
    srcs = glob(["do_not_modify/**/*"]),
)

# Configuration templates
filegroup(
    name = "config_templates",
    srcs = [
        "common/pagespeed.conf.template",
        "debug.conf.template",
        "proxy.conf.template",
        "test.conf.template",
    ],
)

# All test data
filegroup(
    name = "test_data",
    srcs = [
        ":config_templates",
        ":do_not_modify",
        ":mod_pagespeed_example",
        ":mod_pagespeed_test",
    ],
)
