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

// Source pins for the request method and the cache gate in the nginx port.
// The port cannot be linked into a unit test (see test/pagespeed/nginx/BUILD),
// so these read the sources. StripWhitespace strips whitespace, not comments,
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

// The helper that builds the PageSpeed request headers from the nginx
// request records the request method.
TEST(NgxCacheMethodPinTest, PropagatesRequestMethod) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const GoogleString body = FunctionBody(
      cc,
      "copy_request_headers_from_ngx(constngx_http_request_t*r,"
      "RequestHeaders*headers){");
  ASSERT_FALSE(body.empty()) << "copy_request_headers_from_ngx was renamed";
  EXPECT_NE(GoogleString::npos,
            body.find("headers->set_method(RequestHeaders::MethodFromString("
                      "str_to_string_piece(r->method_name)))"));
}

// The domain-mapped proxy branch is entered for GET and HEAD only, so a
// request with another method is never looked up in or stored in the cache.
TEST(NgxCacheMethodPinTest, ServesOnlyGetAndHeadFromCache) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const size_t branch =
      cc.find("response_category==RequestRouting::kResource){");
  ASSERT_NE(GoogleString::npos, branch) << "the proxy branch moved";
  const size_t map_call = cc.find("MapOriginUrl(url,&mapped_url", branch);
  ASSERT_NE(GoogleString::npos, map_call);
  const GoogleString guard = cc.substr(branch, map_call - branch);
  EXPECT_NE(GoogleString::npos,
            guard.find("(r->method==NGX_HTTP_GET||r->method==NGX_HTTP_HEAD)&&"))
      << "the proxy branch no longer checks the request method";
}

// The HTML pipeline runs a HEAD request as the GET it answers, so a HEAD gets
// the same response headers (the Vary tokens included) as the GET.
TEST(NgxCacheMethodPinTest, HtmlHeadRunsAsGet) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const size_t html = cc.find("if(html_rewrite&&options->IsAllowed(url.Spec())){");
  ASSERT_NE(GoogleString::npos, html) << "the HTML branch moved";
  const size_t create = cc.find("ps_create_base_fetch(", html);
  ASSERT_NE(GoogleString::npos, create);
  const GoogleString before = cc.substr(html, create - html);
  EXPECT_NE(GoogleString::npos,
            before.find("if(request_headers->method()==RequestHeaders::kHead)"
                        "{request_headers->set_method(RequestHeaders::kGet);}"))
      << "the HTML branch no longer runs a HEAD as a GET";
}

}  // namespace
}  // namespace net_instaweb
