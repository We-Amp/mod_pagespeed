// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/nginx/ngx_static_asset_path.h"

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kPrefix[] = "/pagespeed_static/";

TEST(NgxStaticAssetPathTest, NamesTheFileUnderThePrefix) {
  EXPECT_EQ("js_defer.0.js",
            NgxStaticAssetName("/pagespeed_static/js_defer.0.js",
                               "/pagespeed_static/js_defer.0.js", kPrefix));
  EXPECT_EQ("1.JiBnMqyl6S.gif",
            NgxStaticAssetName("/pagespeed_static/1.JiBnMqyl6S.gif",
                               "/pagespeed_static/1.JiBnMqyl6S.gif", kPrefix));
}

TEST(NgxStaticAssetPathTest, ConfiguredPrefixIsHonoured) {
  EXPECT_EQ("a.0.js",
            NgxStaticAssetName("/custom/assets/a.0.js", "/custom/assets/a.0.js",
                               "/custom/assets/"));
  EXPECT_EQ("",
            NgxStaticAssetName("/pagespeed_static/a.0.js",
                               "/pagespeed_static/a.0.js", "/custom/assets/"));
}

TEST(NgxStaticAssetPathTest, NothingAfterThePrefixIsNotAnAsset) {
  EXPECT_EQ("", NgxStaticAssetName("/pagespeed_static/", "/pagespeed_static/",
                                   kPrefix));
  EXPECT_EQ("", NgxStaticAssetName("/pagespeed_static", "/pagespeed_static",
                                   kPrefix));
}

TEST(NgxStaticAssetPathTest, OneCharacterAfterThePrefixIsAnAsset) {
  EXPECT_EQ("a", NgxStaticAssetName("/pagespeed_static/a",
                                    "/pagespeed_static/a", kPrefix));
}

TEST(NgxStaticAssetPathTest, NestedPathIsNotAnAsset) {
  EXPECT_EQ("", NgxStaticAssetName("/pagespeed_static/a/b.0.js",
                                   "/pagespeed_static/a/b.0.js", kPrefix));
}

TEST(NgxStaticAssetPathTest, PathOutsideThePrefixIsNotAnAsset) {
  EXPECT_EQ("", NgxStaticAssetName("/index.html", "/index.html", kPrefix));
  EXPECT_EQ("", NgxStaticAssetName("/pagespeed_staticx/a.0.js",
                                   "/pagespeed_staticx/a.0.js", kPrefix));
  EXPECT_EQ("", NgxStaticAssetName("", "", kPrefix));
}

TEST(NgxStaticAssetPathTest, EmptyPrefixNeverMatches) {
  EXPECT_EQ("", NgxStaticAssetName("/a.0.js", "/a.0.js", ""));
}

// A configured prefix always ends in '/'; anything else is declined.
TEST(NgxStaticAssetPathTest, PrefixWithoutTrailingSlashNeverMatches) {
  EXPECT_EQ("", NgxStaticAssetName("/ps/a.0.js", "/ps/a.0.js", "/ps"));
  EXPECT_EQ("", NgxStaticAssetName("/psfoo.0.js", "/psfoo.0.js", "/ps"));
}

TEST(NgxStaticAssetPathTest, PrefixLongerThanThePathNeverMatches) {
  EXPECT_EQ("", NgxStaticAssetName("/a", "/a", kPrefix));
}

TEST(NgxStaticAssetPathTest, PathAsLongAsThePrefixIsNotAnAsset) {
  EXPECT_EQ("", NgxStaticAssetName("/pagespeed_static_", "/pagespeed_static_",
                                   kPrefix));
}

// The two readings of the request path differ: left to the server, whatever
// their lengths relative to the prefix.
TEST(NgxStaticAssetPathTest, DifferentReadings1) {
  EXPECT_EQ("",
            NgxStaticAssetName("/x", "/pagespeed_static/y.0.js", kPrefix));
}

TEST(NgxStaticAssetPathTest, DifferentReadings2) {
  EXPECT_EQ("", NgxStaticAssetName("/", "/pagespeed_static/a.0.js", kPrefix));
  EXPECT_EQ("", NgxStaticAssetName("", "/pagespeed_static/a.0.js", kPrefix));
}

TEST(NgxStaticAssetPathTest, DifferentReadings3) {
  EXPECT_EQ("", NgxStaticAssetName("/some/other/location/a.0.js",
                                   "/pagespeed_static/a.0.js", kPrefix));
}

TEST(NgxStaticAssetPathTest, DifferentReadings4) {
  EXPECT_EQ("", NgxStaticAssetName("/pagespeed_static/a.0.js",
                                   "/pagespeed_static/b.0.js", kPrefix));
  EXPECT_EQ("", NgxStaticAssetName("/pagespeed_static/b.0.js",
                                   "/pagespeed_static/a.0.js", kPrefix));
}

TEST(NgxStaticAssetPathTest, DifferentReadings5) {
  EXPECT_EQ("", NgxStaticAssetName("/pagespeed_static/a.0.js", "/index.html",
                                   kPrefix));
}

}  // namespace
}  // namespace net_instaweb
