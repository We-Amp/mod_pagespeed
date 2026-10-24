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

// Source pins for the request-body discard on module-answered responses.
// nginx requires a content handler that does not use the request body to
// discard it before sending its own response, so every path where the module
// answers the request itself must discard before its header send. The port
// cannot be linked into a unit test (see test/pagespeed/nginx/BUILD), so
// these read the sources. StripWhitespace strips whitespace, not comments,
// so a pinned token may not appear in a comment either.

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

// Returns the substring of the function definition starting at `def_pos`
// (the position of its name), or "" if the definition was renamed.
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

// Every module-answered path discards through one helper, which hands the
// request to nginx's own discard and keeps the connection from being reused
// when that fails.
TEST(NgxRequestBodyDiscardPinTest, DiscardHelperFallsBackToClosing) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const GoogleString body =
      FunctionBody(cc, "ps_discard_request_body(ngx_http_request_t*r){");
  ASSERT_FALSE(body.empty()) << "ps_discard_request_body was renamed";
  EXPECT_NE(GoogleString::npos, body.find("ngx_http_discard_request_body(r)"));
  EXPECT_NE(GoogleString::npos, body.find("r->keepalive=0;"));
  EXPECT_NE(GoogleString::npos, body.find("r->lingering_close=1;"));
}

// Static assets and messages: ps_simple_handler answers the request itself,
// so the discard must precede its single header-and-body send.
TEST(NgxRequestBodyDiscardPinTest, SimpleHandlerDiscardsBeforeSend) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const GoogleString body =
      FunctionBody(cc, "ps_simple_handler(ngx_http_request_t*r,");
  ASSERT_FALSE(body.empty()) << "ps_simple_handler was renamed";
  const size_t discard = body.find("ps_discard_request_body(r);");
  const size_t send = body.find(
      "send_out_headers_and_body(r,response_headers,"
      "output);");
  ASSERT_NE(GoogleString::npos, discard);
  ASSERT_NE(GoogleString::npos, send);
  EXPECT_LT(discard, send);
}

// The webbotauth counter: the case answers the request itself once the hide
// variant has declined, so the discard must precede its send.
TEST(NgxRequestBodyDiscardPinTest, WebBotAuthCounterDiscardsBeforeSend) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const GoogleString content_handler =
      FunctionBody(cc, "ps_content_handler(ngx_http_request_t*r){");
  ASSERT_FALSE(content_handler.empty()) << "ps_content_handler was renamed";
  const size_t case_open =
      content_handler.find("caseRequestRouting::kWebBotAuthCounter:{");
  ASSERT_NE(GoogleString::npos, case_open);
  const size_t open = content_handler.find('{', case_open);
  const size_t close = MatchingBrace(content_handler, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = content_handler.substr(open, close - open);
  const size_t discard = body.find("ps_discard_request_body(r);");
  const size_t send = body.find(
      "send_out_headers_and_body(r,response_headers,"
      "body);");
  ASSERT_NE(GoogleString::npos, discard);
  ASSERT_NE(GoogleString::npos, send);
  EXPECT_LT(discard, send);

  // The discard belongs at the answering sites, not at the top of the
  // content handler: requests the module declines keep flowing to the
  // server's own handlers (proxy_pass, fastcgi), which do read the body.
  const size_t route = content_handler.find("=ps_route_request(r);");
  const size_t first_discard =
      content_handler.find("ps_discard_request_body(r);");
  ASSERT_NE(GoogleString::npos, route);
  ASSERT_NE(GoogleString::npos, first_discard);
  EXPECT_LT(route, first_discard);
}

// The daemon serve arm: every branch that serves from the volume passes one
// send; the discard must sit after the last decline and before that send.
TEST(NgxRequestBodyDiscardPinTest, DaemonServeDiscardsBeforeSend) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const GoogleString body =
      FunctionBody(cc, "PsServeFromDaemonSubstrate(ngx_http_request_t*r,");
  ASSERT_FALSE(body.empty()) << "PsServeFromDaemonSubstrate was renamed";
  const size_t discard = body.find("ps_discard_request_body(r);");
  const size_t send = body.find("ngx_http_send_header(r);");
  ASSERT_NE(GoogleString::npos, discard);
  ASSERT_NE(GoogleString::npos, send);
  EXPECT_LT(discard, send);
  // None of the fall-through declines may sit after the discard.
  const size_t last_decline = body.rfind("returnNGX_DECLINED;");
  ASSERT_NE(GoogleString::npos, last_decline);
  EXPECT_LT(last_decline, discard);
}

// The base fetch handler is the shared send for pagespeed resources,
// MapProxyDomain proxying, admin GETs, cache purge and in-place hits: the
// discard must precede the header collection, but be skipped wherever the
// module does not answer the request itself: for the in-place lookup miss,
// which declines inside the header filter chain and leaves the request to
// the server's own handler, and for the HTML transform, which only
// rewrites an upstream response whose content handler owns the body.
TEST(NgxRequestBodyDiscardPinTest, BaseFetchHandlerDiscardsBeforeHeaders) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const GoogleString body =
      FunctionBody(cc, "ps_base_fetch_handler(ngx_http_request_t*r){");
  ASSERT_FALSE(body.empty()) << "ps_base_fetch_handler was renamed";
  EXPECT_NE(GoogleString::npos, body.find("constboolipro_lookup_miss="));
  // The guard must carry the HTML transform exclusion: a lookup-miss-only
  // guard would also discard the body of upstream responses the module
  // merely filters.
  const size_t discard = body.find(
      "if(!ipro_lookup_miss&&"
      "ctx->base_fetch->base_fetch_type()!=kHtmlTransform){"
      "ps_discard_request_body(r);}");
  ASSERT_NE(GoogleString::npos, discard)
      << "the discard guard lost the kHtmlTransform exclusion";
  const size_t collect = body.find("CollectHeaders(&r->headers_out);");
  ASSERT_NE(GoogleString::npos, collect);
  EXPECT_LT(discard, collect);
}

}  // namespace
}  // namespace net_instaweb
