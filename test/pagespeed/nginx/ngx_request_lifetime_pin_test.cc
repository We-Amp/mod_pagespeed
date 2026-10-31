// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Source pins for the nginx request lifetime around the module's event
// callback, NgxBaseFetch::ReadCallback(). The port cannot be linked into a
// unit test (see test/pagespeed/nginx/BUILD), so these read the sources.
// StripWhitespace strips whitespace, not comments, so a pinned token may not
// appear in a comment either.

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
const char kNgxBaseFetchCc[] = "pagespeed/nginx/ngx_base_fetch.cc";

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

// The body of the function whose stripped signature is `signature`, or an
// empty string when the function is not found.
GoogleString FunctionBody(const GoogleString& stripped, const char* signature) {
  const size_t fn = stripped.find(signature);
  if (fn == GoogleString::npos) {
    return GoogleString();
  }
  const size_t open = stripped.find('{', fn);
  const size_t close = MatchingBrace(stripped, open);
  if (close == GoogleString::npos) {
    return GoogleString();
  }
  return stripped.substr(open, close - open);
}

const char kReadCallback[] =
    "voidNgxBaseFetch::ReadCallback(constps_event_data&data){";

TEST(NgxRequestLifetimePinTest, DeclineLeavesThePostedRequestsToItsCaller) {
  // ps_decline_request() hands an in-place lookup miss back to the server by
  // running the remaining phases itself, and returns into the event callback,
  // which still uses the request. The phases may end the request (a client
  // that is gone makes the write fail), in which case nginx posts its
  // terminate handler, which frees the request whatever its count. Running
  // the posted requests before the caller is done with the request would free
  // it under the caller, so the decline leaves them to the caller.
  const GoogleString cc = ReadStrippedSource(kNgxPagespeedCc);
  const GoogleString decline =
      FunctionBody(cc, "ngx_int_tps_decline_request(ngx_http_request_t*r){");
  ASSERT_FALSE(decline.empty())
      << "ps_decline_request was renamed; this pin needs updating";
  EXPECT_NE(GoogleString::npos, decline.find("ngx_http_core_run_phases(r);"))
      << "ps_decline_request no longer runs the remaining phases";
  EXPECT_EQ(GoogleString::npos, decline.find("ngx_http_run_posted_requests("))
      << "ps_decline_request runs the posted requests while its caller "
         "still holds the request";

  // The caller runs them, after it has finalized the request.
  const GoogleString callback =
      FunctionBody(ReadStrippedSource(kNgxBaseFetchCc), kReadCallback);
  ASSERT_FALSE(callback.empty())
      << "NgxBaseFetch::ReadCallback was renamed; this pin needs updating";
  const size_t finalize = callback.rfind("ngx_http_finalize_request(r,rc);");
  const size_t posted = callback.rfind("ngx_http_run_posted_requests(c);");
  ASSERT_NE(GoogleString::npos, finalize)
      << "ReadCallback no longer finalizes the request with the handler's "
         "result";
  ASSERT_NE(GoogleString::npos, posted)
      << "ReadCallback no longer runs the posted requests";
  EXPECT_LT(finalize, posted)
      << "ReadCallback must finalize the request before it runs the posted "
         "requests";
}

}  // namespace
}  // namespace net_instaweb
