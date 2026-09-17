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

// Unit tests for IisInPlaceResourceHandler - IPRO URL encoding/decoding
// and request handling.

#include "pagespeed/iis/iis_inplace_resource_handler.h"

#include "gtest/gtest.h"
#include "test/pagespeed/iis/iis_test_base.h"

namespace net_instaweb {

class IisInPlaceResourceHandlerTest : public IisTestBase {
 protected:
};

// ============================================================================
// URL Detection Tests
// ============================================================================

TEST_F(IisInPlaceResourceHandlerTest, IsPageSpeedUrlPositive) {
  EXPECT_TRUE(IisInPlaceResourceHandler::IsPageSpeedUrl(
      "/style.pagespeed.cf.ABC123.css"));
  EXPECT_TRUE(IisInPlaceResourceHandler::IsPageSpeedUrl(
      "/path/to/image.pagespeed.ic.XYZ789.png"));
  EXPECT_TRUE(IisInPlaceResourceHandler::IsPageSpeedUrl(
      "/scripts/app.pagespeed.jm.DEF456.js"));
  EXPECT_TRUE(IisInPlaceResourceHandler::IsPageSpeedUrl(
      "http://example.com/style.pagespeed.cf.ABC123.css"));
  EXPECT_TRUE(IisInPlaceResourceHandler::IsPageSpeedUrl(
      "/deep/nested/path/file.pagespeed.ce.HASH.ext"));
}

TEST_F(IisInPlaceResourceHandlerTest, IsPageSpeedUrlNegative) {
  EXPECT_FALSE(IisInPlaceResourceHandler::IsPageSpeedUrl("/normal/page.html"));
  EXPECT_FALSE(IisInPlaceResourceHandler::IsPageSpeedUrl("/style.css"));
  EXPECT_FALSE(IisInPlaceResourceHandler::IsPageSpeedUrl("/image.png"));
  EXPECT_FALSE(IisInPlaceResourceHandler::IsPageSpeedUrl(
      "/almost.pagespeed"));  // Incomplete
  EXPECT_FALSE(IisInPlaceResourceHandler::IsPageSpeedUrl(""));
  EXPECT_FALSE(IisInPlaceResourceHandler::IsPageSpeedUrl("/"));
}

// ============================================================================
// URL Decoding Tests
// ============================================================================

TEST_F(IisInPlaceResourceHandlerTest, DecodeUrlCssFilter) {
  GoogleString original, filter_id, hash;

  EXPECT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      "/styles/main.pagespeed.cf.ABC123.css", &original, &filter_id, &hash));
  EXPECT_EQ("/styles/main.css", original);
  EXPECT_EQ("cf", filter_id);
  EXPECT_EQ("ABC123", hash);
}

TEST_F(IisInPlaceResourceHandlerTest, DecodeUrlJsFilter) {
  GoogleString original, filter_id, hash;

  EXPECT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      "/scripts/app.pagespeed.jm.XYZ789.js", &original, &filter_id, &hash));
  EXPECT_EQ("/scripts/app.js", original);
  EXPECT_EQ("jm", filter_id);
  EXPECT_EQ("XYZ789", hash);
}

TEST_F(IisInPlaceResourceHandlerTest, DecodeUrlImageFilter) {
  GoogleString original, filter_id, hash;

  EXPECT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      "/images/photo.pagespeed.ic.DEF456.png", &original, &filter_id, &hash));
  EXPECT_EQ("/images/photo.png", original);
  EXPECT_EQ("ic", filter_id);
  EXPECT_EQ("DEF456", hash);
}

TEST_F(IisInPlaceResourceHandlerTest, DecodeUrlCacheExtendFilter) {
  GoogleString original, filter_id, hash;

  EXPECT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      "/assets/logo.pagespeed.ce.GHIJ.svg", &original, &filter_id, &hash));
  EXPECT_EQ("/assets/logo.svg", original);
  EXPECT_EQ("ce", filter_id);
  EXPECT_EQ("GHIJ", hash);
}

TEST_F(IisInPlaceResourceHandlerTest, DecodeUrlNoExtension) {
  GoogleString original, filter_id, hash;

  EXPECT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      "/path/file.pagespeed.cf.ABC", &original, &filter_id, &hash));
  EXPECT_EQ("/path/file", original);
  EXPECT_EQ("cf", filter_id);
  EXPECT_EQ("ABC", hash);
}

TEST_F(IisInPlaceResourceHandlerTest, DecodeUrlDeepPath) {
  GoogleString original, filter_id, hash;

  EXPECT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      "/a/b/c/d/e/style.pagespeed.cf.HASH.css", &original, &filter_id, &hash));
  EXPECT_EQ("/a/b/c/d/e/style.css", original);
  EXPECT_EQ("cf", filter_id);
  EXPECT_EQ("HASH", hash);
}

TEST_F(IisInPlaceResourceHandlerTest, DecodeUrlInvalidNoMarker) {
  GoogleString original, filter_id, hash;

  EXPECT_FALSE(IisInPlaceResourceHandler::DecodeUrl(
      "/normal/file.css", &original, &filter_id, &hash));
}

TEST_F(IisInPlaceResourceHandlerTest, DecodeUrlInvalidIncomplete) {
  GoogleString original, filter_id, hash;

  // Missing hash
  EXPECT_FALSE(IisInPlaceResourceHandler::DecodeUrl(
      "/file.pagespeed.cf", &original, &filter_id, &hash));

  // Only marker
  EXPECT_FALSE(IisInPlaceResourceHandler::DecodeUrl(
      "/file.pagespeed.", &original, &filter_id, &hash));
}

// ============================================================================
// URL Encoding Tests
// ============================================================================

TEST_F(IisInPlaceResourceHandlerTest, EncodeUrlCss) {
  GoogleString encoded = IisInPlaceResourceHandler::EncodeUrl(
      "/styles/main.css", "cf", "ABC123");
  EXPECT_EQ("/styles/main.pagespeed.cf.ABC123.css", encoded);
}

TEST_F(IisInPlaceResourceHandlerTest, EncodeUrlJs) {
  GoogleString encoded = IisInPlaceResourceHandler::EncodeUrl(
      "/scripts/app.js", "jm", "XYZ789");
  EXPECT_EQ("/scripts/app.pagespeed.jm.XYZ789.js", encoded);
}

TEST_F(IisInPlaceResourceHandlerTest, EncodeUrlImage) {
  GoogleString encoded = IisInPlaceResourceHandler::EncodeUrl(
      "/images/photo.png", "ic", "DEF456");
  EXPECT_EQ("/images/photo.pagespeed.ic.DEF456.png", encoded);
}

TEST_F(IisInPlaceResourceHandlerTest, EncodeUrlNoExtension) {
  GoogleString encoded = IisInPlaceResourceHandler::EncodeUrl(
      "/path/file", "cf", "ABC");
  EXPECT_EQ("/path/file.pagespeed.cf.ABC", encoded);
}

// ============================================================================
// Roundtrip Tests (Encode then Decode)
// ============================================================================

TEST_F(IisInPlaceResourceHandlerTest, RoundtripCss) {
  GoogleString original_url = "/assets/styles/theme.css";
  GoogleString filter_id = "cf";
  GoogleString hash = "AB12CD34";

  // Encode
  GoogleString encoded = IisInPlaceResourceHandler::EncodeUrl(
      original_url, filter_id, hash);

  // Decode
  GoogleString decoded_url, decoded_filter, decoded_hash;
  ASSERT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      encoded, &decoded_url, &decoded_filter, &decoded_hash));

  EXPECT_EQ(original_url, decoded_url);
  EXPECT_EQ(filter_id, decoded_filter);
  EXPECT_EQ(hash, decoded_hash);
}

TEST_F(IisInPlaceResourceHandlerTest, RoundtripJs) {
  GoogleString original_url = "/js/vendor/jquery.min.js";
  GoogleString filter_id = "jm";
  GoogleString hash = "QWERTY";

  GoogleString encoded = IisInPlaceResourceHandler::EncodeUrl(
      original_url, filter_id, hash);

  GoogleString decoded_url, decoded_filter, decoded_hash;
  ASSERT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      encoded, &decoded_url, &decoded_filter, &decoded_hash));

  EXPECT_EQ(original_url, decoded_url);
  EXPECT_EQ(filter_id, decoded_filter);
  EXPECT_EQ(hash, decoded_hash);
}

TEST_F(IisInPlaceResourceHandlerTest, RoundtripImage) {
  GoogleString original_url = "/images/background.webp";
  GoogleString filter_id = "ic";
  GoogleString hash = "WebPHash";

  GoogleString encoded = IisInPlaceResourceHandler::EncodeUrl(
      original_url, filter_id, hash);

  GoogleString decoded_url, decoded_filter, decoded_hash;
  ASSERT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      encoded, &decoded_url, &decoded_filter, &decoded_hash));

  EXPECT_EQ(original_url, decoded_url);
  EXPECT_EQ(filter_id, decoded_filter);
  EXPECT_EQ(hash, decoded_hash);
}

// ============================================================================
// Filter ID Tests
// ============================================================================

TEST_F(IisInPlaceResourceHandlerTest, CommonFilterIds) {
  // Test that common filter IDs work correctly
  struct TestCase {
    const char* filter_id;
    const char* description;
  };

  TestCase cases[] = {
      {"cf", "CSS filter"},
      {"jm", "JavaScript minify"},
      {"jc", "JavaScript combine"},
      {"ic", "Image compress"},
      {"ce", "Cache extend"},
  };

  for (const auto& tc : cases) {
    GoogleString encoded = IisInPlaceResourceHandler::EncodeUrl(
        "/test.css", tc.filter_id, "HASH");
    GoogleString original, filter_id, hash;
    ASSERT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
        encoded, &original, &filter_id, &hash))
        << "Failed for filter: " << tc.description;
    EXPECT_EQ(tc.filter_id, filter_id)
        << "Filter mismatch for: " << tc.description;
  }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(IisInPlaceResourceHandlerTest, UrlWithQueryString) {
  // Query strings should be preserved (but this may vary by implementation)
  EXPECT_TRUE(IisInPlaceResourceHandler::IsPageSpeedUrl(
      "/style.pagespeed.cf.ABC.css?v=1"));
}

TEST_F(IisInPlaceResourceHandlerTest, UrlWithFragment) {
  // Fragments should be preserved
  EXPECT_TRUE(IisInPlaceResourceHandler::IsPageSpeedUrl(
      "/style.pagespeed.cf.ABC.css#section"));
}

TEST_F(IisInPlaceResourceHandlerTest, UrlWithSpecialChars) {
  GoogleString original, filter_id, hash;

  // URL with encoded characters in path
  EXPECT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      "/path%20with%20spaces/style.pagespeed.cf.ABC.css",
      &original, &filter_id, &hash));
}

TEST_F(IisInPlaceResourceHandlerTest, LongHash) {
  GoogleString original_url = "/style.css";
  GoogleString filter_id = "cf";
  GoogleString long_hash = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnop";

  GoogleString encoded = IisInPlaceResourceHandler::EncodeUrl(
      original_url, filter_id, long_hash);

  GoogleString decoded_url, decoded_filter, decoded_hash;
  ASSERT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      encoded, &decoded_url, &decoded_filter, &decoded_hash));

  EXPECT_EQ(long_hash, decoded_hash);
}

TEST_F(IisInPlaceResourceHandlerTest, ShortHash) {
  GoogleString original_url = "/style.css";
  GoogleString filter_id = "cf";
  GoogleString short_hash = "A";

  GoogleString encoded = IisInPlaceResourceHandler::EncodeUrl(
      original_url, filter_id, short_hash);

  GoogleString decoded_url, decoded_filter, decoded_hash;
  ASSERT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      encoded, &decoded_url, &decoded_filter, &decoded_hash));

  EXPECT_EQ(short_hash, decoded_hash);
}

// ============================================================================
// Extension Handling Tests
// ============================================================================

TEST_F(IisInPlaceResourceHandlerTest, MultipleDotsInFilename) {
  GoogleString original, filter_id, hash;

  EXPECT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      "/jquery.min.pagespeed.jm.HASH.js", &original, &filter_id, &hash));
  EXPECT_EQ("/jquery.min.js", original);
}

TEST_F(IisInPlaceResourceHandlerTest, DotInPath) {
  GoogleString original, filter_id, hash;

  EXPECT_TRUE(IisInPlaceResourceHandler::DecodeUrl(
      "/path.with.dots/style.pagespeed.cf.HASH.css",
      &original, &filter_id, &hash));
  EXPECT_EQ("/path.with.dots/style.css", original);
}

}  // namespace net_instaweb
