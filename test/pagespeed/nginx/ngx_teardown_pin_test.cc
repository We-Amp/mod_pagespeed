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

TEST(NgxTeardownPinTest, FetcherShutDownTearsDownUnderTheLock) {
  // ShutDown races rewrite threads that are inside Fetch: the shutdown flag
  // must be published, and the event connection torn down, under the same
  // lock Fetch queues fetches under.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_url_async_fetcher.cc");
  const size_t fn = cc.find("voidNgxUrlAsyncFetcher::ShutDown(){");
  ASSERT_NE(GoogleString::npos, fn) << "ShutDown was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  EXPECT_NE(GoogleString::npos, body.find("ScopedMutexlock(mutex_);"));
  EXPECT_NE(GoogleString::npos, body.find("deleteevent_connection_;"));
}

TEST(NgxTeardownPinTest, FetchRechecksShutdownUnderTheLock) {
  // A fetch must not be queued, nor an event written, unless the shutdown
  // check under the fetcher's lock has passed: outside the lock, ShutDown
  // may delete the event connection at any moment.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_url_async_fetcher.cc");
  const size_t fn = cc.find("voidNgxUrlAsyncFetcher::Fetch(");
  ASSERT_NE(GoogleString::npos, fn) << "Fetch was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  const size_t lock = body.find("ScopedMutexlock(mutex_);");
  ASSERT_NE(GoogleString::npos, lock);
  const size_t check = body.find("if(shutdown_){", lock);
  ASSERT_NE(GoogleString::npos, check);
  const size_t add = body.find("pending_fetches_.Add(fetch);", check);
  ASSERT_NE(GoogleString::npos, add);
  const size_t write = body.find("event_connection_->WriteEvent(this);", add);
  ASSERT_NE(GoogleString::npos, write);
}

TEST(NgxTeardownPinTest, EventConnectionErrorPathLeavesTheCloseToShutdown) {
  // On a read error the static handler cannot reach the owning object to
  // null connection_, so a close here would be doubled by Shutdown() on a
  // connection object nginx may already have recycled. Shutdown() owns the
  // close; the error path must not close or delete events itself.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_event_connection.cc");
  const size_t fn = cc.find("voidNgxEventConnection::ReadEventHandler(");
  ASSERT_NE(GoogleString::npos, fn) << "ReadEventHandler was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  EXPECT_EQ(GoogleString::npos, body.find("ngx_close_connection("));
  EXPECT_EQ(GoogleString::npos, body.find("ngx_del_event("));
  EXPECT_EQ(GoogleString::npos, body.find("ngx_del_conn("));
}

TEST(NgxTeardownPinTest, WriteEventDropsEventsAfterShutdown) {
  // Shutdown() closes the pipe fds while a rewrite thread that outlived the
  // drain may still hold the kept-alive connection object. Worker-exit
  // teardown reuses the freed fd numbers, so WriteEvent() must refuse the
  // event (write or queue) once Shutdown() has marked the connection
  // closed, and Shutdown() must set that flag before closing the write fd.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_event_connection.cc");
  const size_t fn =
      cc.find("boolNgxEventConnection::WriteEvent(chartype,void*sender){");
  ASSERT_NE(GoogleString::npos, fn) << "WriteEvent was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  const size_t lock = body.find("pthread_mutex_lock(&write_mutex_);");
  ASSERT_NE(GoogleString::npos, lock);
  const size_t closed = body.find("if(closed_){", lock);
  ASSERT_NE(GoogleString::npos, closed);
  // The refusal covers the overflow branch too: a post-close event must not
  // be queued either.
  EXPECT_LT(closed, body.find("if(overflow_mode_){"));

  const size_t shutdown_fn = cc.find("voidNgxEventConnection::Shutdown(){");
  ASSERT_NE(GoogleString::npos, shutdown_fn) << "Shutdown was renamed";
  const size_t shutdown_open = cc.find('{', shutdown_fn);
  const size_t shutdown_close = MatchingBrace(cc, shutdown_open);
  ASSERT_NE(GoogleString::npos, shutdown_close);
  const GoogleString shutdown_body =
      cc.substr(shutdown_open, shutdown_close - shutdown_open);
  const size_t flag = shutdown_body.find("closed_=true;");
  ASSERT_NE(GoogleString::npos, flag);
  EXPECT_LT(flag, shutdown_body.find("close(pipe_write_fd_);"));
}

TEST(NgxTeardownPinTest, EventConnectionShutdownClosesThroughNginx) {
  // The read side is registered with the event loop, so it must be closed
  // through nginx rather than with a bare close() that leaves the loop's
  // bookkeeping stale.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_event_connection.cc");
  const size_t fn = cc.find("voidNgxEventConnection::Shutdown(){");
  ASSERT_NE(GoogleString::npos, fn) << "Shutdown was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  EXPECT_NE(GoogleString::npos,
            body.find("ngx_close_connection(connection_);"));
}

TEST(NgxTeardownPinTest, TerminateDetachesWithoutFreeingTheConnection) {
  // A rewrite thread that outlived the shutdown drain may still hold the
  // event connection pointer: Terminate detaches it with a release store
  // (a later completion loads null and does nothing) but must not free it,
  // or that late WriteEvent would be a use-after-free.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_base_fetch.cc");
  const size_t fn = cc.find("voidNgxBaseFetch::Terminate(){");
  ASSERT_NE(GoogleString::npos, fn) << "Terminate was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  EXPECT_NE(GoogleString::npos,
            body.find("event_connection.store(nullptr,"));
  EXPECT_EQ(GoogleString::npos, body.find("deleteevent_connection;"));
}

TEST(NgxTeardownPinTest, LateRequestCollectionIsANoOp) {
  // After Terminate() a completion must not touch the (null) event
  // connection, and the global must be read exactly once, into a local:
  // reading it again between the null check and the use could observe
  // Terminate()'s nullptr and crash on the call.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_base_fetch.cc");
  const size_t fn = cc.find("voidNgxBaseFetch::RequestCollection(");
  ASSERT_NE(GoogleString::npos, fn) << "RequestCollection was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  const size_t load =
      body.find("event_connection.load(std::memory_order_acquire);");
  ASSERT_NE(GoogleString::npos, load);
  const size_t check = body.find("if(connection==nullptr){", load);
  ASSERT_NE(GoogleString::npos, check);
  EXPECT_NE(GoogleString::npos,
            body.find("connection->WriteEvent(", check));
  // Exactly one read of the global: find == rfind pins a single occurrence
  // of the name ("event_connection" cannot match NgxEventConnection, whose
  // E is capitalized).
  EXPECT_EQ(body.find("event_connection"), body.rfind("event_connection"));
}

TEST(NgxTeardownPinTest, RefusedReloadDeclinesWithoutDereferencing) {
  // A cycle whose server contexts were never child-initialized (a reload
  // with master_process off) must not reach a ProxyFetchFactory use.
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const size_t fn = cc.find("ngx_int_tps_init_module(");
  ASSERT_NE(GoogleString::npos, fn) << "ps_init_module was renamed";
  const size_t open = cc.find('{', fn);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString init_body = cc.substr(open, close - open);
  EXPECT_NE(GoogleString::npos,
            init_body.find("ngx_process==NGX_PROCESS_SINGLE"));

  const size_t handler_fn = cc.find("ngx_int_tps_resource_handler(");
  ASSERT_NE(GoogleString::npos, handler_fn)
      << "ps_resource_handler was renamed";
  const size_t handler_open = cc.find('{', handler_fn);
  const size_t handler_close = MatchingBrace(cc, handler_open);
  ASSERT_NE(GoogleString::npos, handler_close);
  const GoogleString handler_body =
      cc.substr(handler_open, handler_close - handler_open);
  const size_t check =
      handler_body.find("if(cfg_s->proxy_fetch_factory==nullptr){");
  ASSERT_NE(GoogleString::npos, check);
  // And the check must come before the first use of the factory.
  const size_t factory_use =
      handler_body.find("cfg_s->proxy_fetch_factory->");
  ASSERT_NE(GoogleString::npos, factory_use)
      << "the factory use the check guards was moved or renamed";
  EXPECT_LT(check, factory_use);

  // The content handler gates every pagespeed route (beacons and admin
  // paths included) on the context having been child-initialized, so the
  // refused cycle never touches uninitialized statistics, timers or caches.
  const size_t content_fn = cc.find("ngx_int_tps_content_handler(");
  ASSERT_NE(GoogleString::npos, content_fn)
      << "ps_content_handler was renamed";
  const size_t content_open = cc.find('{', content_fn);
  const size_t content_close = MatchingBrace(cc, content_open);
  ASSERT_NE(GoogleString::npos, content_close);
  const GoogleString content_body =
      cc.substr(content_open, content_close - content_open);
  const size_t gate =
      content_body.find("if(!cfg_s->server_context->initialized()){");
  ASSERT_NE(GoogleString::npos, gate);
  const size_t flush = content_body.find("FlushCacheIfNecessary();");
  ASSERT_NE(GoogleString::npos, flush)
      << "the cache-flush poll the gate guards was moved or renamed";
  EXPECT_LT(gate, flush);
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
