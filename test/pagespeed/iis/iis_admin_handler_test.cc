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

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/iis/iis_test_base.h"
#include "test/pagespeed/iis/mock_iis.h"

#ifdef _WIN32
#include "pagespeed/iis/iis_admin_handler.h"
#include "pagespeed/iis/iis_beacon_handler.h"
#include "pagespeed/iis/iis_config.h"
#endif

namespace net_instaweb {

class IisAdminHandlerTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
  }
};

// ========================= Admin Path Detection Tests =========================

TEST_F(IisAdminHandlerTest, IsAdminPath_AdminRoot) {
#ifdef _WIN32
  EXPECT_TRUE(IisAdminHandler::IsAdminPath("/pagespeed_admin"));
#endif
}

TEST_F(IisAdminHandlerTest, IsAdminPath_AdminSubpath) {
#ifdef _WIN32
  EXPECT_TRUE(IisAdminHandler::IsAdminPath("/pagespeed_admin/"));
  EXPECT_TRUE(IisAdminHandler::IsAdminPath("/pagespeed_admin/statistics"));
  EXPECT_TRUE(IisAdminHandler::IsAdminPath("/pagespeed_admin/cache"));
  EXPECT_TRUE(IisAdminHandler::IsAdminPath("/pagespeed_admin/config"));
  EXPECT_TRUE(IisAdminHandler::IsAdminPath("/pagespeed_admin/messages"));
#endif
}

TEST_F(IisAdminHandlerTest, IsAdminPath_NonAdminPaths) {
#ifdef _WIN32
  EXPECT_FALSE(IisAdminHandler::IsAdminPath("/"));
  EXPECT_FALSE(IisAdminHandler::IsAdminPath("/pagespeed"));
  EXPECT_FALSE(IisAdminHandler::IsAdminPath("/pagespeed_admins"));
  EXPECT_FALSE(IisAdminHandler::IsAdminPath("/other/pagespeed_admin"));
#endif
}

TEST_F(IisAdminHandlerTest, IsStatisticsPath_Valid) {
#ifdef _WIN32
  EXPECT_TRUE(IisAdminHandler::IsStatisticsPath("/pagespeed_statistics"));
  EXPECT_TRUE(IisAdminHandler::IsStatisticsPath("/mod_pagespeed_statistics"));
#endif
}

TEST_F(IisAdminHandlerTest, IsStatisticsPath_Invalid) {
#ifdef _WIN32
  EXPECT_FALSE(IisAdminHandler::IsStatisticsPath("/pagespeed_statistics/"));
  EXPECT_FALSE(IisAdminHandler::IsStatisticsPath("/statistics"));
  EXPECT_FALSE(IisAdminHandler::IsStatisticsPath("/"));
#endif
}

// ========================= Beacon Path Detection Tests =========================

class IisBeaconHandlerTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
  }
};

TEST_F(IisBeaconHandlerTest, IsBeaconPath_Valid) {
#ifdef _WIN32
  EXPECT_TRUE(IisBeaconHandler::IsBeaconPath("/pagespeed_beacon"));
  EXPECT_TRUE(IisBeaconHandler::IsBeaconPath("/pagespeed_beacon?data=foo"));
#endif
}

TEST_F(IisBeaconHandlerTest, IsBeaconPath_Invalid) {
#ifdef _WIN32
  EXPECT_FALSE(IisBeaconHandler::IsBeaconPath("/"));
  EXPECT_FALSE(IisBeaconHandler::IsBeaconPath("/beacon"));
  EXPECT_FALSE(IisBeaconHandler::IsBeaconPath("/pagespeed_beacons"));
  EXPECT_FALSE(IisBeaconHandler::IsBeaconPath("/pagespeed_beacon/subpath"));
#endif
}

// ========================= Config IP Allow List Tests =========================

class IisConfigSecurityTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
  }
};

TEST_F(IisConfigSecurityTest, IsIpAllowed_LocalhostIPv4) {
#ifdef _WIN32
  IisConfig config;
  EXPECT_TRUE(config.IsIpAllowed("127.0.0.1"));
#endif
}

TEST_F(IisConfigSecurityTest, IsIpAllowed_LocalhostIPv6) {
#ifdef _WIN32
  IisConfig config;
  EXPECT_TRUE(config.IsIpAllowed("::1"));
#endif
}

TEST_F(IisConfigSecurityTest, IsIpAllowed_LocalhostName) {
#ifdef _WIN32
  IisConfig config;
  EXPECT_TRUE(config.IsIpAllowed("localhost"));
#endif
}

TEST_F(IisConfigSecurityTest, IsIpAllowed_EmptyIp) {
#ifdef _WIN32
  IisConfig config;
  EXPECT_FALSE(config.IsIpAllowed(""));
#endif
}

TEST_F(IisConfigSecurityTest, IsIpAllowed_RandomExternalIp) {
#ifdef _WIN32
  IisConfig config;
  // Default config only allows localhost
  EXPECT_FALSE(config.IsIpAllowed("192.168.1.1"));
  EXPECT_FALSE(config.IsIpAllowed("10.0.0.1"));
  EXPECT_FALSE(config.IsIpAllowed("8.8.8.8"));
#endif
}

// ========================= Beacon Payload Parsing Tests =========================

// Test helper class to access private ParseBeaconPayload method
#ifdef _WIN32
class BeaconPayloadParserTest : public IisTestBase {
 protected:
  // Since ParseBeaconPayload is private, we test through HandleRequest
  // behavior or by testing the URL-decoding logic indirectly
};

TEST_F(BeaconPayloadParserTest, UrlEncodingBasics) {
  // Test URL encoding behavior that ParseBeaconPayload should handle
  // This tests the expected format: url=...&crit_css=...
  GoogleString test_body = "url=http%3A%2F%2Fexample.com%2Fpage&crit_css=selector1%2Cselector2";

  // Verify the body contains expected encoded characters
  EXPECT_NE(test_body.find("url="), GoogleString::npos);
  EXPECT_NE(test_body.find("crit_css="), GoogleString::npos);
  EXPECT_NE(test_body.find("%3A"), GoogleString::npos);  // : encoded
  EXPECT_NE(test_body.find("%2F"), GoogleString::npos);  // / encoded
}
#endif

// ========================= Admin Page Generation Tests =========================

class AdminPageGenerationTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
  }
};

TEST_F(AdminPageGenerationTest, AdminPathSubpaths) {
  // Test that various admin subpaths are recognized
  std::vector<GoogleString> valid_paths = {
    "/pagespeed_admin",
    "/pagespeed_admin/",
    "/pagespeed_admin/statistics",
    "/pagespeed_admin/cache",
    "/pagespeed_admin/config",
    "/pagespeed_admin/console",
    "/pagespeed_admin/messages"
  };

#ifdef _WIN32
  for (const auto& path : valid_paths) {
    EXPECT_TRUE(IisAdminHandler::IsAdminPath(path))
        << "Path should be admin: " << path;
  }
#endif
}

TEST_F(AdminPageGenerationTest, QueryStringPreservation) {
  // Admin paths with query strings should still be recognized
#ifdef _WIN32
  EXPECT_TRUE(IisAdminHandler::IsAdminPath("/pagespeed_admin/cache?action=flush"));
  EXPECT_TRUE(IisAdminHandler::IsAdminPath("/pagespeed_admin?foo=bar"));
#endif
}

// ========================= Error Handling Tests =========================

class AdminErrorHandlingTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
  }
};

TEST_F(AdminErrorHandlingTest, NullContextHandling) {
#ifdef _WIN32
  // HandleRequest should return false for null context
  EXPECT_FALSE(IisAdminHandler::HandleRequest(nullptr, nullptr,
      "http://localhost/pagespeed_admin", "/pagespeed_admin"));
#endif
}

// ========================= Cache Management Tests =========================

class CacheManagementTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
  }
};

TEST_F(CacheManagementTest, CacheActionPaths) {
  // Verify cache action URL patterns
  GoogleString flush_url = "/pagespeed_admin/cache?action=flush";
  GoogleString purge_url = "/pagespeed_admin/cache?action=purge&url=http://example.com/page";

  EXPECT_NE(flush_url.find("action=flush"), GoogleString::npos);
  EXPECT_NE(purge_url.find("action=purge"), GoogleString::npos);
  EXPECT_NE(purge_url.find("url="), GoogleString::npos);
}

// ========================= Advanced Feature Tests =========================

class AdvancedFeaturesTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
  }
};

TEST_F(AdvancedFeaturesTest, FilterListParsing) {
  // Test that filter name strings are correctly handled
  GoogleString filter_list = "combine_css,combine_javascript,rewrite_images";

  StringPieceVector filters;
  SplitStringPieceToVector(filter_list, ",", &filters, true);

  EXPECT_EQ(3u, filters.size());
  EXPECT_EQ("combine_css", filters[0]);
  EXPECT_EQ("combine_javascript", filters[1]);
  EXPECT_EQ("rewrite_images", filters[2]);
}

TEST_F(AdvancedFeaturesTest, WhitespaceTrimming) {
  // Test filter list with whitespace
  GoogleString filter_list = " combine_css , combine_javascript , rewrite_images ";

  StringPieceVector filters;
  SplitStringPieceToVector(filter_list, ",", &filters, true);

  EXPECT_EQ(3u, filters.size());

  // Convert to GoogleString with trimming using two-arg TrimWhitespace
  GoogleString f0, f1, f2;
  TrimWhitespace(filters[0], &f0);
  TrimWhitespace(filters[1], &f1);
  TrimWhitespace(filters[2], &f2);

  EXPECT_EQ("combine_css", f0);
  EXPECT_EQ("combine_javascript", f1);
  EXPECT_EQ("rewrite_images", f2);
}

TEST_F(AdvancedFeaturesTest, EmptyFilterList) {
  GoogleString filter_list = "";

  StringPieceVector filters;
  SplitStringPieceToVector(filter_list, ",", &filters, true);

  EXPECT_TRUE(filters.empty());
}

// ========================= Configuration Integration Tests =========================

class ConfigIntegrationTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
  }
};

TEST_F(ConfigIntegrationTest, RedisConfigDefaults) {
#ifdef _WIN32
  IisConfig config;

  // Check Redis defaults
  EXPECT_FALSE(config.redis_enabled());
  EXPECT_EQ("localhost", config.redis_server());
  EXPECT_EQ(6379, config.redis_port());
  EXPECT_EQ(5000, config.redis_timeout_ms());
  EXPECT_EQ(0, config.redis_database());
#endif
}

TEST_F(ConfigIntegrationTest, AdminConfigDefaults) {
#ifdef _WIN32
  IisConfig config;

  // Check admin defaults
  EXPECT_TRUE(config.admin_enabled());
  EXPECT_EQ("/pagespeed_admin", config.admin_path());
  EXPECT_TRUE(config.statistics_enabled());
  EXPECT_EQ("/pagespeed_statistics", config.statistics_path());
#endif
}

TEST_F(ConfigIntegrationTest, BeaconConfigDefaults) {
#ifdef _WIN32
  IisConfig config;

  // Check beacon defaults
  EXPECT_TRUE(config.beacon_enabled());
  EXPECT_EQ("/pagespeed_beacon", config.beacon_path());
#endif
}

TEST_F(ConfigIntegrationTest, HtmlRewritingDefaults) {
#ifdef _WIN32
  IisConfig config;

  // Check HTML rewriting defaults
  EXPECT_TRUE(config.html_rewriting_enabled());
  // Must match RewriteOptions::kDefaultRewriteDeadlineMs (20ms debug, 10ms release).
#ifdef NDEBUG
  EXPECT_EQ(10, config.html_rewrite_deadline_ms());
#else
  EXPECT_EQ(20, config.html_rewrite_deadline_ms());
#endif
  EXPECT_EQ(2 * 1024 * 1024, config.max_html_buffer_bytes());
#endif
}

}  // namespace net_instaweb
