// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/apache/apache_daemon_html.h"

#include <cstring>

#include "apr_pools.h"   // NOLINT
#include "apr_tables.h"  // NOLINT
#include "pagespeed/kernel/http/http_names.h"
#include "test/pagespeed/apache/mock_apache.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

int CountLines(void* rec, const char*, const char*) {
  ++*static_cast<int*>(rec);
  return 1;
}

int Lines(const apr_table_t* table) {
  int n = 0;
  apr_table_do(CountLines, &n, table, nullptr);
  return n;
}

class ApacheDaemonHtmlTest : public testing::Test {
 protected:
  void SetUp() override {
    MockApache::Initialize();
    memset(&request_, 0, sizeof(request_));
    MockApache::PrepareRequest(&request_);
    request_.err_headers_out = apr_table_make(request_.pool, 10);
    request_.notes = apr_table_make(request_.pool, 10);
  }

  void TearDown() override {
    MockApache::ActionsSinceLastCall();
    MockApache::CleanupRequest(&request_);
    MockApache::Terminate();
  }

  // What a server's own stages leave on a request before the handler.
  void Dirty() {
    apr_table_add(request_.headers_out, "Cache-Control", "public");
    apr_table_add(request_.headers_out, "Set-Cookie", "a=1");
    apr_table_add(request_.err_headers_out, "Cache-Control", "public");
    apr_table_add(request_.err_headers_out, "Strict-Transport-Security",
                  "max-age=1");
    request_.content_languages =
        apr_array_make(request_.pool, 1, sizeof(char*));
    *static_cast<const char**>(apr_array_push(request_.content_languages)) =
        "en";
    apr_table_setn(request_.notes, "no-etag", "1");
    apr_table_setn(request_.subprocess_env, "force-no-vary", "1");
  }

  const DaemonHtmlHeaderLines kOk = {
      {"Content-Type", "text/markdown; charset=utf-8"},
      {"Content-Length", "12"},
      {"Cache-Control", "private"},
      {"Vary", "Accept"},
      {"X-Robots-Tag", "noindex"},
      {"Age", "0"},
      {"ETag", "W/\"psm-0123\""}};
  const DaemonHtmlHeaderLines kNotModified = {{"ETag", "W/\"psm-0123\""},
                                              {"Cache-Control", "private"},
                                              {"Vary", "Accept"}};

  request_rec request_;
};

// Every table the server writes from carries the set and nothing else; the
// fields httpd writes headers from are cleared, and the type and length go
// through the request's own fields.
TEST_F(ApacheDaemonHtmlTest, TheSetReplacesEveryTable) {
  Dirty();
  MockApache::ActionsSinceLastCall();
  bool conflict = true;
  ApacheApplyMarkdownHeaders(&request_, kOk, &conflict);
  EXPECT_FALSE(conflict);
  EXPECT_EQ(0, Lines(request_.err_headers_out));
  // Five lines plus Content-Type (the mock's ap_set_content_type writes it).
  EXPECT_EQ(6, Lines(request_.headers_out));
  EXPECT_STREQ("private", apr_table_get(request_.headers_out, "Cache-Control"));
  EXPECT_EQ(nullptr, apr_table_get(request_.headers_out, "Set-Cookie"));
  EXPECT_EQ(nullptr, apr_table_get(request_.headers_out, "Content-Length"));
  EXPECT_EQ(nullptr, request_.content_languages);
  EXPECT_EQ(nullptr, request_.content_encoding);
  EXPECT_EQ(nullptr, apr_table_get(request_.notes, "no-etag"));
  EXPECT_EQ(nullptr, apr_table_get(request_.subprocess_env, "force-no-vary"));
  EXPECT_EQ(
      "ap_set_content_type(text/markdown; charset=utf-8) "
      "ap_set_content_length(12)",
      MockApache::ActionsSinceLastCall());
}

// A 304 names no type.
TEST_F(ApacheDaemonHtmlTest, ANotModifiedCarriesNoType) {
  request_.content_type = "text/html";
  bool conflict = true;
  ApacheApplyMarkdownHeaders(&request_, kNotModified, &conflict);
  EXPECT_FALSE(conflict);
  EXPECT_EQ(nullptr, request_.content_type);
  EXPECT_EQ(3, Lines(request_.headers_out));
}

// An encoding a later filter applied anyway is kept, so the body can still
// be read, and reported.
TEST_F(ApacheDaemonHtmlTest, AnEncodingFoundIsKeptAndReported) {
  apr_table_add(request_.headers_out, "Content-Encoding", "gzip");
  apr_table_add(request_.headers_out, "X-Filter", "1");
  bool conflict = false;
  ApacheApplyMarkdownHeaders(&request_, kOk, &conflict);
  EXPECT_TRUE(conflict);
  EXPECT_STREQ("gzip", apr_table_get(request_.headers_out, "Content-Encoding"));
  EXPECT_EQ(nullptr, apr_table_get(request_.headers_out, "X-Filter"));
}

// A coded copy's own Content-Encoding is the set's: written once, and the
// filter pass that finds it again reports no conflict.  Another coding found
// next to it still does.
TEST_F(ApacheDaemonHtmlTest, TheSetsOwnEncodingIsNoConflict) {
  DaemonHtmlHeaderLines coded = kOk;
  coded.insert(coded.begin() + 2, {"Content-Encoding", "br"});
  bool conflict = true;
  ApacheApplyMarkdownHeaders(&request_, coded, &conflict);
  EXPECT_FALSE(conflict);
  // The handler pass wrote it; the filter pass finds it and keeps one.
  ApacheApplyMarkdownHeaders(&request_, coded, &conflict);
  EXPECT_FALSE(conflict);
  int lines = 0;
  apr_table_do(
      [](void* rec, const char*, const char*) {
        ++*static_cast<int*>(rec);
        return 1;
      },
      &lines, request_.headers_out, "Content-Encoding", nullptr);
  EXPECT_EQ(1, lines);
  EXPECT_STREQ("br", apr_table_get(request_.headers_out, "Content-Encoding"));

  apr_table_add(request_.headers_out, "Content-Encoding", "gzip");
  ApacheApplyMarkdownHeaders(&request_, coded, &conflict);
  EXPECT_TRUE(conflict);
  // Both codings, in the order applied: the body is br, then gzip.
  EXPECT_STREQ("br, gzip",
               apr_table_get(request_.headers_out, "Content-Encoding"));

  // A stage that merged its coding into the copy's own line: kept whole,
  // never "br, br, gzip".
  apr_table_clear(request_.headers_out);
  apr_table_add(request_.headers_out, "Content-Encoding", "br, gzip");
  ApacheApplyMarkdownHeaders(&request_, coded, &conflict);
  EXPECT_TRUE(conflict);
  EXPECT_STREQ("br, gzip",
               apr_table_get(request_.headers_out, "Content-Encoding"));
}

// Only the request's filters below the protocol filters go; the Markdown
// header filter, the protocol filters and the connection's stay.
TEST_F(ApacheDaemonHtmlTest, OnlyContentFiltersAreRemoved) {
  ap_filter_rec_t recs[5] = {};
  const char* names[5] = {"INCLUDES", "DEFLATE",
                          kApacheMarkdownHeadersFilterName, "HTTP_HEADER",
                          "CORE"};
  const int types[5] = {AP_FTYPE_RESOURCE, AP_FTYPE_CONTENT_SET,
                        kApacheMarkdownHeadersFilterType, AP_FTYPE_PROTOCOL,
                        AP_FTYPE_NETWORK};
  ap_filter_t filters[5] = {};
  for (int i = 0; i < 5; ++i) {
    recs[i].name = names[i];
    recs[i].ftype = static_cast<ap_filter_type>(types[i]);
    filters[i].frec = &recs[i];
    filters[i].r = i < 4 ? &request_ : nullptr;
    filters[i].next = i < 4 ? &filters[i + 1] : nullptr;
  }
  request_.output_filters = &filters[0];
  MockApache::ActionsSinceLastCall();
  ApacheRemoveContentFilters(&request_);
  EXPECT_EQ(
      "ap_remove_output_filter(INCLUDES) ap_remove_output_filter(DEFLATE)",
      MockApache::ActionsSinceLastCall());
}

// A port that cannot emit leaves the compression setting as it found it.
TEST_F(ApacheDaemonHtmlTest, AFailedEmitUndoesNoCompress) {
  apr_table_setn(request_.subprocess_env, "no-gzip", "operator");
  ApacheDaemonHtmlInFlight in_flight;
  ApacheDaemonHtmlPort port(&request_, nullptr, &in_flight);
  port.MarkNoCompress();
  EXPECT_STREQ("1", apr_table_get(request_.subprocess_env, "no-gzip"));
  EXPECT_STREQ("1", apr_table_get(request_.subprocess_env, "no-brotli"));
  request_.sent_bodyct = 1;  // Something already went out.
  DaemonHtmlServeResult result;
  result.status = 200;
  result.headers = kOk;
  EXPECT_FALSE(port.ClearAndEmitAllowlisted(result, /*head=*/false));
  EXPECT_STREQ("operator", apr_table_get(request_.subprocess_env, "no-gzip"));
  EXPECT_EQ(nullptr, apr_table_get(request_.subprocess_env, "no-brotli"));
}

// Not a main request: never emitted.
TEST_F(ApacheDaemonHtmlTest, OnlyAMainRequestIsAnswered) {
  request_rec other = {};
  request_.prev = &other;
  EXPECT_FALSE(ApacheDaemonHtmlMainRequest(&request_));
  ApacheDaemonHtmlInFlight in_flight;
  ApacheDaemonHtmlPort port(&request_, nullptr, &in_flight);
  DaemonHtmlServeResult result;
  result.status = 200;
  result.headers = kOk;
  EXPECT_FALSE(port.ClearAndEmitAllowlisted(result, /*head=*/false));
  request_.prev = nullptr;
  request_.main = &other;
  EXPECT_FALSE(ApacheDaemonHtmlMainRequest(&request_));
}

// Every line is read, in order, and the first marker is kept.
TEST_F(ApacheDaemonHtmlTest, TheFirstAjaxMarkerIsKept) {
  apr_table_t* in = apr_table_make(request_.pool, 4);
  EXPECT_EQ("", ApacheDaemonHtmlAjaxMarker(in));
  EXPECT_EQ("", ApacheDaemonHtmlAjaxMarker(nullptr));
  apr_table_add(in, "X-Requested-With", "com.example.app");
  apr_table_add(in, "X-MicrosoftAjax", "");
  EXPECT_EQ("", ApacheDaemonHtmlAjaxMarker(in));
  apr_table_add(in, "x-requested-with", "XMLHttpRequest");
  apr_table_add(in, "X-Prototype-Version", "1.7");
  EXPECT_EQ("X-Requested-With", ApacheDaemonHtmlAjaxMarker(in));
}

// httpd joins repeated request lines into one value (headers_in is merged
// as the request is read): a marker among them is still found.
TEST_F(ApacheDaemonHtmlTest, AMarkerInAMergedLineIsFound) {
  apr_table_t* in = apr_table_make(request_.pool, 4);
  apr_table_merge(in, "X-Requested-With", "com.example.app");
  EXPECT_EQ("", ApacheDaemonHtmlAjaxMarker(in));
  apr_table_merge(in, "X-Requested-With", "XMLHttpRequest");
  EXPECT_STREQ("com.example.app, XMLHttpRequest",
               apr_table_get(in, "X-Requested-With"));
  EXPECT_EQ("X-Requested-With", ApacheDaemonHtmlAjaxMarker(in));
}

// A decoded path is a key only when the first "?" of the key is the real
// boundary of the query: "/a%3Fb" decodes to "/a?b", which would read as
// the path "/a" with the query "b".
TEST(ApacheDaemonHtmlKeyablePathTest, OnlyAPathThatCannotBeReadAsAnother) {
  EXPECT_TRUE(ApacheDaemonHtmlKeyablePath("/"));
  EXPECT_TRUE(ApacheDaemonHtmlKeyablePath("/a/b.html"));
  EXPECT_TRUE(ApacheDaemonHtmlKeyablePath("/caf\xc3\xa9"));
  EXPECT_FALSE(ApacheDaemonHtmlKeyablePath(""));
  EXPECT_FALSE(ApacheDaemonHtmlKeyablePath("page"));
  EXPECT_FALSE(ApacheDaemonHtmlKeyablePath("/a?b"));
  EXPECT_FALSE(ApacheDaemonHtmlKeyablePath("/a#b"));
  EXPECT_FALSE(ApacheDaemonHtmlKeyablePath("/a b"));
  EXPECT_FALSE(ApacheDaemonHtmlKeyablePath(StringPiece("/a\0b", 4)));
  EXPECT_FALSE(ApacheDaemonHtmlKeyablePath("/a\tb"));
  EXPECT_FALSE(ApacheDaemonHtmlKeyablePath("/a\x7f"));
}

// The wait for work in flight is bounded, and takes no lock.
TEST(ApacheDaemonHtmlInFlightTest, TheWaitIsBounded) {
  ApacheDaemonHtmlInFlight in_flight;
  EXPECT_TRUE(in_flight.WaitIdleFor(0));
  in_flight.Enter();
  EXPECT_FALSE(in_flight.WaitIdleFor(20));
  in_flight.Leave();
  EXPECT_TRUE(in_flight.WaitIdleFor(20));
}

}  // namespace
}  // namespace net_instaweb
