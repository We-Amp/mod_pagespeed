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

// Source pins for the admin POST request-body handoff.
//
// nginx hands a request-body post handler a request the content phase has
// already yielded, so the post handler must finish the request itself, with
// a definite result; and the content phase must arm the body read at most
// once per request, because a second arm would invoke the post handler
// synchronously from inside the phase. A body another module already read
// is not this module's arm and must not be declined. There is no linkable
// unit test for this (nginx is a headers-only library in this build graph;
// see the comment at the top of test/pagespeed/nginx/BUILD), so the handoff
// is pinned by reading the source, the same idiom as the other pin tests
// here.
//
// A source pin is weaker than an execution pin and is written knowing that:
// it catches the removal of the status mapping or of the read guard, which
// are the realistic regressions, and it does not catch a handler that runs
// and does the wrong thing. StripWhitespace strips whitespace, not
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

const char kNgxPagespeedCc[] = "pagespeed/nginx/ngx_pagespeed.cc";

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

class NgxAdminPostPinTest : public testing::Test {};

TEST_F(NgxAdminPostPinTest, DeclinedAdminPostIsFinalizedNotYielded) {
  const GoogleString cc = ReadStrippedSource(kNgxPagespeedCc);
  // The body post handler for an admin POST. It runs once the body has been
  // read, so whatever ps_resource_handler decides must become the answer.
  const size_t handler =
      cc.find("voidps_admin_body_handler(ngx_http_request_t*r){");
  ASSERT_NE(GoogleString::npos, handler)
      << "ps_admin_body_handler was renamed; this pin needs updating";
  const size_t open = cc.find('{', handler);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);

  // A decline is mapped to the status the admin location itself answers
  // with, and the request is finished through the finalize helper after
  // the mapping.
  const size_t mapped =
      body.find("if(rc==NGX_DECLINED){rc=NGX_HTTP_NOT_FOUND;}");
  EXPECT_NE(GoogleString::npos, mapped)
      << "ps_admin_body_handler no longer maps NGX_DECLINED to a definite "
         "status";
  const size_t finalized = body.find("ngx_http_finalize_request(r,rc);");
  EXPECT_NE(GoogleString::npos, finalized)
      << "ps_admin_body_handler no longer finishes the request through "
         "ngx_http_finalize_request";
  EXPECT_LT(mapped, finalized)
      << "the mapping must happen before the request is finished";

  // The mapping must feed the finalize guard directly, with nothing in
  // between that could turn rc back into a decline. On the pre-fix source
  // the raw rc reached ngx_http_finalize_request, so this pin goes red
  // there; the re-entry it caused re-armed the read from the content
  // phase, not from here.
  EXPECT_NE(GoogleString::npos,
            body.find("if(rc==NGX_DECLINED){rc=NGX_HTTP_NOT_FOUND;}"
                      "if(rc!=NGX_DONE){"))
      << "the decline mapping must be the last step before the finalize "
         "guard";

  // The post handler never arms another body read. This is a guard against
  // a direct read call being added here; it cannot go red on the pre-fix
  // source, where the re-entry ran through the phase engine instead.
  EXPECT_EQ(GoogleString::npos, body.find("ngx_http_read_client_request_body"))
      << "ps_admin_body_handler must not start a second body read";
}

TEST_F(NgxAdminPostPinTest, ContentPhaseArmsTheAdminBodyReadAtMostOnce) {
  const GoogleString cc = ReadStrippedSource(kNgxPagespeedCc);
  const size_t handler =
      cc.find("ngx_int_tps_content_handler(ngx_http_request_t*r){");
  ASSERT_NE(GoogleString::npos, handler)
      << "ps_content_handler was renamed; this pin needs updating";
  const size_t open = cc.find('{', handler);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);

  const size_t read =
      body.find("ngx_http_read_client_request_body(r,ps_admin_body_handler);");
  ASSERT_NE(GoogleString::npos, read)
      << "the admin POST arm no longer reads the body in the content phase";
  // The guard against arming a second read runs before the arm itself, and
  // declines only a read this module armed itself: a request body another
  // module read carries that module's post handler and must reach the arm.
  const size_t guard = body.find(
      "if(r->request_body!=nullptr&&"
      "r->request_body->post_handler==ps_admin_body_handler){"
      "returnNGX_DECLINED;}");
  EXPECT_NE(GoogleString::npos, guard)
      << "ps_content_handler no longer declines a second arm of its own "
         "body read";
  EXPECT_LT(guard, read) << "the own-arm guard must run before arming the read";
}

}  // namespace
}  // namespace net_instaweb
