// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/nginx/ngx_inplace_url_agreement.h"

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kHost[] = "www.example.com";

// The server's reading is the decoded, normalized path; the module's is the
// canonical, escaped path.  An ordinary path reads the same both ways.
TEST(NgxInPlaceUrlAgreementTest, PlainPathIsOptimizedInPlace) {
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/styles/site.css", kHost, "/styles/site.css",
                                  kHost, true));
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/images/Puzzle.jpg", kHost,
                                  "/images/Puzzle.jpg", kHost, true));
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/", kHost, "/", kHost, true));
  EXPECT_TRUE(
      NgxInPlaceUrlAgrees("/a+b/c.d.js", kHost, "/a+b/c.d.js", kHost, true));
}

// The server decodes a percent-encoded space or a UTF-8 character; the
// module keeps the escapes.  Both still name the same file.
TEST(NgxInPlaceUrlAgreementTest, PercentEncodedPathIsOptimizedInPlace) {
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/images/a b.png", kHost, "/images/a%20b.png",
                                  kHost, true));
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/caf\xC3\xA9/menu.css", kHost,
                                  "/caf%C3%A9/menu.css", kHost, true));
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/\xE6\x97\xA5\xE6\x9C\xAC.js", kHost,
                                  "/%E6%97%A5%E6%9C%AC.js", kHost, true));
  // Reserved characters the module keeps escaped and the server decodes.
  EXPECT_TRUE(
      NgxInPlaceUrlAgrees("/100%.css", kHost, "/100%25.css", kHost, true));
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/q?.css", kHost, "/q%3F.css", kHost, true));
  EXPECT_TRUE(
      NgxInPlaceUrlAgrees("/a\"b.css", kHost, "/a%22b.css", kHost, true));
}

// A query string takes no part in the decision: both readings are paths
// without their query, and the same path with any query is the same path.
TEST(NgxInPlaceUrlAgreementTest, PathWithQueryStringIsOptimizedInPlace) {
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/styles/site.css", kHost, "/styles/site.css",
                                  kHost, true));
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/images/a b.png", kHost, "/images/a%20b.png",
                                  kHost, true));
}

// A separator the module reads into the path and the server does not, or a
// dot segment the server resolves and the module does not: different paths,
// left to the server.
TEST(NgxInPlaceUrlAgreementTest, PathTheServerReadsDifferentlyIsLeftToIt) {
  EXPECT_FALSE(
      NgxInPlaceUrlAgrees("/a\\b.css", kHost, "/a/b.css", kHost, true));
  EXPECT_FALSE(NgxInPlaceUrlAgrees("/b.css", kHost, "/x/b.css", kHost, true));
  EXPECT_FALSE(
      NgxInPlaceUrlAgrees("/b.css", kHost, "/a%2F..%2Fb.css", kHost, true));
  EXPECT_FALSE(NgxInPlaceUrlAgrees("/a/b.css", kHost, "/a/B.css", kHost, true));
  EXPECT_FALSE(NgxInPlaceUrlAgrees("", kHost, "/a.css", kHost, true));
}

// The server merges runs of slashes (merge_slashes, on by default) and the
// module keeps them; with the server's setting known, the module's decoded
// path is merged the same way, and the two readings agree.  Off, they do not.
// A slash run ahead of a dot segment reads differently either way: the server
// merges before it resolves the segment, the module resolves it first.
TEST(NgxInPlaceUrlAgreementTest, SlashRunsAreMergedAsTheServerMergesThem) {
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/a/b.css", kHost, "/a//b.css", kHost, true));
  EXPECT_TRUE(
      NgxInPlaceUrlAgrees("/a/b.css", kHost, "/a%2F%2Fb.css", kHost, true));
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/a/b/", kHost, "//a///b//", kHost, true));
  EXPECT_FALSE(
      NgxInPlaceUrlAgrees("/a/b.css", kHost, "/a//b.css", kHost, false));
  EXPECT_FALSE(NgxInPlaceUrlAgrees("/b.css", kHost, "/a/b.css", kHost, true));
  EXPECT_FALSE(NgxInPlaceUrlAgrees("/b.css", kHost, "/a/b.css", kHost, false));
}

// An escaped slash is a separator to the server and an ordinary escape to
// the module; decoded, both name the same bytes, and the module's key keeps
// the spelling apart from the slash spelling.
TEST(NgxInPlaceUrlAgreementTest, EscapedSlashReadsAlikeOnceDecoded) {
  EXPECT_TRUE(
      NgxInPlaceUrlAgrees("/a/b.css", kHost, "/a%2Fb.css", kHost, true));
}

// The server matches its server blocks on the host bytes as sent; the module
// decodes and normalizes the host.  A host that reads differently is left to
// the server, in whatever block it chose.
TEST(NgxInPlaceUrlAgreementTest, HostTheServerReadsDifferentlyIsLeftToIt) {
  EXPECT_FALSE(NgxInPlaceUrlAgrees("/a.css", "st%61tic.example.com", "/a.css",
                                   "static.example.com", true));
  EXPECT_FALSE(
      NgxInPlaceUrlAgrees("/a.css", "0x7f000001", "/a.css", "127.0.0.1", true));
  EXPECT_FALSE(NgxInPlaceUrlAgrees("/a.css", "a.example.com", "/a.css",
                                   "b.example.com", true));
  EXPECT_FALSE(NgxInPlaceUrlAgrees("/a.css", "", "/a.css", kHost, true));
}

TEST(NgxInPlaceUrlAgreementTest, HostCaseDoesNotMatter) {
  EXPECT_TRUE(
      NgxInPlaceUrlAgrees("/a.css", "WWW.Example.com", "/a.css", kHost, true));
}

TEST(NgxInPlaceUrlAgreementTest, LiteralAddressHostsAgree) {
  EXPECT_TRUE(
      NgxInPlaceUrlAgrees("/a.css", "127.0.0.1", "/a.css", "127.0.0.1", true));
  EXPECT_TRUE(NgxInPlaceUrlAgrees("/a.css", "[::1]", "/a.css", "[::1]", true));
}

// --- Wiring pin -----------------------------------------------------------
//
// The port cannot be linked into a unit test (see test/pagespeed/nginx/BUILD),
// so the one place that consults the decision is pinned by reading the
// source. StripWhitespace strips whitespace, not comments, so a pinned token
// may not appear in a comment either.

GoogleString ReadStrippedSource(const GoogleString& relative_path) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/", relative_path);
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  CHECK(!source.empty()) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

TEST(NgxInPlaceUrlAgreementTest, TheInPlaceBranchConsultsTheDecision) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  // The decision is fed the server's own normalized URI and the host the
  // module built its URL from, against the module's path and host, with the
  // server block's slash-merging setting.
  EXPECT_NE(GoogleString::npos,
            cc.find("NgxInPlaceUrlAgrees(str_to_string_piece(r->uri),"
                    "ps_determine_host(r),url.PathSansQuery(),url.Host(),"
                    "cscf->merge_slashes!=0)"))
      << "the in-place agreement is no longer decided from the server's URI "
         "and the module's path";
  // The in-place branch -- lookup, daemon serve and recorder mark alike --
  // is entered only when the two readings agree.
  EXPECT_NE(GoogleString::npos,
            cc.find("(r->method&(NGX_HTTP_GET|NGX_HTTP_HEAD))&&!r->internal&&"
                    "ps_in_place_url_agrees(r,url)){"))
      << "the in-place branch of ps_resource_handler no longer requires the "
         "server and the module to read the request alike";
}

}  // namespace
}  // namespace net_instaweb
