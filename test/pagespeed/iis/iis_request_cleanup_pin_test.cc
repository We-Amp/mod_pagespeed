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

// Source pins for the per-request cleanup in the IIS port.
//
// The state OnBeginRequestPageSpeed builds for a request (the module request
// context, its inner context, the parsed URL and the claimed process-context
// reference) is freed by IIS through CleanupStoredContext() when the request
// ends, but only once SetModuleContext() has stored the context. Every early
// return before that handover must free the state itself through the one
// cleanup helper, or the request leaks it. Separately, the final response
// headers handed to the in-place recorder must not be heap-allocated: the
// recorder does not take ownership of them (in_place_resource_recorder.h),
// so a `new` at that call site has no owner and leaks once per recorded
// response. There is no linkable unit test for either site (they sit behind
// the IIS runtime headers), so both are pinned by reading the port source as
// data, the same idiom as iis_recorder_gate_seam_test.cc.
//
// A source pin is weaker than an execution pin and is written knowing that:
// it catches an uncovered early return or a return to heap-allocated
// headers, which are the realistic regressions, and it does not catch a
// cleanup call that runs and does the wrong thing.

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

const char kHttpModuleCc[] = "pagespeed/iis/iis_http_module.cpp";

// The one cleanup call every early return between CreateRequestContext() and
// SetModuleContext() must make, in the whitespace-free form the pins below
// compare against.
const GoogleString kFreeCall = "FreeUnstoredRequestState(ctx,gurl);";

// Reads a source file and removes every whitespace byte, so the result is
// identical no matter where line breaks or indentation land. Stripping
// whitespace strips no comments, so a pinned token may not appear in a
// comment either.
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

class IisRequestCleanupPinTest : public testing::Test {};

TEST_F(IisRequestCleanupPinTest,
       EveryEarlyReturnBetweenCreationAndStorageFreesTheRequestState) {
  const GoogleString cc = ReadStrippedSource(kHttpModuleCc);
  const size_t on_begin = cc.find("IisHttpModule::OnBeginRequestPageSpeed(");
  ASSERT_NE(GoogleString::npos, on_begin)
      << "OnBeginRequestPageSpeed was renamed; this pin needs updating";
  const size_t open = cc.find('{', on_begin);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);

  // The window this pin guards: from building the request context to
  // handing it to IIS. IIS frees a stored context at request end; before
  // the handover, nothing else does.
  const size_t created = body.find("CreateRequestContext(");
  ASSERT_NE(GoogleString::npos, created)
      << "the request context is no longer built through "
         "CreateRequestContext";
  const size_t stored = body.find("SetModuleContext(", created);
  ASSERT_NE(GoogleString::npos, stored)
      << "the request context is no longer stored through SetModuleContext";

  // Every return in the window must free through the one cleanup call, as
  // the last statement before it: after the free nothing may touch the
  // context or the URL. `returnRQ_NOTIFICATION_` is the return statement of
  // this function in the whitespace-free form; matching it rather than the
  // bare keyword keeps a "return" inside a log string or a comment from
  // reading as a statement. On the pre-fix source none of the returns made
  // the call, so this pin goes red there.
  const GoogleString window = body.substr(created, stored - created);
  int returns = 0;
  size_t at = window.find("returnRQ_NOTIFICATION_");
  while (at != GoogleString::npos) {
    ++returns;
    const bool covered =
        at >= kFreeCall.size() &&
        window.compare(at - kFreeCall.size(), kFreeCall.size(), kFreeCall) ==
            0;
    EXPECT_TRUE(covered)
        << "a return between CreateRequestContext and SetModuleContext is "
           "not preceded by "
        << kFreeCall << "; the request state leaks on that path";
    at = window.find("returnRQ_NOTIFICATION_", at + 1);
  }
  // Fail closed: a window that silently matched nothing would make the loop
  // above vacuous. Four returns sit between the two calls today: the
  // non-local bail right after creation, the init-failure page (whose
  // context creation failed), the non-beacon POST bail and the
  // too-many-outstanding-rewrites bail.
  EXPECT_GE(returns, 3)
      << "only " << returns
      << " returns found between CreateRequestContext and SetModuleContext; "
         "the window extraction looks wrong, so this pin proved nothing";
}

TEST_F(IisRequestCleanupPinTest, TheFreePathIsTheStoredContextsCleanup) {
  const GoogleString cc = ReadStrippedSource(kHttpModuleCc);
  // The one helper the early returns go through. A built request context
  // must free through CleanupStoredContext(), the very callback IIS runs
  // for a stored context, so both paths share one free sequence (and with
  // it the inner context, the parsed URL and the claimed process-context
  // reference). When no context was built the parsed URL is still the
  // caller's, because CreateRequestContext() takes it over only on success.
  const size_t helper =
      cc.find("staticvoidFreeUnstoredRequestState(IisModuleRequestContext*"
              "ctx,GoogleUrl*gurl)");
  ASSERT_NE(GoogleString::npos, helper)
      << "FreeUnstoredRequestState was renamed; this pin needs updating";
  const size_t open = cc.find('{', helper);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  EXPECT_NE(GoogleString::npos, body.find("ctx->CleanupStoredContext();"))
      << "the helper no longer frees the request context through the "
         "stored-context cleanup callback";
  EXPECT_NE(GoogleString::npos, body.find("deletegurl;"))
      << "the helper no longer frees the parsed URL when no request context "
         "was built";
}

TEST_F(IisRequestCleanupPinTest, TheRecordersFinalHeadersAreNotHeapAllocated) {
  const GoogleString cc = ReadStrippedSource(kHttpModuleCc);
  const size_t on_send = cc.find("IisHttpModule::OnSendResponse(");
  ASSERT_NE(GoogleString::npos, on_send)
      << "OnSendResponse was renamed; this pin needs updating";
  const size_t open = cc.find('{', on_send);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  // The recorder reads the headers but does not delete them, so a heap
  // allocation at this call site has no owner. The headers must live on the
  // stack, exactly like the ones offered to ConsiderResponseHeaders earlier
  // in the same function. On the pre-fix source the final call was fed
  // `new ResponseHeaders()`, so this pin goes red there.
  EXPECT_EQ(GoogleString::npos, body.find("newResponseHeaders"))
      << "a heap-allocated ResponseHeaders in OnSendResponse has no owner: "
         "the recorder reads it but does not take ownership of it";
  EXPECT_NE(GoogleString::npos,
            body.find("DoneAndSetHeaders(&response_headers,true)"))
      << "the final DoneAndSetHeaders no longer feeds a stack-allocated "
         "headers object";
}

}  // namespace
}  // namespace net_instaweb
