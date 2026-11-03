// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/inplace_url_agreement.h"

#include "base/logging.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/google_url.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kHost[] = "www.example.com";

// The server's reading is the decoded, normalized path; the module's is the
// canonical, escaped path.  An ordinary path reads the same both ways.
TEST(InPlaceUrlAgreementTest, PlainPathIsOptimizedInPlace) {
  EXPECT_TRUE(InPlaceUrlAgrees("/styles/site.css", kHost, "/styles/site.css",
                               kHost, true));
  EXPECT_TRUE(InPlaceUrlAgrees("/images/Puzzle.jpg", kHost,
                               "/images/Puzzle.jpg", kHost, true));
  EXPECT_TRUE(InPlaceUrlAgrees("/", kHost, "/", kHost, true));
  EXPECT_TRUE(
      InPlaceUrlAgrees("/a+b/c.d.js", kHost, "/a+b/c.d.js", kHost, true));
}

// The server decodes a percent-encoded space or a UTF-8 character; the
// module keeps the escapes.  Both still name the same file.
TEST(InPlaceUrlAgreementTest, PercentEncodedPathIsOptimizedInPlace) {
  EXPECT_TRUE(InPlaceUrlAgrees("/images/a b.png", kHost, "/images/a%20b.png",
                               kHost, true));
  EXPECT_TRUE(InPlaceUrlAgrees("/caf\xC3\xA9/menu.css", kHost,
                               "/caf%C3%A9/menu.css", kHost, true));
  EXPECT_TRUE(InPlaceUrlAgrees("/\xE6\x97\xA5\xE6\x9C\xAC.js", kHost,
                               "/%E6%97%A5%E6%9C%AC.js", kHost, true));
  // Reserved characters the module keeps escaped and the server decodes.
  EXPECT_TRUE(InPlaceUrlAgrees("/100%.css", kHost, "/100%25.css", kHost, true));
  EXPECT_TRUE(InPlaceUrlAgrees("/q?.css", kHost, "/q%3F.css", kHost, true));
  EXPECT_TRUE(InPlaceUrlAgrees("/a\"b.css", kHost, "/a%22b.css", kHost, true));
}

// A query string takes no part in the decision: both readings are paths
// without their query, and the same path with any query is the same path.
TEST(InPlaceUrlAgreementTest, PathWithQueryStringIsOptimizedInPlace) {
  EXPECT_TRUE(InPlaceUrlAgrees("/styles/site.css", kHost, "/styles/site.css",
                               kHost, true));
  EXPECT_TRUE(InPlaceUrlAgrees("/images/a b.png", kHost, "/images/a%20b.png",
                               kHost, true));
}

// A separator the module reads into the path and the server does not, or a
// dot segment the server resolves and the module does not: different paths,
// left to the server.
TEST(InPlaceUrlAgreementTest, PathTheServerReadsDifferentlyIsLeftToIt) {
  EXPECT_FALSE(InPlaceUrlAgrees("/a\\b.css", kHost, "/a/b.css", kHost, true));
  EXPECT_FALSE(InPlaceUrlAgrees("/b.css", kHost, "/x/b.css", kHost, true));
  EXPECT_FALSE(
      InPlaceUrlAgrees("/b.css", kHost, "/a%2F..%2Fb.css", kHost, true));
  EXPECT_FALSE(InPlaceUrlAgrees("/a/b.css", kHost, "/a/B.css", kHost, true));
  EXPECT_FALSE(InPlaceUrlAgrees("", kHost, "/a.css", kHost, true));
}

// The server merges runs of slashes (merge_slashes, on by default) and the
// module keeps them; with the server's setting known, the module's decoded
// path is merged the same way, and the two readings agree.  Off, they do not.
// A slash run ahead of a dot segment reads differently either way: the server
// merges before it resolves the segment, the module resolves it first.
TEST(InPlaceUrlAgreementTest, SlashRunsAreMergedAsTheServerMergesThem) {
  EXPECT_TRUE(InPlaceUrlAgrees("/a/b.css", kHost, "/a//b.css", kHost, true));
  EXPECT_TRUE(
      InPlaceUrlAgrees("/a/b.css", kHost, "/a%2F%2Fb.css", kHost, true));
  EXPECT_TRUE(InPlaceUrlAgrees("/a/b/", kHost, "//a///b//", kHost, true));
  EXPECT_FALSE(InPlaceUrlAgrees("/a/b.css", kHost, "/a//b.css", kHost, false));
  EXPECT_FALSE(InPlaceUrlAgrees("/b.css", kHost, "/a/b.css", kHost, true));
  EXPECT_FALSE(InPlaceUrlAgrees("/b.css", kHost, "/a/b.css", kHost, false));
}

// An escaped slash is a separator to the server and an ordinary escape to
// the module; decoded, both name the same bytes, and the module's key keeps
// the spelling apart from the slash spelling.
TEST(InPlaceUrlAgreementTest, EscapedSlashReadsAlikeOnceDecoded) {
  EXPECT_TRUE(InPlaceUrlAgrees("/a/b.css", kHost, "/a%2Fb.css", kHost, true));
}

// The server matches its server blocks on the host bytes as sent; the module
// decodes and normalizes the host.  A host that reads differently is left to
// the server, in whatever block it chose.
TEST(InPlaceUrlAgreementTest, HostTheServerReadsDifferentlyIsLeftToIt) {
  EXPECT_FALSE(InPlaceUrlAgrees("/a.css", "st%61tic.example.com", "/a.css",
                                "static.example.com", true));
  EXPECT_FALSE(
      InPlaceUrlAgrees("/a.css", "0x7f000001", "/a.css", "127.0.0.1", true));
  EXPECT_FALSE(InPlaceUrlAgrees("/a.css", "a.example.com", "/a.css",
                                "b.example.com", true));
  EXPECT_FALSE(InPlaceUrlAgrees("/a.css", "", "/a.css", kHost, true));
}

TEST(InPlaceUrlAgreementTest, HostCaseDoesNotMatter) {
  EXPECT_TRUE(
      InPlaceUrlAgrees("/a.css", "WWW.Example.com", "/a.css", kHost, true));
}

TEST(InPlaceUrlAgreementTest, LiteralAddressHostsAgree) {
  EXPECT_TRUE(
      InPlaceUrlAgrees("/a.css", "127.0.0.1", "/a.css", "127.0.0.1", true));
  EXPECT_TRUE(InPlaceUrlAgrees("/a.css", "[::1]", "/a.css", "[::1]", true));
}

// --- Apache's reading ------------------------------------------------------
//
// The Apache port compares httpd's request->uri at translate_name time with
// the path of the URL the module parsed from the raw request line.  httpd
// percent-decodes the path, resolves dot segments and, unless MergeSlashes
// is off, merges runs of slashes; it keeps a backslash as an ordinary byte.
// Each case below gives the raw path of the request line, the request->uri
// httpd derives from it, and the MergeSlashes setting; the module's side is
// the module's own parse of that raw path.

struct ApacheReading {
  const char* raw_path;   // as sent on the request line
  const char* httpd_uri;  // request->uri at translate_name time
  bool merge_slashes;     // MergeSlashes for the server
};

bool ApacheAgrees(const ApacheReading& reading, StringPiece httpd_host,
                  StringPiece request_host) {
  GoogleUrl url(StrCat("http://", request_host, reading.raw_path));
  CHECK(url.IsWebValid()) << reading.raw_path;
  return InPlaceUrlAgrees(reading.httpd_uri, httpd_host, url.PathSansQuery(),
                          url.Host(), reading.merge_slashes);
}

TEST(InPlaceUrlAgreementTest, ApachePlainAndEncodedPathsAreOptimizedInPlace) {
  const ApacheReading kAlike[] = {
      {"/styles/site.css", "/styles/site.css", true},
      {"/styles/site.css?v=2", "/styles/site.css", true},
      {"/styles/%73ite.css", "/styles/site.css", true},
      {"/styles/a%20b.css", "/styles/a b.css", true},
      {"/caf%C3%A9/menu.css", "/caf\xC3\xA9/menu.css", true},
      {"/styles/a%5Cb.css", "/styles/a\\b.css", true},
  };
  for (const ApacheReading& reading : kAlike) {
    EXPECT_TRUE(ApacheAgrees(reading, kHost, kHost)) << reading.raw_path;
  }
}

TEST(InPlaceUrlAgreementTest, ApacheDotSegmentsAreResolvedAlike) {
  const ApacheReading kAlike[] = {
      {"/styles/../site.css", "/site.css", true},
      {"/styles/./site.css", "/styles/site.css", true},
      {"/styles/%2e%2e/site.css", "/site.css", true},
      {"/a/b/../../styles/site.css", "/styles/site.css", false},
  };
  for (const ApacheReading& reading : kAlike) {
    EXPECT_TRUE(ApacheAgrees(reading, kHost, kHost)) << reading.raw_path;
  }
}

TEST(InPlaceUrlAgreementTest, ApacheSlashRunsFollowMergeSlashes) {
  // MergeSlashes on (httpd's default): httpd merges, and so does the
  // comparison.
  EXPECT_TRUE(ApacheAgrees({"/styles//site.css", "/styles/site.css", true},
                           kHost, kHost));
  EXPECT_TRUE(ApacheAgrees({"//styles///site.css", "/styles/site.css", true},
                           kHost, kHost));
  // MergeSlashes off: httpd keeps the run, and so does the comparison.
  EXPECT_TRUE(ApacheAgrees({"/styles//site.css", "/styles//site.css", false},
                           kHost, kHost));
  // A comparison that merged where httpd did not, or the other way round,
  // would name different paths.
  EXPECT_FALSE(ApacheAgrees({"/styles//site.css", "/styles//site.css", true},
                            kHost, kHost));
  EXPECT_FALSE(ApacheAgrees({"/styles//site.css", "/styles/site.css", false},
                            kHost, kHost));
}

TEST(InPlaceUrlAgreementTest, ApacheSeparatorReadDifferentlyIsLeftToApache) {
  // httpd keeps a backslash as a byte of the path; the module reads it as a
  // separator.  Different paths: the request is left to Apache.
  const ApacheReading kDifferent[] = {
      {"/styles\\site.css", "/styles\\site.css", true},
      {"/members\\..\\styles/site.css", "/members\\..\\styles/site.css", true},
      {"/styles/sub\\site.css", "/styles/sub\\site.css", false},
  };
  for (const ApacheReading& reading : kDifferent) {
    EXPECT_FALSE(ApacheAgrees(reading, kHost, kHost)) << reading.raw_path;
  }
}

TEST(InPlaceUrlAgreementTest, ApacheHostCaseDoesNotMatter) {
  // httpd lowercases the host it matched; the module's URL host is
  // lowercased too, whatever case the request carried.
  const ApacheReading kPlain = {"/styles/site.css", "/styles/site.css", true};
  EXPECT_TRUE(ApacheAgrees(kPlain, "www.example.com", "WWW.Example.COM"));
  EXPECT_TRUE(ApacheAgrees(kPlain, "WWW.EXAMPLE.COM", "www.example.com"));
  EXPECT_TRUE(ApacheAgrees(kPlain, "[::1]", "[::1]"));
  EXPECT_FALSE(ApacheAgrees(kPlain, "www.example.com", "static.example.com"));
}

}  // namespace
}  // namespace net_instaweb
