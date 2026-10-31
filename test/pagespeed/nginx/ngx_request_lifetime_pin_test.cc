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

TEST(NgxRequestLifetimePinTest, OutputUnderAnActiveUpstreamIsNotFinalized) {
  // While an upstream (proxy_pass and the other *_pass modules) is still
  // delivering the response it owns the request: its handlers are installed,
  // the reference count is its, and its cleanup handler, which drops that
  // reference, is on the cleanup list. The callback therefore (a) samples
  // whether an upstream is active BEFORE the handler runs, because the
  // in-place decline path starts one inside that call and balances its own
  // reference; (b) does not finalize a request the upstream owns when it has
  // only passed output down the chain; and (c) holds the upstream's reference
  // when it ends such a request, so the cleanup handler cannot drop the last
  // one from inside nginx's terminate loop, and then runs the posted requests
  // on every branch, so the terminate handler that ending posts is run even
  // when the client is already gone.
  const GoogleString callback =
      FunctionBody(ReadStrippedSource(kNgxBaseFetchCc), kReadCallback);
  ASSERT_FALSE(callback.empty())
      << "NgxBaseFetch::ReadCallback was renamed; this pin needs updating";
  const size_t sampled = callback.find(
      "constboolupstream_active="
      "r->upstream!=nullptr&&r->upstream->cleanup!=nullptr;");
  const size_t handler =
      callback.find("rc=ps_base_fetch::ps_base_fetch_handler(r);");
  ASSERT_NE(GoogleString::npos, handler)
      << "ReadCallback no longer calls ps_base_fetch_handler";
  EXPECT_NE(GoogleString::npos, sampled)
      << "ReadCallback no longer records whether an upstream owns the request";
  EXPECT_LT(sampled, handler)
      << "the upstream must be sampled before the handler runs: the decline "
         "path starts one inside it";
  const size_t owned = callback.find(
      "if(upstream_active){if(rc==NGX_AGAIN){if(run_posted){"
      "ngx_http_run_posted_requests(c);}return;}r->main->count++;"
      "run_posted=true;}ngx_http_finalize_request(r,rc);");
  EXPECT_NE(GoogleString::npos, owned)
      << "ReadCallback finalizes a request its upstream still owns, ends one "
         "without holding the upstream's reference, or ends one without "
         "running the posted requests afterwards";
  EXPECT_LT(handler, owned);
}

}  // namespace
}  // namespace net_instaweb
