// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The RequestOptionOverride token in responses on IIS.
//
// An origin may send the token in a response header, beside its own option
// headers, to have those honoured.  The token is a credential and must never
// reach the client.  The IIS port does not read the token from a response, so
// it removes both header names from every response it sees, before anything
// copies, records or sends the response headers.  Kept free of IIS/Windows
// headers (the pattern of iis_zerocopy_serve.h) so it is unit-testable on
// every platform.

#ifndef PAGESPEED_IIS_IIS_OPTION_TOKEN_H_
#define PAGESPEED_IIS_IIS_OPTION_TOKEN_H_

#include <cstddef>

namespace net_instaweb {

// RewriteQuery::kModPagespeed and RewriteQuery::kPageSpeed, each followed by
// RewriteOptions::kRequestOptionOverride (a unit test keeps them in step).
inline constexpr const char* kIisOptionOverrideTokenHeaders[] = {
    "ModPagespeedRequestOptionOverride",
    "PageSpeedRequestOptionOverride",
};

// Upper bound on the copies of one header name removed from a response; a
// response with more is malformed beyond what any origin sends.
inline constexpr int kIisOptionOverrideTokenMaxCopies = 16;

// Removes every copy of both token header names from `response`.  Response
// is IHttpResponse in the module (GetHeader and DeleteHeader compare names
// without regard to case) and a fake in tests.  DeleteHeader returns an
// HRESULT, negative on failure.
template <class Response>
void RemoveOptionOverrideToken(Response* response) {
  if (response == nullptr) {
    return;
  }
  for (const char* name : kIisOptionOverrideTokenHeaders) {
    for (int i = 0; i < kIisOptionOverrideTokenMaxCopies; ++i) {
      if (response->GetHeader(name) == nullptr ||
          response->DeleteHeader(name) < 0) {
        break;
      }
    }
  }
}

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_OPTION_TOKEN_H_
