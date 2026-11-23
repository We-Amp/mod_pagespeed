// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// Source pins for two things the native fetcher of the nginx port does to
// every fetch it makes on the module's behalf: what it records as the
// original content length, and the marker it puts in the User-Agent.  The
// fetcher runs on nginx's event loop and cannot be linked into a unit test
// (see test/pagespeed/nginx/BUILD), so these read the source.
// ReadStrippedSource strips whitespace, not comments, so a pinned token may
// not appear in a comment either.
//
// What the pins say, in words:
//
//  - With length tracking on, X-Original-Content-Length is taken from the
//    response's Content-Length header when the headers arrive, before any
//    body byte is read.  Every fetch is wrapped in an inflating fetch, which
//    inflates a gzip response when the caller did not ask for gzip itself,
//    but that happens to the body afterwards: the length is recorded before
//    the wrapper inflates anything.  So for a compressed response it is the
//    COMPRESSED length, not the size of the resource.  The fetcher based on
//    the HTTP library records something else for the same responses (see
//    curl_url_async_fetcher_test.cc); the difference is tracked in issue
//    #892.
//  - A response without a Content-Length (the fetcher sends HTTP/1.0
//    requests, so that is a close-delimited body) gets no such header from
//    the fetcher at all.
//  - When the fetch completes, the number of body bytes received is recorded
//    only where the header is already present, and into the fetch's extra
//    headers rather than its response headers.
//  - Every request carries the module's marker in its User-Agent.

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

// Returns the position of the brace closing the block opened at `open_pos`,
// or npos. Counts braces only.
size_t MatchingBrace(const GoogleString& text, size_t open_pos) {
  int depth = 0;
  for (size_t i = open_pos; i < text.size(); ++i) {
    if (text[i] == '{') {
      ++depth;
    } else if (text[i] == '}') {
      if (--depth == 0) {
        return i;
      }
    }
  }
  return GoogleString::npos;
}

// Returns the body of the function whose definition starts with `def`, or ""
// if the definition was renamed.
GoogleString FunctionBody(const GoogleString& cc, const char* def) {
  const size_t fn = cc.find(def);
  if (fn == GoogleString::npos) {
    return "";
  }
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  CHECK(close != GoogleString::npos);
  return cc.substr(open, close - open);
}

const char kFetchSource[] = "pagespeed/nginx/ngx_fetch.cc";

TEST(NgxFetchOriginalLengthPinTest, HeaderTimeValueIsTheContentLengthHeader) {
  const GoogleString cc = ReadStrippedSource(kFetchSource);
  const GoogleString body =
      FunctionBody(cc, "boolNgxFetch::HandleHeader(ngx_connection_t*c){");
  ASSERT_FALSE(body.empty()) << "NgxFetch::HandleHeader was renamed";

  // The length comes from the response's own Content-Length header ...
  const size_t find_length = body.find(
      "fetch->async_fetch_->response_headers()->FindContentLength("
      "&fetch->content_length_)");
  ASSERT_NE(GoogleString::npos, find_length);
  // ... and that value, only when it is known, is what gets recorded.
  const GoogleString record_statement =
      "if(fetch->fetcher_->track_original_content_length()&&"
      "fetch->content_length_known_){"
      "fetch->async_fetch_->response_headers()->SetOriginalContentLength("
      "fetch->content_length_);}";
  const size_t record = body.find(record_statement);
  ASSERT_NE(GoogleString::npos, record);
  EXPECT_LT(find_length, record);
  // It is recorded before the body handler is installed.
  const size_t body_handler =
      body.find("fetch->set_response_handler(NgxFetch::HandleBody);");
  ASSERT_NE(GoogleString::npos, body_handler);
  EXPECT_LT(record, body_handler);
  // Nowhere else in the header handler.
  EXPECT_EQ(GoogleString::npos,
            body.substr(0, record).find("SetOriginalContentLength"));
  EXPECT_EQ(GoogleString::npos, body.find("SetOriginalContentLength",
                                          record + record_statement.size()));
}

TEST(NgxFetchOriginalLengthPinTest, CompletionRecordsBytesOnlyOverAHeader) {
  const GoogleString cc = ReadStrippedSource(kFetchSource);
  const GoogleString body =
      FunctionBody(cc, "voidNgxFetch::CallbackDone(boolsuccess){");
  ASSERT_FALSE(body.empty()) << "NgxFetch::CallbackDone was renamed";
  EXPECT_NE(GoogleString::npos,
            body.find("if(fetcher_->track_original_content_length()&&"
                      "async_fetch_->response_headers()->Has("
                      "HttpAttributes::kXOriginalContentLength)){"
                      "async_fetch_->extra_response_headers()"
                      "->SetOriginalContentLength(bytes_received_);}"));
}

TEST(NgxFetchOriginalLengthPinTest, LengthIsRecordedBeforeTheWrapperInflates) {
  // The fetch itself has no inflater and looks at no content coding: the
  // bytes it counts and the Content-Length it records are the transfer's.
  const GoogleString cc = ReadStrippedSource(kFetchSource);
  EXPECT_EQ(GoogleString::npos, cc.find("GzipInflater"));
  EXPECT_EQ(GoogleString::npos, cc.find("kContentEncoding"));
  EXPECT_EQ(GoogleString::npos, cc.find("InflatingFetch"));

  // Inflation is done by a wrapper around the caller's fetch, installed by
  // the fetcher before the native fetch is created.  The native fetch writes
  // the recorded length into the response headers it shares with that
  // wrapper while the headers arrive, so the value is fixed before the
  // wrapper sees a body byte.
  const GoogleString fetcher_cc =
      ReadStrippedSource("pagespeed/nginx/ngx_url_async_fetcher.cc");
  const size_t wrap =
      fetcher_cc.find("async_fetch=EnableInflation(async_fetch);");
  ASSERT_NE(GoogleString::npos, wrap);
  const size_t create = fetcher_cc.find(
      "NgxFetch*fetch=newNgxFetch(url,async_fetch,message_handler,log_);");
  ASSERT_NE(GoogleString::npos, create);
  EXPECT_LT(wrap, create);
  // Exactly one place wraps, and exactly one creates.
  EXPECT_EQ(GoogleString::npos, fetcher_cc.find("EnableInflation(", wrap + 1));
  EXPECT_EQ(GoogleString::npos, fetcher_cc.find("newNgxFetch(", create + 1));
}

TEST(NgxFetchOriginalLengthPinTest, EveryRequestCarriesTheModuleMarker) {
  const GoogleString cc = ReadStrippedSource(kFetchSource);
  const GoogleString init = FunctionBody(cc, "intNgxFetch::InitRequest(){");
  ASSERT_FALSE(init.empty()) << "NgxFetch::InitRequest was renamed";
  EXPECT_NE(GoogleString::npos, init.find("FixUserAgent();"));

  const GoogleString fix = FunctionBody(cc, "voidNgxFetch::FixUserAgent(){");
  ASSERT_FALSE(fix.empty()) << "NgxFetch::FixUserAgent was renamed";
  EXPECT_NE(GoogleString::npos,
            fix.find("StrCat(\"(\",kModPagespeedSubrequestUserAgent,"));
  EXPECT_NE(
      GoogleString::npos,
      fix.find("request_headers->Add(HttpAttributes::kUserAgent,user_agent);"));
}

}  // namespace
}  // namespace net_instaweb
