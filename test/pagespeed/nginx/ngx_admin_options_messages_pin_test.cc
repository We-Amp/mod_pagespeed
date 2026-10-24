/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Source pins for three request-path behaviours: an admin POST refused
// before its shared handler ran must complete the base fetch created for
// it (an abandoned one holds the worker's shutdown drain for its full 30 s
// budget), a request whose script-variable option value is rejected must
// free its per-request option copies and be served without optimization,
// and the messages handler's status and Allow header must reach the client
// instead of a fixed 200. The port cannot be linked into a unit test (see
// test/pagespeed/nginx/BUILD), so these read the sources, the same idiom
// as the other pin tests here. StripWhitespace strips whitespace, not
// comments, so a pinned token may not appear in a comment either.

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

size_t CountOccurrences(const GoogleString& text, const GoogleString& needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = text.find(needle, pos)) != GoogleString::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

// An admin POST whose body cannot be used is refused with 413 before its
// shared handler ran; every such refusal must complete the base fetch that
// was created for the request.
TEST(NgxAdminOptionsMessagesPinTest, RefusedAdminPostCompletesItsBaseFetch) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");

  // The completion helper: it records the refusal's status on the fetch
  // (so the completion is not counted as a resource 404), finishes the
  // fetch with a failure and releases the request's reference to it.
  const size_t helper =
      cc.find("voidps_abandon_admin_base_fetch(ps_request_ctx_t*ctx){");
  ASSERT_NE(GoogleString::npos, helper)
      << "ps_abandon_admin_base_fetch was renamed; this pin needs updating";
  const size_t helper_open = cc.find('{', helper);
  const size_t helper_close = MatchingBrace(cc, helper_open);
  ASSERT_NE(GoogleString::npos, helper_close);
  const GoogleString helper_body =
      cc.substr(helper_open, helper_close - helper_open);
  EXPECT_NE(GoogleString::npos,
            helper_body.find("HttpStatus::kEntityTooLarge);"));
  EXPECT_NE(GoogleString::npos,
            helper_body.find("ctx->base_fetch->Done(false/*failure*/);"));
  EXPECT_NE(GoogleString::npos,
            helper_body.find("ps_release_base_fetch(ctx);"));

  // Both 413 returns of the admin branch go through the helper, and no 413
  // return bypasses it: a return without the helper abandons the fetch,
  // which nothing else ever completes.
  const size_t handler = cc.find(
      "ngx_int_tps_resource_handler(ngx_http_request_t*r,boolhtml_rewrite,"
      "RequestRouting::Responseresponse_category){");
  ASSERT_NE(GoogleString::npos, handler)
      << "ps_resource_handler was renamed; this pin needs updating";
  const size_t handler_open = cc.find('{', handler);
  const size_t handler_close = MatchingBrace(cc, handler_open);
  ASSERT_NE(GoogleString::npos, handler_close);
  const GoogleString handler_body =
      cc.substr(handler_open, handler_close - handler_open);
  const size_t completed_refusals =
      CountOccurrences(handler_body,
                       "ps_abandon_admin_base_fetch(ctx);"
                       "returnNGX_HTTP_REQUEST_ENTITY_TOO_LARGE;");
  const size_t all_refusals = CountOccurrences(
      handler_body, "returnNGX_HTTP_REQUEST_ENTITY_TOO_LARGE;");
  EXPECT_EQ(2u, completed_refusals)
      << "each 413 return of the admin branch must complete the fetch first";
  EXPECT_EQ(all_refusals, completed_refusals)
      << "a 413 return bypasses ps_abandon_admin_base_fetch";
}

// A script variable that evaluates to a value the configuration rejects
// must free the option copies made for the request, and the request must
// be served without optimization instead of failing it.
TEST(NgxAdminOptionsMessagesPinTest, ScriptVariableFailureFreesAndDeclines) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");

  // The option determination frees what it allocated for this request
  // before reporting the failure.
  const size_t determine = cc.find(
      "boolps_determine_options(ngx_http_request_t*r,"
      "RequestHeaders*request_headers,");
  ASSERT_NE(GoogleString::npos, determine)
      << "ps_determine_options was renamed; this pin needs updating";
  const size_t determine_open = cc.find('{', determine);
  const size_t determine_close = MatchingBrace(cc, determine_open);
  ASSERT_NE(GoogleString::npos, determine_close);
  const GoogleString determine_body =
      cc.substr(determine_open, determine_close - determine_open);
  EXPECT_NE(GoogleString::npos,
            determine_body.find(
                "if(!ngx_options->ExecuteScriptVariables(r,cfg_s->handler,"
                "ngx_factory)){delete*options;*options=nullptr;"
                "deleterequest_options;returnfalse;}"))
      << "the ExecuteScriptVariables failure must free the option copies";

  // The caller declines the request: nginx then serves it without
  // optimization, where an NGX_ERROR would close the connection.
  const size_t handler = cc.find(
      "ngx_int_tps_resource_handler(ngx_http_request_t*r,boolhtml_rewrite,"
      "RequestRouting::Responseresponse_category){");
  ASSERT_NE(GoogleString::npos, handler);
  const size_t handler_open = cc.find('{', handler);
  const size_t handler_close = MatchingBrace(cc, handler_open);
  ASSERT_NE(GoogleString::npos, handler_close);
  const GoogleString handler_body =
      cc.substr(handler_open, handler_close - handler_open);
  const size_t call = handler_body.find(
      "if(!ps_determine_options(r,request_headers.get(),"
      "response_headers.get(),&options,request_context,cfg_s,&url,"
      "&pagespeed_query_params,&pagespeed_option_cookies,html_rewrite)){");
  ASSERT_NE(GoogleString::npos, call)
      << "the ps_determine_options call changed shape; this pin needs "
         "updating";
  const size_t call_open = handler_body.find('{', call);
  const size_t call_close = MatchingBrace(handler_body, call_open);
  ASSERT_NE(GoogleString::npos, call_close);
  const GoogleString call_body =
      handler_body.substr(call_open, call_close - call_open);
  EXPECT_NE(GoogleString::npos, call_body.find("returnNGX_DECLINED;"))
      << "an undeterminable options set must decline the request";
  EXPECT_EQ(GoogleString::npos, call_body.find("returnNGX_ERROR;"))
      << "an undeterminable options set must not fail the request";
}

// The messages handler's answer carries the status the shared handler set
// (405 for a non-GET grouped request, 429 when busy) and its Allow header,
// not a fixed 200 without Allow.
TEST(NgxAdminOptionsMessagesPinTest, MessagesStatusAndAllowReachTheClient) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const size_t handler = cc.find(
      "ngx_int_tps_simple_handler(ngx_http_request_t*r,"
      "NgxServerContext*server_context,RequestRouting::Responseresponse_"
      "category){");
  ASSERT_NE(GoogleString::npos, handler)
      << "ps_simple_handler was renamed; this pin needs updating";
  const size_t handler_open = cc.find('{', handler);
  const size_t handler_close = MatchingBrace(cc, handler_open);
  ASSERT_NE(GoogleString::npos, handler_close);
  const GoogleString handler_body =
      cc.substr(handler_open, handler_close - handler_open);

  // The messages case takes the shared handler's status and Allow header
  // from its fetch, like it already did for the content type and the cache
  // policy.
  const size_t messages = handler_body.find("caseRequestRouting::kMessages:{");
  ASSERT_NE(GoogleString::npos, messages)
      << "the messages case of ps_simple_handler changed shape";
  const size_t messages_open = handler_body.find('{', messages);
  const size_t messages_close = MatchingBrace(handler_body, messages_open);
  ASSERT_NE(GoogleString::npos, messages_close);
  const GoogleString messages_body =
      handler_body.substr(messages_open, messages_close - messages_open);
  EXPECT_NE(GoogleString::npos,
            messages_body.find("constintfetch_status=fetch.response_headers()->"
                               "status_code();"))
      << "the messages case must read the shared handler's status";
  EXPECT_NE(
      GoogleString::npos,
      messages_body.find("status=static_cast<HttpStatus::Code>(fetch_status);"))
      << "the messages case must take the shared handler's status";
  EXPECT_NE(
      GoogleString::npos,
      messages_body.find("fetch_allow=fetch.response_headers()->LookupJoined("
                         "HttpAttributes::kAllow);"))
      << "the messages case must read the shared handler's Allow header";

  // The Allow header is carried into the response the module sends.
  EXPECT_NE(GoogleString::npos,
            handler_body.find("if(!fetch_allow.empty()){response_headers.Add("
                              "HttpAttributes::kAllow,fetch_allow);}"))
      << "the response must carry the Allow header the shared handler set";
}

}  // namespace
}  // namespace net_instaweb
