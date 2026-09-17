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

#include "net/instaweb/rewriter/public/insert_dns_prefetch_filter.h"

#include <memory>

#include "base/logging.h"
#include "net/instaweb/http/public/logging_proto_impl.h"
#include "net/instaweb/rewriter/flush_early.pb.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/opt/logging/enums.pb.h"
#include "pagespeed/opt/logging/log_record.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/user_agent_matcher_test_base.h"

// TODO(bharathbhushan): Test interaction with the flush early flow and related
// filters.
// TODO(bharathbhushan): Have a test to ensure that this is the last post render
// filter.
// TODO(bharathbhushan): Add a test for noscript.

namespace net_instaweb {

class InsertDnsPrefetchFilterTest : public RewriteTestBase {
 public:
  InsertDnsPrefetchFilterTest() : writer_(&output_) {}

 protected:
  void SetUp() override {
    options()->set_support_noscript_enabled(false);
    options()->EnableFilter(RewriteOptions::kInsertDnsPrefetch);
    RewriteTestBase::SetUp();
    rewrite_driver()->AddFilters();
    rewrite_driver()->SetWriter(&writer_);
    SetCurrentUserAgent(UserAgentMatcherTestBase::kChromeUserAgent);
  }

  void TearDown() override {
    filter_.reset(nullptr);
    RewriteTestBase::TearDown();
  }

  void CheckPrefetchInfo(int num_domains_in_current_rewrite,
                         int num_domains_in_previous_rewrite,
                         int num_domains_to_store,
                         const GoogleString& stored_domains_str) {
    StringPieceVector stored_domains;
    SplitStringPieceToVector(stored_domains_str, ",", &stored_domains, true);
    ASSERT_EQ(num_domains_to_store, stored_domains.size());
    FlushEarlyInfo* info = rewrite_driver()->flush_early_info();
    EXPECT_EQ(num_domains_in_current_rewrite,
              info->total_dns_prefetch_domains());
    EXPECT_EQ(num_domains_in_previous_rewrite,
              info->total_dns_prefetch_domains_previous());
    EXPECT_EQ(num_domains_to_store, info->dns_prefetch_domains_size());
    for (int i = 0; i < info->dns_prefetch_domains_size(); ++i) {
      EXPECT_EQ(stored_domains[i], info->dns_prefetch_domains(i));
    }
  }

  void CheckLogStatus(RewriterHtmlApplication::Status html_status) {
    rewrite_driver_->log_record()->WriteLog();
    for (int i = 0; i < logging_info()->rewriter_stats_size(); i++) {
      if (logging_info()->rewriter_stats(i).id() ==
              RewriteOptions::FilterId(RewriteOptions::kInsertDnsPrefetch) &&
          logging_info()->rewriter_stats(i).has_html_status()) {
        EXPECT_EQ(html_status, logging_info()->rewriter_stats(i).html_status());
        return;
      }
    }
    FAIL();
  }

  void CheckLogStatus(RewriterHtmlApplication::Status html_status,
                      RewriterApplication::Status app_status,
                      int app_status_count) {
    rewrite_driver_->log_record()->WriteLog();
    for (int i = 0; i < logging_info()->rewriter_stats_size(); i++) {
      if (logging_info()->rewriter_stats(i).id() ==
              RewriteOptions::FilterId(RewriteOptions::kInsertDnsPrefetch) &&
          logging_info()->rewriter_stats(i).has_html_status()) {
        EXPECT_EQ(html_status, logging_info()->rewriter_stats(i).html_status());
        const RewriteStatusCount& count_applied =
            logging_info()->rewriter_stats(i).status_counts(0);
        EXPECT_EQ(app_status, count_applied.application_status());
        EXPECT_EQ(app_status_count, count_applied.count());
        return;
      }
    }
    FAIL();
  }

  GoogleString CreateHtml(int num_scripts) {
    GoogleString html = "<head><script></script></head><body>";
    for (int i = 1; i <= num_scripts; ++i) {
      StrAppend(&html, "<script src=\"http://", IntegerToString(i),
                ".com/\"/>");
    }
    StrAppend(&html, "</body>");
    return html;
  }

  GoogleString CreateHtmlWithHintTags(int num_scripts, int num_tags,
                                      int num_preconnect) {
    GoogleString html = "<head><script></script>";
    for (int i = 1; i <= num_tags; ++i) {
      // Preconnect hints carry the resource's scheme (http here, matching the
      // script URLs produced by CreateHtml); dns-prefetch hints stay
      // scheme-relative.
      StrAppend(&html, "<link rel=\"",
                i <= num_preconnect ? "preconnect" : "dns-prefetch",
                "\" href=\"", i <= num_preconnect ? "http://" : "//",
                IntegerToString(i), ".com\">");
    }
    StrAppend(&html, "</head><body>");
    for (int i = 1; i <= num_scripts; ++i) {
      StrAppend(&html, "<script src=\"http://", IntegerToString(i),
                ".com/\"/>");
    }
    html += "</body>";
    return html;
  }

  GoogleString CreateDomainsVector(int num_domains) {
    GoogleString result;
    for (int i = 1; i <= num_domains; ++i) {
      StrAppend(&result, "http://", IntegerToString(i), ".com,");
    }
    LOG(INFO) << "CreateDomainsVector: " << result;
    return result;
  }

  GoogleString output_;

 private:
  StringWriter writer_;
  ResponseHeaders headers_;
  std::unique_ptr<InsertDnsPrefetchFilter> filter_;

  InsertDnsPrefetchFilterTest(const InsertDnsPrefetchFilterTest&) = delete;
  InsertDnsPrefetchFilterTest& operator=(const InsertDnsPrefetchFilterTest&) =
      delete;
};

TEST_F(InsertDnsPrefetchFilterTest, IgnoreDomainsInHead) {
  GoogleString html =
      "<head>"
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"http://a.com/\">"
      "<script src=\"http://b.com/\"/>"
      "<link rel=\"dns-prefetch\" href=\"http://c.com\">"
      "</head><body></body>";
  Parse("ignore_domains_in_head", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  CheckPrefetchInfo(0, 0, 0, "");
}

TEST_F(InsertDnsPrefetchFilterTest, StoreDomainsInBody) {
  GoogleString html =
      "<head></head>"
      "<body>"
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"http://a.com/\">"
      "<script src=\"http://b.com/\"/>"
      "<img src=\"http://c.com/\"/>"
      "</body>";
  Parse("store_domains_in_body", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  CheckPrefetchInfo(3, 0, 3, "http://a.com,http://b.com,http://c.com");
}

TEST_F(InsertDnsPrefetchFilterTest, IgnoreCurrentDomain) {
  GoogleString html = StrCat(
      "<head></head>"
      "<body>"
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"",
      AbsolutifyUrl("style.css"),
      "\">"
      "<script src=\"",
      AbsolutifyUrl("script.js"),
      "\">"
      "<img src=\"",
      AbsolutifyUrl("img.src"),
      "\">"
      "</body>");
  Parse("ignore_current_domain", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  CheckPrefetchInfo(0, 0, 0, "");
}

TEST_F(InsertDnsPrefetchFilterTest,
       DisableInsertDnsPrefetchForUserAgentsNotSupported) {
  SetCurrentUserAgent("");
  GoogleString html =
      "<head></head>"
      "<body>"
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"http://a.com/\">"
      "<script src=\"http://b.com/\"/>"
      "<img src=\"http://c.com/\"/>"
      "</body>";
  Parse("store_domains_in_body", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  CheckPrefetchInfo(0, 0, 0, "");
  CheckLogStatus(RewriterHtmlApplication::USER_AGENT_NOT_SUPPORTED);
}

TEST_F(InsertDnsPrefetchFilterTest, StoreDomainsOnlyInBody) {
  GoogleString html =
      "<head>"
      "<script src=\"http://b.com/\"/>"
      "</head>"
      "<body>"
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"http://a.com/\">"
      "<script src=\"http://b.com/\"/>"
      "<img src=\"http://c.com/\"/>"
      "</body>";
  Parse("store_domains_in_body", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  // b.com is not stored since it is already in HEAD.
  CheckPrefetchInfo(2, 0, 2, "http://a.com,http://c.com");
}

TEST_F(InsertDnsPrefetchFilterTest, StoreDomainsInBodyMax) {
  GoogleString html(CreateHtml(10));
  Parse("store_domains_in_body_max", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  // Only 8/10 domains get stored.
  CheckPrefetchInfo(10, 0, 8, CreateDomainsVector(8));
}

// TODO(bharathbhushan): Add tests for all the html tags which can have URI
// attributes.
TEST_F(InsertDnsPrefetchFilterTest, LinkTagTest) {
  GoogleString html =
      "<head>"
      "<script></script>"
      "<link rel=\"alternate\" href=\"http://a.com\">"
      "<link rel=\"author\" href=\"http://b.com\">"
      "<link rel=\"dns-prefetch\" href=\"http://c.com\">"
      "<link rel=\"help\" href=\"http://d.com\">"
      "<link rel=\"icon\" href=\"http://e.com\">"
      "<link rel=\"license\" href=\"http://f.com\">"
      "<link rel=\"next\" href=\"http://g.com\">"
      "<link rel=\"prefetch\" href=\"http://h.com\">"
      "<link rel=\"prev\" href=\"http://i.com\">"
      "<link rel=\"search\" href=\"http://j.com\">"
      "<link rel=\"stylesheet\" href=\"http://k.com\">"
      "</head>"
      "<body>"
      "<script src=\"http://a.com/\"/>"
      "<script src=\"http://b.com/\"/>"
      "<script src=\"http://c.com/\"/>"
      "<script src=\"http://d.com/\"/>"
      "<script src=\"http://e.com/\"/>"
      "<script src=\"http://f.com/\"/>"
      "<script src=\"http://g.com/\"/>"
      "<script src=\"http://h.com/\"/>"
      "<script src=\"http://i.com/\"/>"
      "<script src=\"http://j.com/\"/>"
      "<script src=\"http://k.com/\"/>"
      "</body>";
  Parse("test_different_link_tags", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  // The following link types are for resources or relevant to DNS prefetch
  // tags: dns-prefetch, icon, prefetch, stylesheet. The domains in those tags
  // are not stored. The rest of link types have hyperlinks and their domains
  // get stored.
  CheckPrefetchInfo(7, 0, 7,
                    "http://a.com,http://b.com,http://d.com,http://f.com,"
                    "http://g.com,http://i.com,http://j.com");
}

TEST_F(InsertDnsPrefetchFilterTest, FullFlowTest) {
  GoogleString html_input = CreateHtml(10);
  Parse("store_8_of_10", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  CheckPrefetchInfo(10, 0, 8, CreateDomainsVector(8));
  output_.clear();

  html_input = CreateHtml(9);
  Parse("store_8_of_9", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  CheckPrefetchInfo(9, 10, 8, CreateDomainsVector(8));
  output_.clear();

  html_input = CreateHtml(6);
  // 8 connection warm-up hint tags (2 preconnect + 6 dns-prefetch) inserted
  // since the difference in the number of domains in the last two rewrites
  // (10, 9) is <= 2 and we had stored 8 domains in the previous rewrite. This
  // is the common case.
  // In this rewrite we have an unstable response, whose effect shows up in the
  // next rewrite.
  GoogleString html_output = CreateHtmlWithHintTags(6, 8, 2);
  Parse("stable_domain_list_so_insert_tags", html_input);
  EXPECT_EQ(AddHtmlBody(html_output), output_);
  CheckPrefetchInfo(6, 9, 6, CreateDomainsVector(6));
  output_.clear();
  CheckLogStatus(RewriterHtmlApplication::ACTIVE,
                 RewriterApplication::APPLIED_OK, 8);

  // Since the last response caused instability in the domain list, we don't
  // insert any prefetch tags in this rewrite.
  Parse("after_unstable_response", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  CheckPrefetchInfo(6, 6, 6, CreateDomainsVector(6));
  output_.clear();
}

TEST_F(InsertDnsPrefetchFilterTest, PreconnectCappedAtLimit) {
  GoogleString html_input = CreateHtml(3);
  // First rewrite: the stored domain list is still empty, so no tags are
  // inserted even though the (trivially stable) counts match.
  Parse("store_3_domains", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  CheckPrefetchInfo(3, 0, 3, CreateDomainsVector(3));
  output_.clear();

  // Second rewrite: |3 - 0| > kMaxDomainDiff, so the list is not yet stable.
  Parse("domain_list_not_yet_stable", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  CheckPrefetchInfo(3, 3, 3, CreateDomainsVector(3));
  output_.clear();

  // Third rewrite: the list is stable. The first kMaxPreconnectTags (2)
  // domains get rel="preconnect", the remaining domain gets
  // rel="dns-prefetch".
  GoogleString html_output = CreateHtmlWithHintTags(3, 3, 2);
  Parse("stable_list_preconnect_capped", html_input);
  EXPECT_EQ(AddHtmlBody(html_output), output_);
  CheckPrefetchInfo(3, 3, 3, CreateDomainsVector(3));
  output_.clear();
  CheckLogStatus(RewriterHtmlApplication::ACTIVE,
                 RewriterApplication::APPLIED_OK, 3);
}

TEST_F(InsertDnsPrefetchFilterTest, AllPreconnectWhenListSmall) {
  GoogleString html_input = CreateHtml(2);
  // First rewrite: the counts (0, 0) are trivially stable, but the stored
  // domain list is empty (it is written at the end of this rewrite), so no
  // tags are inserted.
  Parse("store_2_domains", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  CheckPrefetchInfo(2, 0, 2, CreateDomainsVector(2));
  output_.clear();

  // Second rewrite: |2 - 0| <= kMaxDomainDiff, so the list is already stable
  // and both stored domains fit within the preconnect budget: two
  // rel="preconnect" tags and no rel="dns-prefetch" tags.
  GoogleString html_output = CreateHtmlWithHintTags(2, 2, 2);
  Parse("stable_small_list_all_preconnect", html_input);
  EXPECT_EQ(AddHtmlBody(html_output), output_);
  CheckPrefetchInfo(2, 2, 2, CreateDomainsVector(2));
  output_.clear();
  CheckLogStatus(RewriterHtmlApplication::ACTIVE,
                 RewriterApplication::APPLIED_OK, 2);
}

TEST_F(InsertDnsPrefetchFilterTest, PreconnectCarriesResourceScheme) {
  // The page is served over http, but secure.com's resource URL is explicitly
  // https. A preconnect hint opens a scheme-specific connection (TCP, plus
  // TLS for https), so it must carry the resource's own scheme: a
  // scheme-relative hint would resolve against the page scheme and warm the
  // wrong connection. The dns-prefetch hint is scheme-agnostic and stays
  // scheme-relative.
  GoogleString html_input =
      "<head></head>"
      "<body>"
      "<script src=\"https://secure.com/script.js\"/>"
      "<img src=\"http://img.com/i.png\"/>"
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"http://css.com/s.css\">"
      "</body>";
  Parse("store_3_domains", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  CheckPrefetchInfo(3, 0, 3,
                    "https://secure.com,http://img.com,http://css.com");
  output_.clear();

  // |3 - 0| > kMaxDomainDiff, so the list is not yet stable.
  Parse("domain_list_not_yet_stable", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  output_.clear();

  GoogleString html_output =
      "<head>"
      "<link rel=\"preconnect\" href=\"https://secure.com\">"
      "<link rel=\"preconnect\" href=\"http://img.com\">"
      "<link rel=\"dns-prefetch\" href=\"//css.com\">"
      "</head>"
      "<body>"
      "<script src=\"https://secure.com/script.js\"/>"
      "<img src=\"http://img.com/i.png\"/>"
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"http://css.com/s.css\">"
      "</body>";
  Parse("stable_list_scheme_aware_preconnect", html_input);
  EXPECT_EQ(AddHtmlBody(html_output), output_);
  CheckPrefetchInfo(3, 3, 3,
                    "https://secure.com,http://img.com,http://css.com");
  output_.clear();
}

TEST_F(InsertDnsPrefetchFilterTest, SchemeRelativeResourceUsesPageScheme) {
  // A scheme-relative resource URL has no scheme of its own, so the emitted
  // preconnect hint falls back to the page (request) scheme: http here.
  GoogleString html_input =
      "<head></head>"
      "<body>"
      "<script src=\"//cdn.com/script.js\"/>"
      "</body>";
  Parse("store_scheme_relative_domain", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  CheckPrefetchInfo(1, 0, 1, "http://cdn.com");
  output_.clear();

  GoogleString html_output =
      "<head>"
      "<link rel=\"preconnect\" href=\"http://cdn.com\">"
      "</head>"
      "<body>"
      "<script src=\"//cdn.com/script.js\"/>"
      "</body>";
  Parse("stable_list_page_scheme_fallback", html_input);
  EXPECT_EQ(AddHtmlBody(html_output), output_);
  CheckPrefetchInfo(1, 1, 1, "http://cdn.com");
  output_.clear();
}

TEST_F(InsertDnsPrefetchFilterTest, PreconnectPreservesNonDefaultPort) {
  // A preconnect hint warms an origin (scheme, host, port), so a resource on
  // a non-default port must keep its port in the stored origin: dropping it
  // would warm the default port, a connection the browser cannot reuse. A
  // default port carries no information and stays off the stored origin.
  GoogleString html_input =
      "<head></head>"
      "<body>"
      "<script src=\"https://cdn.com:8443/script.js\"/>"
      "<img src=\"http://img.com:80/i.png\"/>"
      "</body>";
  Parse("store_port_domains", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  CheckPrefetchInfo(2, 0, 2, "https://cdn.com:8443,http://img.com");
  output_.clear();

  GoogleString html_output =
      "<head>"
      "<link rel=\"preconnect\" href=\"https://cdn.com:8443\">"
      "<link rel=\"preconnect\" href=\"http://img.com\">"
      "</head>"
      "<body>"
      "<script src=\"https://cdn.com:8443/script.js\"/>"
      "<img src=\"http://img.com:80/i.png\"/>"
      "</body>";
  Parse("stable_list_port_aware_preconnect", html_input);
  EXPECT_EQ(AddHtmlBody(html_output), output_);
  CheckPrefetchInfo(2, 2, 2, "https://cdn.com:8443,http://img.com");
  output_.clear();
}

TEST_F(InsertDnsPrefetchFilterTest, LegacyBareHostEntryEmittedSchemeRelative) {
  // Domain lists written by older revisions of this filter hold bare hosts
  // with no scheme. Seed one directly: the counts (0, 0) are trivially
  // stable, so the entry is consumed, and the emitted preconnect hint must
  // keep the legacy scheme-relative form.
  rewrite_driver()->flush_early_info()->add_dns_prefetch_domains("a.com");
  GoogleString html_input = "<head></head><body></body>";
  GoogleString html_output =
      "<head>"
      "<link rel=\"preconnect\" href=\"//a.com\">"
      "</head><body></body>";
  Parse("legacy_bare_host_entry", html_input);
  EXPECT_EQ(AddHtmlBody(html_output), output_);
  // The page itself references no domains, so nothing is stored for the next
  // rewrite.
  CheckPrefetchInfo(0, 0, 0, "");
  output_.clear();
}

TEST_F(InsertDnsPrefetchFilterTest, OriginPreconnectInHeadIgnored) {
  GoogleString html =
      "<head>"
      "<link rel=\"preconnect\" href=\"//b.com\">"
      "</head>"
      "<body>"
      "<script src=\"http://a.com/\"/>"
      "<script src=\"http://b.com/\"/>"
      "</body>";
  Parse("origin_preconnect_in_head_ignored", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  // b.com already has an origin-supplied preconnect hint in HEAD, so only
  // a.com is stored.
  CheckPrefetchInfo(1, 0, 1, "http://a.com");
}

TEST_F(InsertDnsPrefetchFilterTest, MultiTokenRelHintInHeadIgnored) {
  GoogleString html =
      "<head>"
      "<link rel=\"preconnect dns-prefetch\" href=\"//cdn.x.com\">"
      "</head>"
      "<body>"
      "<script src=\"http://a.com/\"/>"
      "<script src=\"http://cdn.x.com/\"/>"
      "</body>";
  Parse("multi_token_rel_hint_in_head_ignored", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  // cdn.x.com has an origin-supplied connection warm-up hint in HEAD, spelled
  // with a multi-token rel value. The ResourceTagScanner tokenizes rel on
  // spaces and categorizes the link as kPrefetch, so the filter must recognize
  // it too and not store (and later duplicate) the author-hinted domain.
  CheckPrefetchInfo(1, 0, 1, "http://a.com");
}

TEST_F(InsertDnsPrefetchFilterTest, CaseVariantRelTokenInHeadIgnored) {
  GoogleString html =
      "<head>"
      "<link rel=\"PreConnect DNS-PREFETCH\" href=\"//b.com\">"
      "</head>"
      "<body>"
      "<script src=\"http://a.com/\"/>"
      "<script src=\"http://b.com/\"/>"
      "</body>";
  Parse("case_variant_rel_token_in_head_ignored", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  // Rel tokens match case-insensitively, so b.com must be ignored even though
  // the author's hint is not lower-case.
  CheckPrefetchInfo(1, 0, 1, "http://a.com");
}

TEST_F(InsertDnsPrefetchFilterTest,
       FlushSubresourcesDeprecatedIgnoresOriginHintDomains) {
  // flush_subresources is a deprecated no-op: enabling it must not change
  // which domains get dns-prefetch hints. In particular, b.com is covered by
  // the author's own preconnect hint and must not land in the emission list:
  // storing it would make later rewrites emit a duplicate of the author's
  // tag.
  options()->ClearSignatureForTesting();
  options()->EnableFilter(RewriteOptions::kFlushSubresourcesDeprecated);
  server_context()->ComputeSignature(options());
  GoogleString html =
      "<head>"
      "<link rel=\"preconnect\" href=\"//b.com\">"
      "<script src=\"http://d.com/\"/>"
      "</head>"
      "<body>"
      "<script src=\"http://a.com/\"/>"
      "<script src=\"http://b.com/\"/>"
      "</body>";
  Parse("flush_early_flow_ignores_origin_hint_domains", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  CheckPrefetchInfo(1, 0, 1, "http://a.com");
}

TEST_F(InsertDnsPrefetchFilterTest, FullFlowTestForLogging) {
  GoogleString html_input = CreateHtml(10);
  Parse("store_8_of_10", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  CheckPrefetchInfo(10, 0, 8, CreateDomainsVector(8));
  output_.clear();

  html_input = CreateHtml(9);
  Parse("store_8_of_9", html_input);
  EXPECT_EQ(AddHtmlBody(html_input), output_);
  CheckPrefetchInfo(9, 10, 8, CreateDomainsVector(8));
  CheckLogStatus(RewriterHtmlApplication::ACTIVE,
                 RewriterApplication::NOT_APPLIED, 1);
  output_.clear();
}

TEST_F(InsertDnsPrefetchFilterTest, InsertDnsPrefetchFilterWithOtherFilters) {
  options()->ClearSignatureForTesting();
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  options()->EnableFilter(RewriteOptions::kDelayImages);
  server_context()->ComputeSignature(options());
  GoogleString html =
      "<head>"
      "<script src=\"http://b.com/\"/>"
      "</head>"
      "<body>"
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"http://a.com/\">"
      "<script src=\"http://b.com/\"/>"
      "<img src=\"http://c.com/\"/>"
      "</body>";
  Parse("store_domains_in_body", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  // b.com is not stored since it is already in HEAD.
  CheckPrefetchInfo(2, 0, 2, "http://a.com,http://c.com");
}

TEST_F(InsertDnsPrefetchFilterTest,
       FlushSubresourcesDeprecatedDoesNotAffectDnsPrefetch) {
  // flush_subresources is a deprecated no-op: enabling it must not change
  // which head domains get dns-prefetch hints.
  options()->ClearSignatureForTesting();
  options()->EnableFilter(RewriteOptions::kFlushSubresourcesDeprecated);
  server_context()->ComputeSignature(options());
  GoogleString html =
      "<head>"
      "<script src=\"http://b.com/\"/>"
      "<script src=\"http://d.com/\"/>"
      "</head>"
      "<body>"
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"http://a.com/\">"
      "<script src=\"http://b.com/\"/>"
      "<img src=\"http://c.com/\"/>"
      "</body>";
  Parse("store_domains_in_body", html);
  EXPECT_EQ(AddHtmlBody(html), output_);
  CheckPrefetchInfo(2, 0, 2, "http://a.com,http://c.com");
}

}  // namespace net_instaweb
