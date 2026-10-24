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

// Source pins for nginx teardown and allocation invariants. The port
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

TEST(NgxTeardownPinTest, ShutDownDoesNotIterateTheLiveActivePool) {
  // Completing a fetch removes it from the active pool, so the pool must be
  // snapshotted before its fetches are completed.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_url_async_fetcher.cc");
  const size_t fn = cc.find("voidNgxUrlAsyncFetcher::ShutDown(){");
  ASSERT_NE(GoogleString::npos, fn) << "ShutDown was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  EXPECT_EQ(GoogleString::npos, body.find("active_fetches_.begin()"));
  EXPECT_NE(GoogleString::npos, body.find("CancelActiveFetches();"));
}

TEST(NgxTeardownPinTest, BodyPinsAreAllocatedWithoutThrowing) {
  // These allocations run on nginx's C call stack, where an exception
  // cannot unwind.
  const GoogleString cc = ReadStrippedSource("pagespeed/nginx/ngx_base_fetch.cc");
  EXPECT_EQ(GoogleString::npos, cc.find("=newMappedSharedString("));
  EXPECT_NE(GoogleString::npos,
            cc.find("=new(std::nothrow)MappedSharedString("));
}

TEST(NgxTeardownPinTest, ClassicRecordingFinishesOffTheEventThread) {
  // A complete classic recording ends in the HTTP cache write, which can
  // compress and wait on cache locks. The body filter must hand it to the
  // classic record pool; the only inline call left with response headers is
  // the incomplete or failed one, which does no I/O.
  const GoogleString cc = ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const size_t fn = cc.find(
      "ngx_int_tps_in_place_body_filter(ngx_http_request_t*r,ngx_chain_t*in){");
  ASSERT_NE(GoogleString::npos, fn) << "ps_in_place_body_filter was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  const size_t gate =
      body.find("elseif(cl->buf->last_buf&&!recorder->failed()){");
  ASSERT_NE(GoogleString::npos, gate) << "classic hand-off arm is gone";
  const size_t handoff = body.find(
      "g_classic_record_sequence->Add(newDaemonRecordCompletion(", gate);
  EXPECT_NE(GoogleString::npos, handoff);
  const size_t inline_call =
      body.find("ctx->recorder->DoneAndSetHeaders(&response_headers,");
  ASSERT_NE(GoogleString::npos, inline_call);
  EXPECT_GT(inline_call, handoff)
      << "a complete classic recording finishes inline again";
  EXPECT_EQ(GoogleString::npos,
            body.find("ctx->recorder->DoneAndSetHeaders(&response_headers,",
                      inline_call + 1));
}

}  // namespace
}  // namespace net_instaweb
