// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The IIS port optimizes a request in place only when the path IIS routes it
// on (HTTP.sys's decoded CookedUrl.pAbsPath) and the module's parse of the
// request URL name the same path.  The decision is tested on the readings
// IIS produces; the call sites sit behind the IIS runtime headers, so they
// are pinned by reading the port source as data, the idiom of
// iis_request_stage_pin_test.cc.

#include "pagespeed/iis/iis_inplace_url_agreement.h"

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/google_url.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// IIS hands the module the decoded full URL; the module parses that string.
// `iis_path` is what IIS routes on for the same request.
bool Agrees(StringPiece iis_path, StringPiece decoded_full_url) {
  GoogleUrl url(decoded_full_url);
  CHECK(url.IsWebValid()) << decoded_full_url;
  return IisInPlaceUrlAgrees(iis_path, url.PathSansQuery());
}

TEST(IisInPlaceUrlAgreementTest, PlainPathsAreOptimizedInPlace) {
  EXPECT_TRUE(Agrees("/iua/site.css", "http://localhost/iua/site.css"));
  EXPECT_TRUE(Agrees("/iua/site.css", "http://localhost/iua/site.css?v=1"));
  EXPECT_TRUE(Agrees("/", "http://localhost/"));
  EXPECT_TRUE(Agrees("/a b.css", "http://localhost/a b.css"));
  EXPECT_TRUE(Agrees("/caf\xC3\xA9.css", "http://localhost/caf\xC3\xA9.css"));
}

TEST(IisInPlaceUrlAgreementTest, ADecodedQuestionMarkIsLeftToIis) {
  // IIS reads a path with a question mark in it; the module's parse reads
  // a query.  Different paths: left to IIS.
  EXPECT_FALSE(
      Agrees("/iua/site.css?v=1", "http://localhost/iua/site.css?v=1"));
  EXPECT_FALSE(Agrees("/iua/site.css?", "http://localhost/iua/site.css?"));
  EXPECT_FALSE(Agrees("/iua/a?b/c.css", "http://localhost/iua/a?b/c.css"));
}

TEST(IisInPlaceUrlAgreementTest, ADecodedHashIsLeftToIis) {
  // IIS reads a path with a hash in it; the module's parse reads a
  // fragment.  Different paths: left to IIS.
  EXPECT_FALSE(Agrees("/iua/x#y.css", "http://localhost/iua/x#y.css"));
}

TEST(IisInPlaceUrlAgreementTest, ADecodedPercentSign) {
  // IIS reads a path with a percent sign in it.  One that starts no escape
  // reads the same both ways; one that does is decoded a second time by the
  // module's parse and names another path.
  EXPECT_TRUE(Agrees("/iua/100%.css", "http://localhost/iua/100%.css"));
  EXPECT_FALSE(Agrees("/iua/a%41.css", "http://localhost/iua/a%41.css"));
  EXPECT_FALSE(Agrees("/iua/a%3Fb.css", "http://localhost/iua/a%3Fb.css"));
}

TEST(IisInPlaceUrlAgreementTest, CaseIsComparedAsIisComparesIt) {
  EXPECT_TRUE(Agrees("/iua/HELD.CSS", "http://localhost/iua/HELD.CSS"));
  EXPECT_TRUE(Agrees("/iua/held.css", "http://localhost/iua/HELD.CSS"));
  EXPECT_TRUE(Agrees("/IUA/held.css", "http://localhost/iua/held.css"));
  // Letters the module keeps escaped compare like the letters.
  EXPECT_TRUE(IisInPlaceUrlAgrees("/iua/held.css", "/iua/%48ELD.css"));
  // Beyond ASCII, bytes must match exactly.
  EXPECT_FALSE(Agrees("/caf\xC3\x89.css", "http://localhost/caf\xC3\xA9.css"));
}

TEST(IisInPlaceUrlAgreementTest, WindowsSpellingsOfOneFileReadAlike) {
  // A trailing dot or an 8.3 name is the spelling IIS routes on, and the
  // module's key is that same spelling.
  EXPECT_TRUE(Agrees("/iua/held.css.", "http://localhost/iua/held.css."));
  EXPECT_TRUE(Agrees("/iua/HELD~1.CSS", "http://localhost/iua/HELD~1.CSS"));
  // The module's parse drops a trailing space that IIS keeps.
  EXPECT_FALSE(Agrees("/iua/held.css ", "http://localhost/iua/held.css "));
}

TEST(IisInPlaceUrlAgreementTest, SeparatorsAndDotSegments) {
  // HTTP.sys turns a backslash into a slash and resolves dot segments before
  // IIS routes; the module's parse does the same.
  EXPECT_TRUE(Agrees("/iua/held.css", "http://localhost/iua/held.css"));
  // A path IIS reads with a backslash kept, or unresolved, is left to it.
  EXPECT_FALSE(Agrees("/iua\\held.css", "http://localhost/iua\\held.css"));
  EXPECT_FALSE(Agrees("/iua/../held.css", "http://localhost/iua/../held.css"));
  // IIS does not merge runs of slashes.
  EXPECT_TRUE(Agrees("/iua//held.css", "http://localhost/iua//held.css"));
  EXPECT_FALSE(IisInPlaceUrlAgrees("/iua/held.css", "/iua//held.css"));
}

TEST(IisInPlaceUrlAgreementTest, NoPathIsLeftToIis) {
  EXPECT_FALSE(IisInPlaceUrlAgrees("", "/iua/held.css"));
}

// --- Wiring pins ----------------------------------------------------------

GoogleString ReadStrippedSource(const GoogleString& file_name) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = RunfilePath(StrCat("pagespeed/iis/", file_name));
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

TEST(IisInPlaceUrlAgreementWiringTest, ThePathIsKeptBesideThePristineUrl) {
  const GoogleString cc = ReadStrippedSource("dll_main.cc");
  const size_t url = cc.find("\"X-PRISTINE-URL\",url);");
  ASSERT_NE(GoogleString::npos, url);
  const size_t path =
      cc.find("SetServerVariable(\"X-PRISTINE-PATH\",path.c_str());", url);
  ASSERT_NE(GoogleString::npos, path)
      << "the path IIS routes on is no longer kept beside the pristine URL";
  EXPECT_NE(GoogleString::npos,
            cc.substr(url, path - url).find("cooked.pAbsPath"));
}

TEST(IisInPlaceUrlAgreementWiringTest, EveryInPlacePathRequiresAgreement) {
  const GoogleString cc = ReadStrippedSource("iis_http_module.cpp");
  const size_t branch = cc.find(
      "elseif(options->in_place_rewriting_enabled()&&options->enabled()");
  ASSERT_NE(GoogleString::npos, branch)
      << "the in-place branch has moved; this pin needs updating";
  const size_t gate = cc.find(
      "if(!IisInPlaceUrlAgreesForRequest(pHttpContext,*innerContext->gurl(),"
      "&iis_path)){",
      branch);
  ASSERT_NE(GoogleString::npos, gate)
      << "the in-place branch no longer requires IIS and the module to read "
         "the path alike";
  EXPECT_NE(GoogleString::npos,
            cc.substr(gate, 300).find("returnRQ_NOTIFICATION_CONTINUE;"));
  for (const char* step :
       {"ServeFromIisDaemonSubstrate(pHttpContext", "BuildDaemonRecordRequest(",
        "FetchInPlaceResource("}) {
    const size_t at = cc.find(step, branch);
    ASSERT_NE(GoogleString::npos, at) << step;
    EXPECT_LT(gate, at) << step << " runs before the agreement is checked";
  }
  // The decision is made on IIS's own path, kept before any module could
  // change the request.
  EXPECT_NE(GoogleString::npos,
            cc.find("GetServerVariable(\"X-PRISTINE-PATH\",&wide,&wide_len)"));
}

}  // namespace
}  // namespace net_instaweb
