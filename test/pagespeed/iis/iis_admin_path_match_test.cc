// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_admin_path_match.h"

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

TEST(IisAdminPathMatchTest, RoutesExactAdminPrefix) {
  EXPECT_TRUE(IisAdminPathMatches("/pagespeed_admin", "/pagespeed_admin"));
  EXPECT_TRUE(
      IisAdminPathMatches("/pagespeed_global_admin", "/pagespeed_global_admin"));
}

TEST(IisAdminPathMatchTest, RoutesAdminPrefixFollowedBySlash) {
  EXPECT_TRUE(IisAdminPathMatches("/pagespeed_admin/", "/pagespeed_admin"));
  EXPECT_TRUE(
      IisAdminPathMatches("/pagespeed_admin/statistics", "/pagespeed_admin"));
  EXPECT_TRUE(IisAdminPathMatches("/pagespeed_admin/a/b/c", "/pagespeed_admin"));
}

TEST(IisAdminPathMatchTest, RoutesAdminPrefixFollowedByQuery) {
  EXPECT_TRUE(IisAdminPathMatches("/pagespeed_admin?x=1", "/pagespeed_admin"));
}

TEST(IisAdminPathMatchTest, RoutesOnlyExactAdminPrefix) {
  EXPECT_FALSE(IisAdminPathMatches("/pagespeed_admin2", "/pagespeed_admin"));
  EXPECT_FALSE(IisAdminPathMatches("/pagespeed_admin_x", "/pagespeed_admin"));
  EXPECT_FALSE(IisAdminPathMatches("/pagespeed_administrator/x",
                                   "/pagespeed_admin"));
  EXPECT_FALSE(IisAdminPathMatches("/pagespeed_admin.js", "/pagespeed_admin"));
  EXPECT_FALSE(IisAdminPathMatches("/pagespeed_admin%2fx", "/pagespeed_admin"));
  EXPECT_FALSE(IisAdminPathMatches("/pagespeed_global_admin2",
                                   "/pagespeed_global_admin"));
}

TEST(IisAdminPathMatchTest, DoesNotRouteShorterOrUnrelatedPaths) {
  EXPECT_FALSE(IisAdminPathMatches("/pagespeed", "/pagespeed_admin"));
  EXPECT_FALSE(IisAdminPathMatches("/", "/pagespeed_admin"));
  EXPECT_FALSE(IisAdminPathMatches("", "/pagespeed_admin"));
  EXPECT_FALSE(IisAdminPathMatches("/other/pagespeed_admin", "/pagespeed_admin"));
}

TEST(IisAdminPathMatchTest, MatchesCaseInsensitively) {
  EXPECT_TRUE(IisAdminPathMatches("/PageSpeed_Admin", "/pagespeed_admin"));
  EXPECT_TRUE(IisAdminPathMatches("/PAGESPEED_ADMIN/statistics",
                                  "/pagespeed_admin"));
  EXPECT_TRUE(IisAdminPathMatches("/pagespeed_admin?x=1", "/PAGESPEED_ADMIN"));
  EXPECT_FALSE(IisAdminPathMatches("/PageSpeed_Admin2", "/pagespeed_admin"));
  EXPECT_FALSE(IisAdminPathMatches("/PAGESPEED_ADMINX/y", "/pagespeed_admin"));
}

TEST(IisAdminPathMatchTest, UnconfiguredHandlerNeverMatches) {
  EXPECT_FALSE(IisAdminPathMatches("/pagespeed_admin", ""));
  EXPECT_FALSE(IisAdminPathMatches("/", ""));
  EXPECT_FALSE(IisAdminPathMatches("", ""));
}

TEST(IisAdminPathMatchTest, HandlerPathWithTrailingSlashMatchesBelowIt) {
  EXPECT_TRUE(IisAdminPathMatches("/admin/", "/admin/"));
  EXPECT_TRUE(IisAdminPathMatches("/admin/x", "/admin/"));
  EXPECT_FALSE(IisAdminPathMatches("/admin", "/admin/"));
  EXPECT_FALSE(IisAdminPathMatches("/admin2/x", "/admin/"));
}

}  // namespace
}  // namespace net_instaweb
