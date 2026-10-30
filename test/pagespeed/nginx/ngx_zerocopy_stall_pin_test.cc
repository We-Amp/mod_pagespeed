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

// Source pins for the zero-copy HTTP/2 stall handling. The port
// cannot be linked into a unit test (see test/pagespeed/nginx/BUILD), so
// these read the sources. The behaviour itself is covered by
// test/system/nginx/zerocopy_ring_stall_smoke.sh.

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

TEST(NgxZeroCopyStallPinTest, RefillTimerEndsAStreamThatStoppedReading) {
  // The refill timer drives the writer, and every writer pass re-arms
  // send_timeout, so the timer itself must end a stream that makes no
  // progress, before it runs the writer again.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const size_t fn = cc.find("voidps_bounded_ring_refill_event(");
  ASSERT_NE(GoogleString::npos, fn) << "the refill handler was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  const size_t stalled = body.find("ps_bounded_ring_stalled(");
  const size_t finalize =
      body.find("ngx_http_finalize_request(r,NGX_HTTP_REQUEST_TIME_OUT);");
  const size_t writer = body.find("r->write_event_handler(r);");
  ASSERT_NE(GoogleString::npos, stalled);
  ASSERT_NE(GoogleString::npos, finalize);
  ASSERT_NE(GoogleString::npos, writer);
  EXPECT_LT(stalled, finalize);
  EXPECT_LT(finalize, writer);
  EXPECT_NE(GoogleString::npos, body.find("stall_stat->Add(1);"));
}

TEST(NgxZeroCopyStallPinTest, RefillTimerRefillsBeforeItDrivesTheWriter) {
  // nginx's postpone filter drops a NULL-input pass once nothing is queued on
  // the connection, so the timer must refill the ring itself and pass the new
  // windows down, or a client that drains everything in flight starves.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const size_t fn = cc.find("voidps_bounded_ring_refill_event(");
  ASSERT_NE(GoogleString::npos, fn) << "the refill handler was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  const size_t refill = body.find("ps_bounded_ring_refill(r,ring,&refills)");
  const size_t pass = body.find("ngx_http_next_body_filter(r,refills)");
  const size_t writer = body.find("r->write_event_handler(r);");
  ASSERT_NE(GoogleString::npos, refill);
  ASSERT_NE(GoogleString::npos, pass);
  ASSERT_NE(GoogleString::npos, writer);
  // Module output enters at the module's own position, not the chain top.
  EXPECT_EQ(GoogleString::npos, body.find("ngx_http_output_filter("));
  EXPECT_LT(refill, pass);
  EXPECT_LT(pass, writer);
}

TEST(NgxZeroCopyStallPinTest, ProgressIsMeasuredInBytesTheClientAccepted) {
  // A slow client that keeps reading must keep resetting the clock: the
  // measure is emitted bytes minus bytes still in flight, and the limit is
  // nginx's own send_timeout.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const size_t fn = cc.find("boolps_bounded_ring_stalled(");
  ASSERT_NE(GoogleString::npos, fn) << "the stall check was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  EXPECT_NE(GoogleString::npos, body.find("ring->cursor-in_flight"));
  EXPECT_NE(GoogleString::npos, body.find("clcf->send_timeout"));
}

TEST(NgxZeroCopyStallPinTest, TheStatIsWiredIntoTheRing) {
  const GoogleString fetch =
      ReadStrippedSource("pagespeed/nginx/ngx_base_fetch.cc");
  EXPECT_NE(GoogleString::npos,
            fetch.find("FindVariable(\"zerocopy_serve_stall_timeouts\")"));
}

}  // namespace
}  // namespace net_instaweb
