// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/llms_txt_builder.h"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

class LlmsTxtBuilderTest : public testing::Test {
 protected:
  std::map<std::string, LlmsTxtFetchResult> responses_;
  std::map<std::string, std::string> variants_;
  std::vector<std::string> fetched_;

  LlmsTxtBuilder MakeBuilder() {
    auto fetch = [this](const std::string& url) -> LlmsTxtFetchResult {
      fetched_.push_back(url);
      auto it = responses_.find(url);
      return it == responses_.end() ? LlmsTxtFetchResult{} : it->second;
    };
    auto reader = [this](const std::string& url) -> std::optional<std::string> {
      auto it = variants_.find(url);
      if (it == variants_.end()) return std::nullopt;
      return it->second;
    };
    return LlmsTxtBuilder(fetch, reader);
  }

  static LlmsTxtFetchResult Ok(std::string body, std::string xrobots = "",
                               std::string gext = "") {
    return LlmsTxtFetchResult{true, 200, std::move(body), std::move(xrobots),
                              std::move(gext)};
  }

  LlmsTxtBuildOptions Opts() {
    LlmsTxtBuildOptions o;
    o.scheme = "https";
    o.host = "example.com";
    o.respect_ai_directives = false;  // most tests skip the robots fetch
    return o;
  }

  bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
  }
};

TEST_F(LlmsTxtBuilderTest, FiltersBySitemapAndAllowPaths) {
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset>"
         "<url><loc>https://example.com/docs/a</loc></url>"
         "<url><loc>https://example.com/blog/b</loc></url>"
         "<url><loc>https://other.com/x</loc></url>"
         "</urlset>");
  variants_["https://example.com/docs/a"] = "# Doc A\n\nAbout doc A.";
  LlmsTxtBuildOptions o = Opts();
  o.allow_paths = {"/docs"};

  LlmsTxtBuildResult r = MakeBuilder().Build(o);
  EXPECT_EQ(r.status, LlmsTxtBuildStatus::kOk);
  EXPECT_TRUE(Contains(r.llms_txt, "https://example.com/docs/a"));
  EXPECT_FALSE(Contains(r.llms_txt, "/blog/b"));
  EXPECT_FALSE(Contains(r.llms_txt, "other.com"));
}

TEST_F(LlmsTxtBuilderTest, SummaryFromExistingVariant) {
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset><url><loc>https://example.com/p</loc></url></urlset>");
  variants_["https://example.com/p"] = "# Page Title\n\nThe first paragraph.";
  LlmsTxtBuildResult r = MakeBuilder().Build(Opts());
  EXPECT_TRUE(
      Contains(r.llms_txt,
               "- [Page Title](https://example.com/p): The first paragraph."));
  // A variant was available, so no per-page HTTP fetch occurred.
  for (const std::string& u : fetched_) EXPECT_NE(u, "https://example.com/p");
}

TEST_F(LlmsTxtBuilderTest, SummaryFromHtmlFetchWhenNoVariant) {
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset><url><loc>https://example.com/p</loc></url></urlset>");
  responses_["https://example.com/p"] =
      Ok("<html><head><title>Fetched Title</title>"
         "<meta name=description content=\"Fetched summary\"></head></html>");
  LlmsTxtBuildResult r = MakeBuilder().Build(Opts());
  EXPECT_TRUE(Contains(
      r.llms_txt, "- [Fetched Title](https://example.com/p): Fetched summary"));
}

TEST_F(LlmsTxtBuilderTest, FetchCapBoundsPerPageFetches) {
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset>"
         "<url><loc>https://example.com/one</loc></url>"
         "<url><loc>https://example.com/two</loc></url>"
         "</urlset>");
  responses_["https://example.com/one"] = Ok("<head><title>One</title></head>");
  responses_["https://example.com/two"] = Ok("<head><title>Two</title></head>");
  LlmsTxtBuildOptions o = Opts();
  o.summary_fetch_cap = 1;
  LlmsTxtBuildResult r = MakeBuilder().Build(o);

  int page_fetches = 0;
  for (const std::string& u : fetched_) {
    if (u == "https://example.com/one" || u == "https://example.com/two") {
      ++page_fetches;
    }
  }
  EXPECT_EQ(page_fetches, 1);
  // Both pages still appear (the uncapped one gets a path-derived title).
  EXPECT_TRUE(Contains(r.llms_txt, "https://example.com/one"));
  EXPECT_TRUE(Contains(r.llms_txt, "https://example.com/two"));
}

TEST_F(LlmsTxtBuilderTest, StubOnlySkipsAllPerPageFetches) {
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset>"
         "<url><loc>https://example.com/getting-started.html</loc></url>"
         "</urlset>");
  responses_["https://example.com/getting-started.html"] =
      Ok("<head><title>Should Not Be Used</title></head>");
  LlmsTxtBuildOptions o = Opts();
  o.stub_only = true;
  LlmsTxtBuildResult r = MakeBuilder().Build(o);
  EXPECT_EQ(r.status, LlmsTxtBuildStatus::kOk);
  // Only the sitemap was fetched.
  EXPECT_EQ(fetched_.size(), 1u);
  EXPECT_EQ(fetched_[0], "https://example.com/sitemap.xml");
  // Title derived from the path; no summary.
  EXPECT_TRUE(Contains(r.llms_txt, "Getting Started"));
  EXPECT_FALSE(Contains(r.llms_txt, "Should Not Be Used"));
}

TEST_F(LlmsTxtBuilderTest, OffOriginLocsAreNeverFetched) {
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset><url><loc>https://evil.example/x</loc></url></urlset>");
  responses_["https://evil.example/x"] = Ok("<head><title>Evil</title></head>");
  LlmsTxtBuildResult r = MakeBuilder().Build(Opts());
  for (const std::string& u : fetched_) {
    EXPECT_EQ(u.find("evil.example"), std::string::npos);
  }
  EXPECT_EQ(r.status, LlmsTxtBuildStatus::kNoSource);  // nothing own-origin
}

TEST_F(LlmsTxtBuilderTest, SitemapHashIsStableAndContentDependent) {
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset><url><loc>https://example.com/p</loc></url></urlset>");
  std::array<std::byte, 32> h1 = MakeBuilder().Build(Opts()).sitemap_hash;

  fetched_.clear();
  std::array<std::byte, 32> h2 = MakeBuilder().Build(Opts()).sitemap_hash;
  EXPECT_EQ(h1, h2);  // same bytes -> same hash

  std::array<std::byte, 32> zero{};
  EXPECT_NE(h1, zero);  // a real sitemap yields a non-zero hash

  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset><url><loc>https://example.com/q</loc></url></urlset>");
  std::array<std::byte, 32> h3 = MakeBuilder().Build(Opts()).sitemap_hash;
  EXPECT_NE(h1, h3);  // different bytes -> different hash
}

TEST_F(LlmsTxtBuilderTest, AiBlockedRobotsSuppresses) {
  responses_["https://example.com/robots.txt"] =
      Ok("User-agent: *\nDisallow: /");
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset><url><loc>https://example.com/p</loc></url></urlset>");
  LlmsTxtBuildOptions o = Opts();
  o.respect_ai_directives = true;
  LlmsTxtBuildResult r = MakeBuilder().Build(o);
  EXPECT_EQ(r.status, LlmsTxtBuildStatus::kAiBlocked);
  EXPECT_TRUE(r.llms_txt.empty());
}

TEST_F(LlmsTxtBuilderTest, PerPageNoaiExcludesThatPage) {
  responses_["https://example.com/robots.txt"] = Ok("User-agent: *\nAllow: /");
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset>"
         "<url><loc>https://example.com/keep</loc></url>"
         "<url><loc>https://example.com/hide</loc></url>"
         "</urlset>");
  responses_["https://example.com/keep"] =
      Ok("<head><title>Keep</title></head>");
  responses_["https://example.com/hide"] =
      Ok("<head><title>Hide</title></head>", /*xrobots=*/"noai");
  LlmsTxtBuildOptions o = Opts();
  o.respect_ai_directives = true;
  LlmsTxtBuildResult r = MakeBuilder().Build(o);
  EXPECT_TRUE(Contains(r.llms_txt, "https://example.com/keep"));
  EXPECT_FALSE(Contains(r.llms_txt, "https://example.com/hide"));
}

TEST_F(LlmsTxtBuilderTest, VariantPageWithNoaiExcludedWhenRespecting) {
  // A page with an EXISTING rendered variant that later adds noai must still be
  // excluded — the directive check must not be skipped just because a variant
  // supplied the summary.
  responses_["https://example.com/robots.txt"] = Ok("User-agent: *\nAllow: /");
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset><url><loc>https://example.com/p</loc></url></urlset>");
  variants_["https://example.com/p"] = "# P\n\nfrom the rendered variant";
  responses_["https://example.com/p"] =
      Ok("<head><title>P</title></head>", /*xrobots=*/"noai");
  LlmsTxtBuildOptions o = Opts();
  o.respect_ai_directives = true;
  LlmsTxtBuildResult r = MakeBuilder().Build(o);
  EXPECT_FALSE(Contains(r.llms_txt, "https://example.com/p"));
}

TEST_F(LlmsTxtBuilderTest, CappedPagesExcludedWhenRespectingDirectives) {
  // With directives on, a page we cannot fetch-and-verify (beyond the cap) is
  // omitted (fail-closed) rather than indexed unchecked.
  responses_["https://example.com/robots.txt"] = Ok("User-agent: *\nAllow: /");
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset>"
         "<url><loc>https://example.com/one</loc></url>"
         "<url><loc>https://example.com/two</loc></url>"
         "</urlset>");
  responses_["https://example.com/one"] = Ok("<head><title>One</title></head>");
  responses_["https://example.com/two"] = Ok("<head><title>Two</title></head>");
  LlmsTxtBuildOptions o = Opts();
  o.respect_ai_directives = true;
  o.summary_fetch_cap = 1;
  LlmsTxtBuildResult r = MakeBuilder().Build(o);
  EXPECT_TRUE(Contains(r.llms_txt, "https://example.com/one"));
  EXPECT_FALSE(Contains(r.llms_txt, "https://example.com/two"));
}

TEST_F(LlmsTxtBuilderTest, ExplicitDefaultPortLocIsOwnOrigin) {
  // A same-origin <loc> written with an explicit default port must NOT be
  // dropped by the own-origin filter (default ports normalize away).
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset><url><loc>https://example.com:443/p</loc></url></urlset>");
  variants_["https://example.com:443/p"] = "# P\n\nsummary";
  LlmsTxtBuildResult r = MakeBuilder().Build(Opts());
  EXPECT_EQ(r.status, LlmsTxtBuildStatus::kOk);
  EXPECT_TRUE(Contains(r.llms_txt, "https://example.com:443/p"));
}

TEST_F(LlmsTxtBuilderTest, FallbackToAllowPathsWhenNoSitemap) {
  // No sitemap response -> fetch fails. Concrete allow-paths become the pages.
  LlmsTxtBuildOptions o = Opts();
  o.allow_paths = {"/about", "/contact", "/blog/*"};
  variants_["https://example.com/about"] = "# About\n\nAbout us.";
  variants_["https://example.com/contact"] = "# Contact\n\nReach us.";
  LlmsTxtBuildResult r = MakeBuilder().Build(o);
  EXPECT_EQ(r.status, LlmsTxtBuildStatus::kOk);
  EXPECT_TRUE(Contains(r.llms_txt, "https://example.com/about"));
  EXPECT_TRUE(Contains(r.llms_txt, "https://example.com/contact"));
  EXPECT_FALSE(Contains(r.llms_txt, "blog/*"));  // wildcard entry is not a page
  std::array<std::byte, 32> zero{};
  EXPECT_EQ(r.sitemap_hash, zero);  // no sitemap -> zero hash
}

TEST_F(LlmsTxtBuilderTest, NoSourceWhenNothingReachable) {
  LlmsTxtBuildResult r = MakeBuilder().Build(Opts());
  EXPECT_EQ(r.status, LlmsTxtBuildStatus::kNoSource);
}

TEST_F(LlmsTxtBuilderTest, NestedSitemapIndexFanout) {
  responses_["https://example.com/sitemap.xml"] =
      Ok("<sitemapindex><sitemap>"
         "<loc>https://example.com/sitemap-pages.xml</loc>"
         "</sitemap></sitemapindex>");
  responses_["https://example.com/sitemap-pages.xml"] =
      Ok("<urlset><url><loc>https://example.com/deep</loc></url></urlset>");
  variants_["https://example.com/deep"] = "# Deep\n\nNested page.";
  LlmsTxtBuildResult r = MakeBuilder().Build(Opts());
  EXPECT_EQ(r.status, LlmsTxtBuildStatus::kOk);
  EXPECT_TRUE(Contains(r.llms_txt, "https://example.com/deep"));
}

TEST_F(LlmsTxtBuilderTest, SiteTitleIsHost) {
  responses_["https://example.com/sitemap.xml"] =
      Ok("<urlset><url><loc>https://example.com/p</loc></url></urlset>");
  variants_["https://example.com/p"] = "# P\n\nsummary";
  LlmsTxtBuildResult r = MakeBuilder().Build(Opts());
  EXPECT_TRUE(Contains(r.llms_txt, "# example.com\n"));
}

}  // namespace
}  // namespace pagespeed
