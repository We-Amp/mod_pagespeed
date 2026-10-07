// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/nginx/ngx_admin_path_match.h"

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

TEST(NgxAdminPathMatchTest, ExactHandlersMatchOnlyTheExactPath) {
  EXPECT_TRUE(NgxAdminPathMatches("/ngx_pagespeed_statistics",
                                  "/ngx_pagespeed_statistics", false));
  EXPECT_FALSE(NgxAdminPathMatches("/ngx_pagespeed_statistics/",
                                   "/ngx_pagespeed_statistics", false));
  EXPECT_FALSE(NgxAdminPathMatches("/ngx_pagespeed_statistics_extra",
                                   "/ngx_pagespeed_statistics", false));
  EXPECT_FALSE(
      NgxAdminPathMatches("/other", "/ngx_pagespeed_statistics", false));
}

TEST(NgxAdminPathMatchTest, SubtreeHandlersMatchThePrefixAndEverythingUnderIt) {
  EXPECT_TRUE(
      NgxAdminPathMatches("/pagespeed_admin", "/pagespeed_admin", true));
  EXPECT_TRUE(
      NgxAdminPathMatches("/pagespeed_admin/", "/pagespeed_admin", true));
  EXPECT_TRUE(NgxAdminPathMatches("/pagespeed_admin/v1/daemon/health",
                                  "/pagespeed_admin", true));
  EXPECT_FALSE(NgxAdminPathMatches("/pagespeed", "/pagespeed_admin", true));
}

TEST(NgxAdminPathMatchTest, UnconfiguredHandlerNeverMatches) {
  EXPECT_FALSE(NgxAdminPathMatches("/", "", true));
  EXPECT_FALSE(NgxAdminPathMatches("", "", false));
}

// nginx `location` blocks are case-sensitive, so the handler lookup is too.
TEST(NgxAdminPathMatchTest, MatchingIsCaseSensitiveLikeNginxLocations) {
  EXPECT_FALSE(
      NgxAdminPathMatches("/PAGESPEED_ADMIN/config", "/pagespeed_admin", true));
  EXPECT_FALSE(
      NgxAdminPathMatches("/Pagespeed_Admin", "/pagespeed_admin", true));
  EXPECT_FALSE(NgxAdminPathMatches("/NGX_PAGESPEED_STATISTICS",
                                   "/ngx_pagespeed_statistics", false));
  EXPECT_FALSE(NgxAdminPathMatches("/Ngx_PageSpeed_Message",
                                   "/ngx_pagespeed_message", false));
  // The configured spelling itself still matches, whatever its case.
  EXPECT_TRUE(NgxAdminPathMatches("/My_Admin/x", "/My_Admin", true));
}

// A subtree handler takes the paths below its own at a `/` boundary only,
// like `location ^~ /path/`.
TEST(NgxAdminPathMatchTest, SubtreeMatchesOnlyAtASlashBoundary) {
  EXPECT_FALSE(
      NgxAdminPathMatches("/pagespeed_adminX", "/pagespeed_admin", true));
  EXPECT_FALSE(NgxAdminPathMatches("/pagespeed_admin2/config",
                                   "/pagespeed_admin", true));
  EXPECT_FALSE(
      NgxAdminPathMatches("/pagespeed_admin.css", "/pagespeed_admin", true));
  EXPECT_FALSE(NgxAdminPathMatches("/pagespeed_admin_extra/v1/sites",
                                   "/pagespeed_admin", true));
  EXPECT_TRUE(NgxAdminPathMatches("/pagespeed_admin//config",
                                  "/pagespeed_admin", true));
  // A handler path configured with a trailing slash carries its own boundary.
  EXPECT_TRUE(NgxAdminPathMatches("/admin/", "/admin/", true));
  EXPECT_TRUE(NgxAdminPathMatches("/admin/config", "/admin/", true));
  EXPECT_FALSE(NgxAdminPathMatches("/admin", "/admin/", true));
}

class NgxAdminRouteTest : public testing::Test {
 protected:
  NgxAdminRouteTest() {
    paths_.statistics = "/ngx_pagespeed_statistics";
    paths_.global_statistics = "/ngx_pagespeed_global_statistics";
    paths_.console = "/pagespeed_console";
    paths_.messages = "/ngx_pagespeed_message";
    paths_.admin = "/pagespeed_admin";
    paths_.global_admin = "/pagespeed_global_admin";
  }

  // The route for a request that was not redirected by the server.
  NgxAdminRoute Route(StringPiece server_uri, StringPiece module_path) const {
    return NgxDecideAdminRoute(server_uri, module_path, false, paths_);
  }

  // Both readings of the request path are the same.
  NgxAdminHandler Served(StringPiece path) const {
    const NgxAdminRoute route = Route(path, path);
    EXPECT_FALSE(route.declined) << path;
    return route.handler;
  }

  void ExpectLeftToServer(StringPiece server_uri, StringPiece module_path,
                          bool expect_declined) const {
    const NgxAdminRoute route = Route(server_uri, module_path);
    EXPECT_EQ(NgxAdminHandler::kNone, route.handler)
        << server_uri << " / " << module_path;
    EXPECT_EQ(expect_declined, route.declined)
        << server_uri << " / " << module_path;
  }

  NgxAdminHandlerPaths paths_;
};

TEST_F(NgxAdminRouteTest, EachHandlerIsSelectedByItsConfiguredPath) {
  EXPECT_EQ(NgxAdminHandler::kStatistics, Served("/ngx_pagespeed_statistics"));
  EXPECT_EQ(NgxAdminHandler::kGlobalStatistics,
            Served("/ngx_pagespeed_global_statistics"));
  EXPECT_EQ(NgxAdminHandler::kConsole, Served("/pagespeed_console"));
  EXPECT_EQ(NgxAdminHandler::kMessages, Served("/ngx_pagespeed_message"));
  EXPECT_EQ(NgxAdminHandler::kAdmin, Served("/pagespeed_admin"));
  EXPECT_EQ(NgxAdminHandler::kGlobalAdmin, Served("/pagespeed_global_admin"));
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/"));
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/index.html"));
}

TEST_F(NgxAdminRouteTest, AdminHandlersServeTheirSubPagesAndApiRoutes) {
  EXPECT_EQ(NgxAdminHandler::kAdmin, Served("/pagespeed_admin/"));
  EXPECT_EQ(NgxAdminHandler::kAdmin, Served("/pagespeed_admin/statistics"));
  EXPECT_EQ(NgxAdminHandler::kAdmin, Served("/pagespeed_admin/config"));
  EXPECT_EQ(NgxAdminHandler::kAdmin, Served("/pagespeed_admin/cache"));
  EXPECT_EQ(NgxAdminHandler::kAdmin,
            Served("/pagespeed_admin/message_history"));
  EXPECT_EQ(NgxAdminHandler::kAdmin,
            Served("/pagespeed_admin/v1/daemon/health"));
  EXPECT_EQ(NgxAdminHandler::kAdmin,
            Served("/pagespeed_admin/v1/sites/www.example.com/stats"));
  EXPECT_EQ(NgxAdminHandler::kGlobalAdmin,
            Served("/pagespeed_global_admin/statistics"));
  // The exact-match handlers have no sub-pages.
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/ngx_pagespeed_statistics/"));
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/pagespeed_console/x"));
  // Not below the admin path.
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/pagespeed_adminX"));
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/pagespeed_admin2/statistics"));
}

TEST_F(NgxAdminRouteTest, HandlerPathsAreCaseSensitive) {
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/PAGESPEED_ADMIN"));
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/Pagespeed_Admin/statistics"));
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/PAGESPEED_GLOBAL_ADMIN"));
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/NGX_PAGESPEED_GLOBAL_STATISTICS"));
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/Pagespeed_Console"));
}

TEST_F(NgxAdminRouteTest, MixedCaseConfiguredPathIsServedInThatSpelling) {
  paths_.admin = "/My_Admin";
  EXPECT_EQ(NgxAdminHandler::kAdmin, Served("/My_Admin"));
  EXPECT_EQ(NgxAdminHandler::kAdmin, Served("/My_Admin/config"));
  EXPECT_EQ(NgxAdminHandler::kNone, Served("/my_admin"));
}

TEST_F(NgxAdminRouteTest, UnconfiguredHandlersAreNeverSelected) {
  const NgxAdminHandlerPaths none{};
  const NgxAdminRoute route =
      NgxDecideAdminRoute("/pagespeed_admin", "/pagespeed_admin", false, none);
  EXPECT_EQ(NgxAdminHandler::kNone, route.handler);
  EXPECT_FALSE(route.declined);
  const NgxAdminRoute root = NgxDecideAdminRoute("/", "/", false, none);
  EXPECT_EQ(NgxAdminHandler::kNone, root.handler);
  EXPECT_FALSE(root.declined);
}

// The two readings of the request path differ in spelling but select the
// same handler: the request is served.
TEST_F(NgxAdminRouteTest, RequestTargetAndNormalizedUriAgreeOnTheHandler) {
  EXPECT_EQ(NgxAdminHandler::kAdmin,
            Route("/pagespeed_admin/statistics", "/pagespeed_admin//statistics")
                .handler);
  EXPECT_EQ(NgxAdminHandler::kAdmin,
            Route("/pagespeed_admin/v1/sites/example.com:8080/stats",
                  "/pagespeed_admin/v1/sites/example.com%3A8080/stats")
                .handler);
  EXPECT_EQ(NgxAdminHandler::kAdmin,
            Route("/pagespeed_admin/a b", "/pagespeed_admin/a%20b").handler);
  EXPECT_EQ(
      NgxAdminHandler::kGlobalAdmin,
      Route("/pagespeed_global_admin/", "/pagespeed_global_admin/").handler);
}

TEST_F(NgxAdminRouteTest, RequestTargetAndNormalizedUriDisagree1) {
  ExpectLeftToServer("/x\\..\\pagespeed_admin", "/pagespeed_admin", true);
}

TEST_F(NgxAdminRouteTest, RequestTargetAndNormalizedUriDisagree2) {
  ExpectLeftToServer("/static/..\\pagespeed_admin/v1/x",
                     "/pagespeed_admin/v1/x", true);
}

TEST_F(NgxAdminRouteTest, RequestTargetAndNormalizedUriDisagree3) {
  ExpectLeftToServer("/x\\..\\ngx_pagespeed_statistics",
                     "/ngx_pagespeed_statistics", true);
}

TEST_F(NgxAdminRouteTest, RequestTargetAndNormalizedUriDisagree4) {
  ExpectLeftToServer("/pagespeed\t_admin", "/pagespeed_admin", true);
  ExpectLeftToServer("/ngx_pagespeed_statistics\t", "/ngx_pagespeed_statistics",
                     true);
}

TEST_F(NgxAdminRouteTest, RequestTargetAndNormalizedUriDisagree5) {
  // The server selects a handler and the module does not.
  ExpectLeftToServer("/pagespeed_admin", "/%70agespeed_admin", true);
  ExpectLeftToServer("/pagespeed_admin", "//pagespeed_admin", true);
  ExpectLeftToServer("/ngx_pagespeed_message", "/ngx_pagespeed_messag%65",
                     true);
}

TEST_F(NgxAdminRouteTest, RequestTargetAndNormalizedUriDisagree6) {
  // Each reading selects a different handler.
  ExpectLeftToServer("/pagespeed_admin/x", "/pagespeed_global_admin/x", true);
  ExpectLeftToServer("/pagespeed_console", "/pagespeed_admin/console", true);
}

TEST_F(NgxAdminRouteTest, RequestTargetAndNormalizedUriDisagree7) {
  // The server moved the request away from, or onto, a handler path.
  ExpectLeftToServer("/index.php", "/pagespeed_admin/cache", true);
  ExpectLeftToServer("/pagespeed_admin/cache", "/other", true);
}

TEST_F(NgxAdminRouteTest, NeitherReadingSelectsAHandler) {
  ExpectLeftToServer("/a\\..\\b", "/b", false);
  ExpectLeftToServer("/index.html", "/index.html", false);
}

TEST_F(NgxAdminRouteTest, RequestsTheServerRedirectedAreLeftToTheServer) {
  const NgxAdminRoute route =
      NgxDecideAdminRoute("/pagespeed_admin/statistics",
                          "/pagespeed_admin/statistics", true, paths_);
  EXPECT_EQ(NgxAdminHandler::kNone, route.handler);
  EXPECT_TRUE(route.declined);

  const NgxAdminRoute exact = NgxDecideAdminRoute(
      "/ngx_pagespeed_statistics", "/ngx_pagespeed_statistics", true, paths_);
  EXPECT_EQ(NgxAdminHandler::kNone, exact.handler);
  EXPECT_TRUE(exact.declined);

  // Nothing to decline when no handler path is involved.
  const NgxAdminRoute other =
      NgxDecideAdminRoute("/index.html", "/index.html", true, paths_);
  EXPECT_EQ(NgxAdminHandler::kNone, other.handler);
  EXPECT_FALSE(other.declined);
}

}  // namespace
}  // namespace net_instaweb
