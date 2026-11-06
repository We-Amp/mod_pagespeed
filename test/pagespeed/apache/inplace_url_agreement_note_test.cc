// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The Apache port optimizes a request in place only when httpd and the module
// read it alike.  The verdict is saved as a request note at translate_name
// time, beside the URL the module saved, and a request that follows another
// (an internal redirect, a subrequest) keeps the verdict of the request its
// URL came from.  The note logic runs here on real APR tables; the two call
// sites need a live httpd, so they are pinned by reading the source, the
// idiom daemon_seam_test.cc uses.

#include <cctype>
#include <cstring>

#include "apr_strings.h"
#include "pagespeed/apache/instaweb_context.h"
#include "pagespeed/apache/instaweb_handler.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/google_url.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/apache/mock_apache.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const char kHost[] = "www.example.com";

class InPlaceUrlAgreementNoteTest : public testing::Test {
 protected:
  void SetUp() override {
    MockApache::Initialize();  // apr_initialize underneath.
    apr_pool_create(&pool_, nullptr);
  }
  void TearDown() override {
    apr_pool_destroy(pool_);
    MockApache::Terminate();
  }

  // A request httpd has read as `uri`.
  request_rec* NewRequest(const char* uri) {
    request_rec* request =
        static_cast<request_rec*>(apr_pcalloc(pool_, sizeof(request_rec)));
    request->pool = pool_;
    request->notes = apr_table_make(pool_, 4);
    request->uri = apr_pstrdup(pool_, uri);
    return request;
  }

  // Saves the verdict for `request` against the module's parse of `url`.
  bool Save(request_rec* request, const char* url, bool merge_slashes = true,
            StringPiece server_host = kHost,
            StringPiece server_scheme = "http") {
    GoogleUrl gurl(url);
    return InstawebContext::SaveInPlaceUrlAgreement(
        request, gurl, server_scheme, server_host, merge_slashes);
  }

  apr_pool_t* pool_ = nullptr;
};

TEST_F(InPlaceUrlAgreementNoteTest, ARequestReadAlikeIsOptimizedInPlace) {
  request_rec* request = NewRequest("/styles/site.css");
  EXPECT_TRUE(Save(request, "http://www.example.com/styles/site.css?v=1"));
  EXPECT_TRUE(InstawebContext::InPlaceUrlAgreed(request));
}

TEST_F(InPlaceUrlAgreementNoteTest, AnEncodedSpellingIsOptimizedInPlace) {
  request_rec* request = NewRequest("/styles/a b.css");
  EXPECT_TRUE(Save(request, "http://www.example.com/styles/a%20b.css"));
  EXPECT_TRUE(InstawebContext::InPlaceUrlAgreed(request));
}

TEST_F(InPlaceUrlAgreementNoteTest, ARequestReadDifferentlyIsLeftToApache) {
  // httpd keeps the backslash in its path; the module reads a separator.
  request_rec* request = NewRequest("/styles\\site.css");
  EXPECT_FALSE(Save(request, "http://www.example.com/styles\\site.css"));
  EXPECT_FALSE(InstawebContext::InPlaceUrlAgreed(request));
}

TEST_F(InPlaceUrlAgreementNoteTest, SlashRunsFollowTheServersMergeSlashes) {
  request_rec* merged = NewRequest("/styles/site.css");
  EXPECT_TRUE(Save(merged, "http://www.example.com/styles//site.css", true));
  request_rec* kept = NewRequest("/styles//site.css");
  EXPECT_TRUE(Save(kept, "http://www.example.com/styles//site.css", false));
  request_rec* unmerged = NewRequest("/styles/site.css");
  EXPECT_FALSE(
      Save(unmerged, "http://www.example.com/styles//site.css", false));
}

TEST_F(InPlaceUrlAgreementNoteTest, TheHostIsComparedWithoutCase) {
  request_rec* request = NewRequest("/styles/site.css");
  EXPECT_TRUE(Save(request, "http://WWW.Example.COM/styles/site.css", true,
                   "www.example.com"));
  request_rec* other = NewRequest("/styles/site.css");
  EXPECT_FALSE(Save(other, "http://static.example.com/styles/site.css", true,
                    "www.example.com"));
}

TEST_F(InPlaceUrlAgreementNoteTest, TheSchemeIsPartOfTheReading) {
  // A request line naming another scheme than the one httpd serves the
  // request under is left to Apache; the same scheme agrees, in any case.
  request_rec* other = NewRequest("/styles/site.css");
  EXPECT_FALSE(Save(other, "https://www.example.com/styles/site.css", true,
                    kHost, "http"));
  request_rec* reverse = NewRequest("/styles/site.css");
  EXPECT_FALSE(Save(reverse, "http://www.example.com/styles/site.css", true,
                    kHost, "https"));
  request_rec* same = NewRequest("/styles/site.css");
  EXPECT_TRUE(Save(same, "https://www.example.com/styles/site.css", true, kHost,
                   "https"));
  request_rec* upper = NewRequest("/styles/site.css");
  EXPECT_TRUE(Save(upper, "HTTPS://www.example.com/styles/site.css", true,
                   kHost, "HTTPS"));
}

TEST_F(InPlaceUrlAgreementNoteTest, AnInvalidUrlOrNoPathIsLeftToApache) {
  request_rec* invalid = NewRequest("/styles/site.css");
  EXPECT_FALSE(Save(invalid, "not a url"));
  request_rec* no_uri = NewRequest("/styles/site.css");
  no_uri->uri = nullptr;
  EXPECT_FALSE(Save(no_uri, "http://www.example.com/styles/site.css"));
}

TEST_F(InPlaceUrlAgreementNoteTest, WithoutASavedVerdictNothingIsInPlace) {
  EXPECT_FALSE(InstawebContext::InPlaceUrlAgreed(NewRequest("/a.css")));
}

TEST_F(InPlaceUrlAgreementNoteTest, TheFirstVerdictOfARequestStands) {
  // Saved at translate_name time; a later call, after a module may have
  // rewritten request->uri, keeps it.
  request_rec* request = NewRequest("/styles/site.css");
  EXPECT_TRUE(Save(request, "http://www.example.com/styles/site.css"));
  request->uri = apr_pstrdup(pool_, "/index.php");
  EXPECT_TRUE(Save(request, "http://www.example.com/styles/site.css"));
  EXPECT_TRUE(InstawebContext::InPlaceUrlAgreed(request));

  request_rec* differs = NewRequest("/styles\\site.css");
  EXPECT_FALSE(Save(differs, "http://www.example.com/styles\\site.css"));
  differs->uri = apr_pstrdup(pool_, "/styles/site.css");
  EXPECT_FALSE(Save(differs, "http://www.example.com/styles/site.css"));
  EXPECT_FALSE(InstawebContext::InPlaceUrlAgreed(differs));
}

TEST_F(InPlaceUrlAgreementNoteTest, AnInternalRedirectKeepsTheVerdict) {
  // The redirected request is keyed on the original URL, so it carries the
  // original request's verdict, whatever its own path.
  request_rec* original = NewRequest("/styles\\site.css");
  EXPECT_FALSE(Save(original, "http://www.example.com/styles\\site.css"));
  request_rec* redirected = NewRequest("/styles/site.css");
  redirected->prev = original;
  EXPECT_FALSE(Save(redirected, "http://www.example.com/styles/site.css"));
  EXPECT_FALSE(InstawebContext::InPlaceUrlAgreed(redirected));

  request_rec* agreeing = NewRequest("/styles/site.css");
  EXPECT_TRUE(Save(agreeing, "http://www.example.com/styles/site.css"));
  request_rec* followed = NewRequest("/index.php");
  followed->prev = agreeing;
  EXPECT_TRUE(InstawebContext::InPlaceUrlAgreed(followed));
  EXPECT_TRUE(Save(followed, "http://www.example.com/styles/site.css"));
}

TEST_F(InPlaceUrlAgreementNoteTest, AnErrorDocumentAfterANotFoundIsItsOwn) {
  // As for the URL: what follows a request the module answered with a
  // not-found is httpd serving the site's ErrorDocument, read on its own.
  request_rec* original = NewRequest("/styles\\site.css");
  EXPECT_FALSE(Save(original, "http://www.example.com/styles\\site.css"));
  apr_table_setn(original->notes, kPagespeedNotFoundNote, "1");
  request_rec* error_document = NewRequest("/errors/404.html");
  error_document->prev = original;
  EXPECT_TRUE(Save(error_document, "http://www.example.com/errors/404.html"));
}

TEST_F(InPlaceUrlAgreementNoteTest, ASubrequestKeepsTheMainRequestsVerdict) {
  request_rec* main = NewRequest("/styles\\site.css");
  EXPECT_FALSE(Save(main, "http://www.example.com/styles\\site.css"));
  request_rec* subrequest = NewRequest("/styles/site.css");
  subrequest->main = main;
  EXPECT_FALSE(InstawebContext::InPlaceUrlAgreed(subrequest));
  EXPECT_FALSE(Save(subrequest, "http://www.example.com/styles/site.css"));
}

// --- The scheme of an origin-form request ----------------------------------

class OriginFormSchemeTest : public RewriteTestBase {
 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();
    MockApache::Initialize();
    apr_pool_create(&pool_, nullptr);
    request_ = static_cast<request_rec*>(apr_pcalloc(pool_, sizeof(*request_)));
    request_->pool = pool_;
    request_->headers_in = apr_table_make(pool_, 4);
  }
  void TearDown() override {
    apr_pool_destroy(pool_);
    MockApache::Terminate();
    RewriteTestBase::TearDown();
  }
  GoogleString Scheme() {
    return InstawebContext::OriginFormScheme(*options(), request_).as_string();
  }

  apr_pool_t* pool_ = nullptr;
  request_rec* request_ = nullptr;
};

TEST_F(OriginFormSchemeTest, HttpdsSchemeWithoutForwardedProto) {
  // The mock httpd serves plain http.
  EXPECT_EQ("http", Scheme());
  apr_table_setn(request_->headers_in, "X-Forwarded-Proto", "https");
  EXPECT_EQ("http", Scheme()) << "honoured although the option is off";
}

TEST_F(OriginFormSchemeTest, AValidForwardedProtoWhenRespected) {
  options()->ClearSignatureForTesting();
  options()->set_respect_x_forwarded_proto(true);
  EXPECT_EQ("http", Scheme());
  apr_table_setn(request_->headers_in, "X-Forwarded-Proto", "https");
  EXPECT_EQ("https", Scheme());
  apr_table_setn(request_->headers_in, "X-Forwarded-Proto", "gopher");
  EXPECT_EQ("http", Scheme()) << "an unsupported value is ignored";
}

// --- Wiring pins -----------------------------------------------------------

GoogleString ReadStrippedSource(const GoogleString& relative_path) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/", relative_path);
  EXPECT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

TEST(InPlaceUrlAgreementWiringTest, TheVerdictIsSavedAtTranslateName) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/apache/instaweb_handler.cc");
  const size_t hook = cc.find("apr_status_tInstawebHandler::save_url_in_note(");
  ASSERT_NE(GoogleString::npos, hook)
      << "the translate_name hook has been renamed; this pin needs updating";
  const size_t url = cc.find("InstawebContext::MakeRequestUrl(", hook);
  ASSERT_NE(GoogleString::npos, url);
  // Saved right after the URL, from httpd's host and its slash-merging
  // setting for the server.
  const size_t save = cc.find(
      "InstawebContext::SaveInPlaceUrlAgreement(request,gurl,"
      "InstawebContext::OriginFormScheme(*server_context->global_options(),"
      "request),ap_get_server_name_for_url(request),core_config->"
      "merge_slashes!=AP_CORE_CONFIG_OFF);",
      hook);
  ASSERT_NE(GoogleString::npos, save)
      << "translate_name no longer saves whether httpd and the module read "
         "the request alike";
  EXPECT_LT(url, save);
  EXPECT_NE(GoogleString::npos,
            cc.find("ap_get_core_module_config(request->server->module_config)",
                    hook));
  const size_t next =
      cc.find("InstawebHandler::instaweb_map_to_storage(", hook);
  ASSERT_NE(GoogleString::npos, next);
  EXPECT_LT(save, next);
}

TEST(InPlaceUrlAgreementWiringTest, EveryInPlacePathRequiresTheVerdict) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/apache/instaweb_handler.cc");
  const size_t handler = cc.find("boolInstawebHandler::HandleAsInPlace(){");
  ASSERT_NE(GoogleString::npos, handler)
      << "the in-place handler has been renamed; this pin needs updating";
  const size_t gate =
      cc.find("if(!InstawebContext::InPlaceUrlAgreed(request_)){", handler);
  ASSERT_NE(GoogleString::npos, gate)
      << "the in-place handler no longer requires httpd and the module to "
         "read the request alike";
  // The gate returns before any of the four in-place paths: the daemon serve
  // and recorder, the classic lookup and recorder.
  const size_t gate_end = cc.find("}", gate);
  EXPECT_NE(GoogleString::npos,
            cc.substr(gate, gate_end - gate).find("returnhandled;"));
  for (const char* path :
       {"ServeFromDaemonSubstrate()", "MakeDaemonIproRecorderIfReady(",
        "FetchInPlaceResource(", "MakeIproRecorderIfClassic("}) {
    const size_t at = cc.find(path, handler);
    ASSERT_NE(GoogleString::npos, at) << path;
    EXPECT_LT(gate, at) << path << " runs before the agreement is checked";
  }
}

}  // namespace

}  // namespace net_instaweb
