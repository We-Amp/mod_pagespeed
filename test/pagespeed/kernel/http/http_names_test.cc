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

#include "pagespeed/kernel/http/http_names.h"

#include <cstddef>  // for size_t
#include <iterator>

#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

struct ReasonPhraseCase {
  HttpStatus::Code code;
  const char* expected;
};

}  // namespace

TEST(HttpStatusTest, GetReasonPhraseKnownCodes) {
  static const ReasonPhraseCase kCases[] = {
      {HttpStatus::kContinue, "Continue"},
      {HttpStatus::kSwitchingProtocols, "Switching Protocols"},
      {HttpStatus::kOK, "OK"},
      {HttpStatus::kCreated, "Created"},
      {HttpStatus::kAccepted, "Accepted"},
      {HttpStatus::kNonAuthoritative, "Non-Authoritative Information"},
      {HttpStatus::kNoContent, "No Content"},
      {HttpStatus::kResetContent, "Reset Content"},
      {HttpStatus::kPartialContent, "Partial Content"},
      {HttpStatus::kMultipleChoices, "Multiple Choices"},
      {HttpStatus::kMovedPermanently, "Moved Permanently"},
      {HttpStatus::kFound, "Found"},
      {HttpStatus::kSeeOther, "See Other"},
      {HttpStatus::kNotModified, "Not Modified"},
      {HttpStatus::kUseProxy, "Use Proxy"},
      {HttpStatus::kBadRequest, "Bad Request"},
      {HttpStatus::kUnauthorized, "Unauthorized"},
      {HttpStatus::kPaymentRequired, "Payment Required"},
      {HttpStatus::kForbidden, "Forbidden"},
      {HttpStatus::kNotFound, "Not Found"},
      {HttpStatus::kMethodNotAllowed, "Method Not Allowed"},
      {HttpStatus::kNotAcceptable, "Not Acceptable"},
      {HttpStatus::kProxyAuthRequired, "Proxy Authentication Required"},
      {HttpStatus::kRequestTimeout, "Request Time-out"},
      {HttpStatus::kConflict, "Conflict"},
      {HttpStatus::kGone, "Gone"},
      {HttpStatus::kLengthRequired, "Length Required"},
      {HttpStatus::kPreconditionFailed, "Precondition Failed"},
      {HttpStatus::kEntityTooLarge, "Request Entity Too Large"},
      {HttpStatus::kUriTooLong, "Request-URI Too Large"},
      {HttpStatus::kUnsupportedMediaType, "Unsupported Media Type"},
      {HttpStatus::kRangeNotSatisfiable, "Requested range not satisfiable"},
      {HttpStatus::kExpectationFailed, "Expectation Failed"},
      {HttpStatus::kInternalServerError, "Internal Server Error"},
      {HttpStatus::kNotImplemented, "Not Implemented"},
      {HttpStatus::kBadGateway, "Bad Gateway"},
      {HttpStatus::kUnavailable, "Service Unavailable"},
      {HttpStatus::kGatewayTimeout, "Gateway Time-out"},
      {HttpStatus::kProxyPublisherFailure, "Proxy Publisher Failure"},
      {HttpStatus::kProxyFailure, "Proxy Failure"},
      {HttpStatus::kProxyConfigurationFailure, "Proxy Config Failure"},
      {HttpStatus::kProxyDeclinedRequest, "Proxy Declined Request"},
  };
  for (const ReasonPhraseCase& test_case : kCases) {
    EXPECT_STREQ(test_case.expected,
                 HttpStatus::GetReasonPhrase(test_case.code))
        << "reason phrase for status " << static_cast<int>(test_case.code);
  }
}

TEST(HttpStatusTest, GetReasonPhraseUnknownCode) {
  // A code with no phrase in the table is attributed to the server itself.
  EXPECT_STREQ("Internal Server Error",
               HttpStatus::GetReasonPhrase(static_cast<HttpStatus::Code>(599)));
  EXPECT_STREQ("Internal Server Error",
               HttpStatus::GetReasonPhrase(HttpStatus::kUnknownStatusCode));
}

// The hop-by-hop table is binary-searched case-insensitively (header
// sanitization removes these names from responses), so it must hold the
// expected names in sort order: an inversion makes the misplaced entries
// unfindable.
TEST(HttpAttributesTest, SortedHopByHopHeaders) {
  const StringPieceVector& names = HttpAttributes::SortedHopByHopHeaders();
  static const char* const kExpected[] = {
      HttpAttributes::kAltSvc,
      HttpAttributes::kAlternateProtocol,
      HttpAttributes::kConnection,
      HttpAttributes::kKeepAlive,
      HttpAttributes::kProxyAuthenticate,
      HttpAttributes::kProxyAuthorization,
      HttpAttributes::kSetCookie,
      HttpAttributes::kSetCookie2,
      HttpAttributes::kTE,
      HttpAttributes::kTrailers,
      HttpAttributes::kTransferEncoding,
      HttpAttributes::kUpgrade,
  };
  ASSERT_EQ(std::size(kExpected), names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    EXPECT_EQ(GoogleString(kExpected[i]), names[i].as_string())
        << "hop-by-hop entry " << i;
  }
  StringCompareInsensitive compare;
  for (size_t i = 1; i < names.size(); ++i) {
    EXPECT_FALSE(compare(names[i], names[i - 1]))
        << names[i - 1] << " must not be listed after " << names[i];
  }
}

// Unlike the hop-by-hop table, this list carries no sort-order contract; it
// just has to hold the caching headers that get removed, each exactly once.
TEST(HttpAttributesTest, CachingHeadersToBeRemoved) {
  const StringPieceVector& names = HttpAttributes::CachingHeadersToBeRemoved();
  static const char* const kExpected[] = {
      HttpAttributes::kLastModified,
      HttpAttributes::kExpires,
      HttpAttributes::kEtag,
  };
  ASSERT_EQ(std::size(kExpected), names.size());
  for (const char* expected : kExpected) {
    int occurrences = 0;
    for (const StringPiece& name : names) {
      if (name == expected) {
        ++occurrences;
      }
    }
    EXPECT_EQ(1, occurrences) << expected << " must appear exactly once";
  }
}

TEST(HttpStatusTest, ReasonPhrases) {
  EXPECT_STREQ("OK", HttpStatus::GetReasonPhrase(HttpStatus::kOK));
  EXPECT_STREQ("Not Found", HttpStatus::GetReasonPhrase(HttpStatus::kNotFound));
}

TEST(HttpStatusTest, RedirectReasonPhrases) {
  EXPECT_STREQ("Moved Permanently",
               HttpStatus::GetReasonPhrase(HttpStatus::kMovedPermanently));
  // 307 used to fall back to "OK".
  EXPECT_STREQ("Temporary Redirect",
               HttpStatus::GetReasonPhrase(HttpStatus::kTemporaryRedirect));
}

TEST(HttpStatusTest, ServerErrorReasonPhrases) {
  EXPECT_STREQ("Service Unavailable",
               HttpStatus::GetReasonPhrase(HttpStatus::kUnavailable));
  EXPECT_STREQ("Gateway Time-out",
               HttpStatus::GetReasonPhrase(HttpStatus::kGatewayTimeout));
  // 505 used to fall through to "Internal Server Error".
  EXPECT_STREQ(
      "HTTP Version Not Supported",
      HttpStatus::GetReasonPhrase(HttpStatus::kHttpVersionNotSupported));
}

}  // namespace net_instaweb
