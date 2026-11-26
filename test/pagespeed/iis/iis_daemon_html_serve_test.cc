// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_daemon_html_serve.h"

#include <algorithm>

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// ---- the one spelling of a path the HTML cache keeps on IIS

TEST(IisHtmlCachePathTest, PlainLowercasePathsAreCanonical) {
  for (const char* path :
       {"/", "/page.html", "/sub/page.html", "/sub/", "/a-b_c.d/e.aspx",
        "/page.html?x=1", "/page.html?Q=%2F..//", "/.well-known/x",
        "/caf%C3%A9.html", "/a+b.html"}) {
    EXPECT_TRUE(IisHtmlCachePathIsCanonical(path)) << path;
  }
}

TEST(IisHtmlCachePathTest, SpellingsIisReadsAsAnotherPathAreNot) {
  // Each names a file IIS also reaches under another spelling, or a form
  // IIS reads differently from this module: never the cache's key.
  for (const char* path : {
           "",                   // no path
           "page.html",          // not absolute
           "/PAGE.HTML",         // IIS matches files without regard to case
           "/Page.html",         //
           "/sub%2Fpage.html",   // an encoded slash IIS decodes
           "/sub%5Cpage.html",   // an encoded backslash
           "/sub\\page.html",    // a backslash
           "/page%2Ehtml",       // an encoded unreserved byte
           "/%70age.html",       //
           "/caf%c3%a9.html",    // lowercase hex: not the one spelling
           "/%u0070age.html",    // IIS's %u form
           "/page%252Ehtml",     // double escaping
           "/page%",             // a cut escape
           "/page%2",            //
           "//page.html",        // IIS merges slashes
           "/sub//page.html",    //
           "/sub/./page.html",   // dot segments
           "/sub/../page.html",  //
           "/.",                 //
           "/page.html.",        // a trailing dot
           "/sub./page.html",    //
           "/page.html ",        // a space
           "/page.html;x=1",     // a path parameter
           "/page.html::$DATA",  // an NTFS stream
           "/PAGE~1.HTM",        // an 8.3 short name
           "/page~1.htm",        //
           "/page*.html",        // characters IIS refuses in a path
           "/page<.html",        //
           "/page\".html",       //
       }) {
    EXPECT_FALSE(IisHtmlCachePathIsCanonical(path)) << path;
  }
  EXPECT_FALSE(IisHtmlCachePathIsCanonical(StringPiece("/a\0b", 4)));
  EXPECT_FALSE(IisHtmlCachePathIsCanonical("/caf\xc3\xa9.html"));
  EXPECT_FALSE(IisHtmlCachePathIsCanonical("/a\x7f"));
}

TEST(IisHtmlCachePathTest, OnlyTheEscapedNonAsciiByteIsAllowed) {
  EXPECT_TRUE(IisHtmlCachePathIsCanonical("/%80"));
  EXPECT_TRUE(IisHtmlCachePathIsCanonical("/%FF"));
  EXPECT_FALSE(IisHtmlCachePathIsCanonical("/%7F"));
  EXPECT_FALSE(IisHtmlCachePathIsCanonical("/%20"));
  EXPECT_FALSE(IisHtmlCachePathIsCanonical("/%2F"));
  EXPECT_FALSE(IisHtmlCachePathIsCanonical("/%G0"));
}

// ---- finalising the Markdown answer at the final-view send

IisMarkdownAnswer Answer200() {
  IisMarkdownAnswer answer;
  answer.status = 200;
  answer.headers = {
      {"Content-Type", "text/markdown; charset=utf-8"},
      {"Content-Length", "5"},
      {"Cache-Control", "private"},
      {"Vary", "Accept"},
      {"X-Robots-Tag", "noindex"},
      {"Age", "0"},
      {"ETag", "W/\"psm-x\""},
  };
  answer.body = "# x\n\n";
  return answer;
}

bool Removes(const IisMarkdownFinalPlan& plan, const char* name) {
  return std::find(plan.remove.begin(), plan.remove.end(), name) !=
         plan.remove.end();
}

TEST(IisMarkdownFinalTest, ThePendingAnswerIsFinalisedWithEverythingElseRemoved) {
  const DaemonHtmlHeaderLines current = {
      {"Server", "Microsoft-IIS/10.0"},
      {"Date", "x"},
      {"X-Powered-By", "ASP.NET"},
      {"Strict-Transport-Security", "max-age=60"},
      {"X-Content-Type-Options", "nosniff"},
      {"Cache-Control", "no-store"},
      {"Set-Cookie", "a=1"},
      {"Set-Cookie", "b=2"},
      {"Vary", "Accept-Encoding"},
      {"Connection", "keep-alive"},
  };
  const IisMarkdownFinalPlan plan = PlanIisMarkdownFinal(
      kIisMarkdownPendingStatus, kIisMarkdownPendingSubStatus, 0, current,
      Answer200());
  ASSERT_TRUE(plan.finalize);
  EXPECT_FALSE(plan.transport_conflict);
  for (const char* name :
       {"X-Powered-By", "Strict-Transport-Security", "X-Content-Type-Options",
        "Cache-Control", "Set-Cookie", "Vary"}) {
    EXPECT_TRUE(Removes(plan, name)) << name;
  }
  // Each name once, however many lines it has.
  EXPECT_EQ(1, std::count(plan.remove.begin(), plan.remove.end(),
                          GoogleString("Set-Cookie")));
  // The server core's transport lines stay.
  for (const char* name : {"Server", "Date", "Connection"}) {
    EXPECT_FALSE(Removes(plan, name)) << name;
  }
}

TEST(IisMarkdownFinalTest, AnEncodingOrRangeLineIsKeptAndCounted) {
  for (const char* name : {"Content-Encoding", "Content-Range"}) {
    const IisMarkdownFinalPlan plan = PlanIisMarkdownFinal(
        kIisMarkdownPendingStatus, kIisMarkdownPendingSubStatus, 0,
        {{name, "gzip"}, {"X-Custom", "v"}}, Answer200());
    ASSERT_TRUE(plan.finalize) << name;
    EXPECT_TRUE(plan.transport_conflict) << name;
    EXPECT_FALSE(Removes(plan, name)) << name;
    EXPECT_TRUE(Removes(plan, "X-Custom")) << name;
  }
}

TEST(IisMarkdownFinalTest, ChunkingNeverGoesOutBesideALength) {
  IisMarkdownFinalPlan plan = PlanIisMarkdownFinal(
      kIisMarkdownPendingStatus, kIisMarkdownPendingSubStatus, 0,
      {{"Transfer-Encoding", "chunked"}}, Answer200());
  ASSERT_TRUE(plan.finalize);
  EXPECT_TRUE(Removes(plan, "Transfer-Encoding"));

  IisMarkdownAnswer not_modified;
  not_modified.status = 304;
  not_modified.headers = {{"ETag", "W/\"psm-x\""},
                          {"Cache-Control", "private"},
                          {"Vary", "Accept"}};
  plan = PlanIisMarkdownFinal(kIisMarkdownPendingStatus,
                              kIisMarkdownPendingSubStatus, 0,
                              {{"Transfer-Encoding", "chunked"}}, not_modified);
  ASSERT_TRUE(plan.finalize);
  EXPECT_FALSE(Removes(plan, "Transfer-Encoding"));
}

TEST(IisMarkdownFinalTest, AResponseAnotherModuleReplacedIsNotFinalised) {
  // A custom error page, another module's own status, or a body: what goes
  // out is no longer the main registration's pending answer.
  const IisMarkdownAnswer answer = Answer200();
  EXPECT_FALSE(PlanIisMarkdownFinal(kIisMarkdownPendingStatus,
                                    kIisMarkdownPendingSubStatus, 0,
                                    {{"Content-Type", "text/html"}}, answer)
                   .finalize);
  EXPECT_FALSE(PlanIisMarkdownFinal(kIisMarkdownPendingStatus,
                                    kIisMarkdownPendingSubStatus, 1, {}, answer)
                   .finalize);
  EXPECT_FALSE(
      PlanIisMarkdownFinal(kIisMarkdownPendingStatus, 0, 0, {}, answer)
          .finalize);
  EXPECT_FALSE(PlanIisMarkdownFinal(404, kIisMarkdownPendingSubStatus, 0, {},
                                    answer)
                   .finalize);
  EXPECT_FALSE(PlanIisMarkdownFinal(200, kIisMarkdownPendingSubStatus, 0, {},
                                    answer)
                   .finalize);
  // Never anything but a 200 or a 304.
  IisMarkdownAnswer odd = answer;
  odd.status = 302;
  EXPECT_FALSE(PlanIisMarkdownFinal(kIisMarkdownPendingStatus,
                                    kIisMarkdownPendingSubStatus, 0, {}, odd)
                   .finalize);
}

TEST(IisMarkdownFinalTest, ThePendingStatusIsAnError) {
  // What goes out when nothing finalises the answer.
  EXPECT_EQ(500, kIisMarkdownPendingStatus);
}

// ---- the Vary: Accept line of an HTML response that has a Markdown copy

TEST(IisVaryWithAcceptTest, AcceptIsAddedOnceToTheExistingLine) {
  EXPECT_EQ("Accept", IisVaryWithAccept(""));
  EXPECT_EQ("Accept-Encoding, Accept", IisVaryWithAccept("Accept-Encoding"));
  EXPECT_EQ("Accept-Encoding,Cookie, Accept",
            IisVaryWithAccept("Accept-Encoding,Cookie"));
}

TEST(IisVaryWithAcceptTest, AnExistingAcceptTokenIsKept) {
  EXPECT_EQ("Accept", IisVaryWithAccept("Accept"));
  EXPECT_EQ("accept, Cookie", IisVaryWithAccept("accept, Cookie"));
  EXPECT_EQ("Cookie , ACCEPT ", IisVaryWithAccept("Cookie , ACCEPT "));
  // Accept-Encoding is another token.
  EXPECT_EQ("Accept-Encoding, Accept", IisVaryWithAccept("Accept-Encoding"));
  // "*" already varies on everything.
  EXPECT_EQ("*", IisVaryWithAccept("*"));
}

}  // namespace
}  // namespace net_instaweb
