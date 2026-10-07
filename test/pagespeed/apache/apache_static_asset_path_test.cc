/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pagespeed/apache/apache_static_asset_path.h"

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kPrefix[] = "/pagespeed_static/";

TEST(ApacheStaticAssetPathTest, NamesTheFileUnderThePrefix) {
  EXPECT_EQ("js_defer.0.js",
            ApacheStaticAssetName("/pagespeed_static/js_defer.0.js",
                                  "/pagespeed_static/js_defer.0.js", kPrefix));
  EXPECT_EQ(
      "1.JiBnMqyl6S.gif",
      ApacheStaticAssetName("/pagespeed_static/1.JiBnMqyl6S.gif",
                            "/pagespeed_static/1.JiBnMqyl6S.gif", kPrefix));
}

TEST(ApacheStaticAssetPathTest, ConfiguredPrefixIsHonoured) {
  EXPECT_EQ("a.0.js",
            ApacheStaticAssetName("/custom/assets/a.0.js",
                                  "/custom/assets/a.0.js", "/custom/assets/"));
  EXPECT_EQ(
      "", ApacheStaticAssetName("/pagespeed_static/a.0.js",
                                "/pagespeed_static/a.0.js", "/custom/assets/"));
}

TEST(ApacheStaticAssetPathTest, NothingAfterThePrefixIsNotAnAsset) {
  EXPECT_EQ("", ApacheStaticAssetName("/pagespeed_static/",
                                      "/pagespeed_static/", kPrefix));
  EXPECT_EQ("", ApacheStaticAssetName("/pagespeed_static", "/pagespeed_static",
                                      kPrefix));
}

TEST(ApacheStaticAssetPathTest, NestedPathIsNotAnAsset) {
  EXPECT_EQ("", ApacheStaticAssetName("/pagespeed_static/a/b.0.js",
                                      "/pagespeed_static/a/b.0.js", kPrefix));
}

TEST(ApacheStaticAssetPathTest, PathOutsideThePrefixIsNotAnAsset) {
  EXPECT_EQ("", ApacheStaticAssetName("/index.html", "/index.html", kPrefix));
  EXPECT_EQ("", ApacheStaticAssetName("/pagespeed_staticx/a.0.js",
                                      "/pagespeed_staticx/a.0.js", kPrefix));
  EXPECT_EQ("", ApacheStaticAssetName("", "", kPrefix));
}

TEST(ApacheStaticAssetPathTest, EmptyPrefixNeverMatches) {
  EXPECT_EQ("", ApacheStaticAssetName("/a.0.js", "/a.0.js", ""));
}

// A configured prefix always ends in '/'; anything else is declined.
TEST(ApacheStaticAssetPathTest, PrefixWithoutTrailingSlashNeverMatches) {
  EXPECT_EQ("", ApacheStaticAssetName("/ps/a.0.js", "/ps/a.0.js", "/ps"));
  EXPECT_EQ("", ApacheStaticAssetName("/psfoo.0.js", "/psfoo.0.js", "/ps"));
}

TEST(ApacheStaticAssetPathTest, PrefixLongerThanThePathNeverMatches) {
  EXPECT_EQ("", ApacheStaticAssetName("/a", "/a", kPrefix));
}

TEST(ApacheStaticAssetPathTest, PathAsLongAsThePrefixIsNotAnAsset) {
  EXPECT_EQ("", ApacheStaticAssetName("/pagespeed_static_",
                                      "/pagespeed_static_", kPrefix));
}

// The two readings of the request path differ: left to the server, whatever
// their lengths relative to the prefix.
TEST(ApacheStaticAssetPathTest, DifferentReadings1) {
  EXPECT_EQ("",
            ApacheStaticAssetName("/x", "/pagespeed_static/y.0.js", kPrefix));
}

TEST(ApacheStaticAssetPathTest, DifferentReadings2) {
  EXPECT_EQ("",
            ApacheStaticAssetName("/", "/pagespeed_static/a.0.js", kPrefix));
  EXPECT_EQ("", ApacheStaticAssetName("", "/pagespeed_static/a.0.js", kPrefix));
}

TEST(ApacheStaticAssetPathTest, DifferentReadings3) {
  EXPECT_EQ("", ApacheStaticAssetName("/some/other/location/a.0.js",
                                      "/pagespeed_static/a.0.js", kPrefix));
}

TEST(ApacheStaticAssetPathTest, DifferentReadings4) {
  EXPECT_EQ("", ApacheStaticAssetName("/pagespeed_static/a.0.js",
                                      "/pagespeed_static/b.0.js", kPrefix));
  EXPECT_EQ("", ApacheStaticAssetName("/pagespeed_static/a.0.js",
                                      "/pagespeed_static/c.0.js", kPrefix));
}

TEST(ApacheStaticAssetPathTest, DifferentReadings5) {
  EXPECT_EQ("", ApacheStaticAssetName("/pagespeed_static/a.0.js", "/index.html",
                                      kPrefix));
}

}  // namespace
}  // namespace net_instaweb
